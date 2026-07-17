#pragma once

#include <charconv>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "cpptb/coro_runtime.hpp"
#include "cpptb/diagnostic.hpp"
#include "cpptb/randomized.hpp"
#include "cpptb/test_result.hpp"

namespace cpptb {

class TestContext;

struct RunRequest {
    std::string_view test_name;
    bool list_only = false;
    std::optional<coro::SimTime> simulation_timeout;
    std::optional<uint64_t> random_seed;
    std::string_view configuration_error;
};

struct TestOptions {
    std::vector<std::string_view> tags;
    bool expected_failure = false;
    std::string_view expected_failure_reason;
    std::string_view skip_reason;
    std::optional<coro::SimTime> simulation_timeout;
};

struct TestMetadata {
    std::string base_name;
    std::string case_name;
    std::vector<std::string> tags;
    bool expected_failure = false;
    std::string expected_failure_reason;
    std::string skip_reason;
    std::optional<coro::SimTime> simulation_timeout;
};

namespace detail {

template <typename Value>
void print_diagnostic_integral(FILE* stream, Value value) {
    const auto normalized = [&] {
        if constexpr (std::is_enum_v<Value>) {
            return static_cast<std::underlying_type_t<Value>>(value);
        } else {
            return value;
        }
    }();
    if constexpr (std::is_same_v<std::remove_cv_t<decltype(normalized)>, bool>) {
        std::fputs(normalized ? "true" : "false", stream);
    } else if constexpr (std::is_signed_v<decltype(normalized)>) {
        std::fprintf(stream, "%lld", static_cast<long long>(normalized));
    } else {
        std::fprintf(stream, "%llu",
                     static_cast<unsigned long long>(normalized));
    }
}

inline bool environment_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] != '\0' && std::string_view{value} != "0" &&
           std::string_view{value} != "false";
}

inline bool parse_random_seed(std::string_view text, uint64_t& value) {
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty()) return false;

    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value, base);
    return result.ec == std::errc{} &&
           result.ptr == text.data() + text.size();
}

inline RunRequest environment_run_request() {
    const char* selected = std::getenv("CPPTB_TEST");
    const char* seed_text = std::getenv("CPPTB_RANDOM_SEED");
    std::optional<uint64_t> random_seed;
    std::string_view configuration_error;
    if (seed_text && seed_text[0] != '\0') {
        uint64_t seed = 0;
        if (parse_random_seed(seed_text, seed)) {
            random_seed = seed;
        } else {
            configuration_error =
                "CPPTB_RANDOM_SEED must be a decimal or 0x-prefixed "
                "unsigned 64-bit integer";
        }
    }
    return RunRequest{
        .test_name = selected ? std::string_view{selected} : std::string_view{},
        .list_only = environment_enabled("CPPTB_LIST_TESTS"),
        .random_seed = random_seed,
        .configuration_error = configuration_error,
    };
}

class AbortTest final {};

class SkipTest final {
   public:
    SkipTest(std::string reason, std::source_location location)
        : reason(std::move(reason)), location(location) {}

    std::string reason;
    std::source_location location;
};

class TestRunState;

struct ProcessProvenance {
    TestRunState* owner = nullptr;
    ProcessProvenance* next_free = nullptr;
    uint64_t id = 0;
    std::string_view description;
    const char* source_file = "";
    uint32_t source_line = 0;
    Random random;
};

class TestRunState {
   public:
    TestRunState(coro::Testbench& scheduler, TestResult& result,
                 coro::ClockRegistrar clocks, ResultSink* sink = nullptr)
        : scheduler_(&scheduler), result_(&result), clocks_(clocks), sink_(sink) {}

    void retain() noexcept { ++references_; }

    void release() noexcept {
        if (--references_ == 0) delete this;
    }

    void begin(std::string_view name, const TestMetadata& metadata = {},
               uint64_t random_seed = kDefaultRandomSeed) {
        result_->checks = 0;
        result_->failures = 0;
        result_->warnings = 0;
        result_->status = TestStatus::Running;
        result_->test_name.assign(name);
        result_->case_name = metadata.case_name;
        result_->status_reason.clear();
        result_->tags = metadata.tags;
        result_->failure_records.clear();
        result_->warning_records.clear();
        result_->random_seed = random_seed;
        result_->random_algorithm = kRandomAlgorithm;
        result_->constraint_backend.clear();
        result_->constraint_backend_version.clear();
        result_->random_sampling_solves = 0;
        result_->random_solver_solves = 0;
        result_->simulation_time_fs = scheduler_->now().femtoseconds;
        result_->wall_time_ns = 0;
        result_->finished = false;
        expected_failure_ = metadata.expected_failure;
        expected_failure_reason_ = metadata.expected_failure_reason;
        master_random_seed_ = random_seed;
        fallback_random_.reseed(random_seed);
        if (sink_) sink_->test_started(*result_);
    }

    ProcessProvenance* create_process(
        std::string_view description,
        std::source_location location = std::source_location::current()) {
        retain();
        ProcessProvenance* provenance = nullptr;
        if (!free_process_provenance_) {
            process_provenance_.push_back(ProcessProvenance{});
            provenance = &process_provenance_.back();
        } else {
            provenance = free_process_provenance_;
            free_process_provenance_ = provenance->next_free;
        }
        const uint64_t process_id = next_process_id_++;
        *provenance = ProcessProvenance{
            .owner = this,
            .id = process_id,
            .description = description,
            .source_file = location.file_name(),
            .source_line = location.line(),
            .random = Random{random_detail::derive_seed(master_random_seed_,
                                                        process_id)},
        };
        return provenance;
    }

    void release_process(ProcessProvenance* provenance) noexcept {
        if (!provenance || provenance->owner != this) return;
        provenance->owner = nullptr;
        provenance->next_free = free_process_provenance_;
        free_process_provenance_ = provenance;
        release();
    }

    void record_check() {
        if (!result_->finished) ++result_->checks;
    }

    void record_failure(FailureRecord record) {
        if (result_->finished) return;
        ++result_->failures;
        result_->status = TestStatus::Failed;
        record.simulation_time_fs = scheduler_->now().femtoseconds;
        attach_current_process(record);
        result_->failure_records.push_back(std::move(record));
        const auto& stored = result_->failure_records.back();
        if (sink_) sink_->failure_recorded(stored);

        if (!stored.source_file.empty()) {
            std::fprintf(stderr, "cpptb: %s:%u: ", stored.source_file.c_str(),
                         stored.source_line);
        } else {
            std::fputs("cpptb: ", stderr);
        }
        std::fprintf(stderr, "%s", stored.label.c_str());
        if (stored.has_comparison) {
            std::fprintf(stderr, ": actual=%s expected=%s",
                         stored.actual.c_str(), stored.expected.c_str());
        }
        std::fputc('\n', stderr);
    }

    void record_warning(std::string_view message,
                        std::source_location location) {
        if (result_->finished) return;
        ++result_->warnings;
        WarningRecord record{
            .label = std::string{message},
            .source_file = std::string{location.file_name()},
            .source_line = location.line(),
        };
        record.simulation_time_fs = scheduler_->now().femtoseconds;
        attach_current_process(record);
        result_->warning_records.push_back(std::move(record));
        const auto& stored = result_->warning_records.back();
        if (sink_) sink_->warning_recorded(stored);
        std::fprintf(stderr, "cpptb: %s:%u: warning: %s\n",
                     stored.source_file.c_str(), stored.source_line,
                     stored.label.c_str());
    }

    void record_exception(std::string_view message,
                          std::source_location location) {
        if (result_->finished) return;
        FailureRecord record{
            .kind = FailureKind::Exception,
            .label = std::string{message},
            .source_file = std::string{location.file_name()},
            .source_line = location.line(),
        };
        ++result_->failures;
        result_->status = TestStatus::Error;
        record.simulation_time_fs = scheduler_->now().femtoseconds;
        attach_current_process(record);
        result_->failure_records.push_back(std::move(record));
        const auto& stored = result_->failure_records.back();
        if (sink_) sink_->failure_recorded(stored);
        if (!stored.source_file.empty()) {
            std::fprintf(stderr, "cpptb: %s:%u: ", stored.source_file.c_str(),
                         stored.source_line);
        } else {
            std::fputs("cpptb: ", stderr);
        }
        std::fprintf(stderr, "exception in %s: %s\n", stored.process.c_str(),
                     stored.label.c_str());
        finish(TestStatus::Error);
    }

    void record_skip(std::string_view reason,
                     std::source_location location) {
        if (result_->finished) return;
        result_->status_reason.assign(reason);
        std::fprintf(stderr, "cpptb: %s:%u: skipped: %.*s\n",
                     location.file_name(), location.line(),
                     static_cast<int>(reason.size()), reason.data());
        finish(TestStatus::Skipped);
    }

    void record_timeout(coro::SimTime timeout,
                        std::source_location location) {
        if (result_->finished) return;
        result_->status_reason = "simulation-time limit of " +
                                 std::to_string(timeout.femtoseconds) + " fs";
        record_failure(FailureRecord{
            .kind = FailureKind::Timeout,
            .label = result_->status_reason,
            .source_file = std::string{location.file_name()},
            .source_line = location.line(),
        });
        finish(TestStatus::TimedOut);
    }

    void finish(TestStatus requested) {
        if (result_->finished) return;
        if ((requested == TestStatus::Passed ||
             requested == TestStatus::Failed) &&
            expected_failure_) {
            if (result_->failures != 0) {
                requested = TestStatus::ExpectedFailure;
                result_->status_reason = expected_failure_reason_;
            } else {
                const std::string label = expected_failure_reason_.empty()
                                              ? "test was expected to fail but passed"
                                              : "test was expected to fail but passed: " +
                                                    expected_failure_reason_;
                record_failure(FailureRecord{
                    .kind = FailureKind::UnexpectedPass,
                    .label = label,
                });
                result_->status_reason = expected_failure_reason_;
                requested = TestStatus::UnexpectedPass;
            }
        } else if (requested == TestStatus::Passed && result_->failures != 0) {
            requested = result_->status == TestStatus::Error
                            ? TestStatus::Error
                            : TestStatus::Failed;
        }
        result_->status = requested;
        result_->simulation_time_fs = scheduler_->now().femtoseconds;
        result_->finished = true;
        cancel_owned_processes();
        if (sink_) sink_->test_finished(*result_);
    }

    void cancel_owned_processes() {
        scheduler_->cancel_processes_owned_by(
            this, [](void* process_owner, void* test_owner) noexcept {
                auto* provenance =
                    static_cast<ProcessProvenance*>(process_owner);
                return provenance && provenance->owner == test_owner;
            });
    }

    bool finished() const { return result_->finished; }
    coro::Testbench& scheduler() const { return *scheduler_; }
    TestResult& result() const { return *result_; }
    coro::ClockRegistrar clocks() const { return clocks_; }

    Random& current_random() {
        auto* process = static_cast<ProcessProvenance*>(
            scheduler_->current_execution_context());
        if (process && process->owner == this) return process->random;
        return fallback_random_;
    }

    ConstraintBackend& random_backend() const { return *random_backend_; }
    void set_random_backend(ConstraintBackend& backend) {
        random_backend_ = &backend;
    }

    void record_randomization(const RandomizeResult& randomization,
                              const ConstraintBackend& backend) {
        const auto record_text = [](std::string& destination,
                                    std::string_view value) {
            if (value.empty()) return;
            if (destination.empty()) {
                destination.assign(value);
            } else if (destination != value) {
                destination = "mixed";
            }
        };
        record_text(result_->constraint_backend, backend.name());
        record_text(result_->constraint_backend_version, backend.version());
        if (randomization.engine == RandomizeEngine::Sampling) {
            ++result_->random_sampling_solves;
        } else if (randomization.engine == RandomizeEngine::Solver) {
            ++result_->random_solver_solves;
        }
    }

   private:
    template <typename Record>
    void attach_current_process(Record& record) const {
        auto* process = static_cast<ProcessProvenance*>(
            scheduler_->current_execution_context());
        if (!process || process->owner != this) return;
        record.process.assign(process->description);
        record.process_source_file.assign(process->source_file);
        record.process_id = process->id;
        record.process_source_line = process->source_line;
    }

    coro::Testbench* scheduler_;
    TestResult* result_;
    coro::ClockRegistrar clocks_;
    ResultSink* sink_;
    std::deque<ProcessProvenance> process_provenance_;
    ProcessProvenance* free_process_provenance_ = nullptr;
    uint64_t next_process_id_ = 1;
    uint64_t master_random_seed_ = kDefaultRandomSeed;
    Random fallback_random_;
    ConstraintBackend* random_backend_ = &default_constraint_backend();
    bool expected_failure_ = false;
    std::string expected_failure_reason_;
    size_t references_ = 1;
};

class TestRunStatePtr {
   public:
    TestRunStatePtr() = default;

    explicit TestRunStatePtr(TestRunState* state) noexcept : state_(state) {}

    TestRunStatePtr(const TestRunStatePtr& other) noexcept
        : state_(other.state_) {
        if (state_) state_->retain();
    }

    TestRunStatePtr& operator=(const TestRunStatePtr& other) noexcept {
        if (this == &other) return *this;
        if (other.state_) other.state_->retain();
        reset();
        state_ = other.state_;
        return *this;
    }

    TestRunStatePtr(TestRunStatePtr&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)) {}

    TestRunStatePtr& operator=(TestRunStatePtr&& other) noexcept {
        if (this == &other) return *this;
        reset();
        state_ = std::exchange(other.state_, nullptr);
        return *this;
    }

    ~TestRunStatePtr() { reset(); }

    TestRunState* get() const noexcept { return state_; }
    TestRunState& operator*() const noexcept { return *state_; }
    TestRunState* operator->() const noexcept { return state_; }

   private:
    void reset() noexcept {
        if (state_) state_->release();
        state_ = nullptr;
    }

    TestRunState* state_ = nullptr;
};

template <typename Dut>
using TestFunction = coro::Task<void> (*)(Dut, ::cpptb::TestContext&);

void finish_owned_process(void* owner, coro::ExceptionState& exception,
                          bool cancelled) noexcept;

}  // namespace detail

class TestContext {
   public:
    TestContext(coro::Testbench& scheduler, TestResult& result,
                coro::ClockRegistrar clocks = {})
        : state_(new detail::TestRunState(scheduler, result, clocks)) {}

    template <typename Actual, typename Expected>
        requires requires(const Actual& actual, const Expected& expected) {
            { actual == expected } -> std::convertible_to<bool>;
        }
    void expect_eq(
        std::string_view label, const Actual& actual, const Expected& expected,
        std::source_location location = std::source_location::current()) const {
        state_->record_check();
        if (actual == expected) return;
        state_->record_failure(comparison_failure(
            FailureKind::Expectation, label, actual, expected, location));
    }

    void expect(
        std::string_view label, bool condition,
        std::source_location location = std::source_location::current()) const {
        state_->record_check();
        if (condition) return;
        state_->record_failure(boolean_failure(FailureKind::Expectation, label,
                                               location));
    }

    template <typename Actual, typename Expected>
        requires requires(const Actual& actual, const Expected& expected) {
            { actual == expected } -> std::convertible_to<bool>;
        }
    void require_eq(
        std::string_view label, const Actual& actual, const Expected& expected,
        std::source_location location = std::source_location::current()) const {
        state_->record_check();
        if (actual == expected) return;
        state_->record_failure(comparison_failure(
            FailureKind::Requirement, label, actual, expected, location));
        throw detail::AbortTest{};
    }

    void require(
        std::string_view label, bool condition,
        std::source_location location = std::source_location::current()) const {
        state_->record_check();
        if (condition) return;
        state_->record_failure(boolean_failure(FailureKind::Requirement, label,
                                               location));
        throw detail::AbortTest{};
    }

    void warn(
        std::string_view message,
        std::source_location location = std::source_location::current()) const {
        state_->record_warning(message, location);
    }

    [[noreturn]] void skip(
        std::string_view reason,
        std::source_location location = std::source_location::current()) const {
        throw detail::SkipTest{std::string{reason}, location};
    }

    coro::Process spawn(
        coro::Task<void> task,
        std::source_location location = std::source_location::current()) const {
        auto* provenance = state_->create_process("spawned process", location);
        auto process = state_->scheduler().spawn(
            std::move(task), static_cast<void*>(provenance),
            coro::ProcessCompletionHandler{
                provenance, detail::finish_owned_process});
        if (state_->finished() && process.valid() && !process.done()) {
            process.cancel();
        }
        if (state_->finished()) throw detail::AbortTest{};
        return process;
    }

    void spawn_detached(
        coro::Task<void> task,
        std::source_location location = std::source_location::current()) const {
        static_cast<void>(spawn(std::move(task), location));
    }

    template <typename Signal>
        requires std::convertible_to<Signal, coro::Signal>
    void start_clock(Signal signal, coro::SimTime period,
                     coro::SimTime phase = {}) const {
        state_->clocks().start(static_cast<coro::Signal>(signal), period, phase);
    }

    coro::SimTime now() const { return state_->scheduler().now(); }

    Random& random() const { return state_->current_random(); }

    ConstraintBackend& random_backend() const {
        return state_->random_backend();
    }

    void set_random_backend(ConstraintBackend& backend) const {
        state_->set_random_backend(backend);
    }

    void randomize(
        Randomized& item,
        std::source_location location = std::source_location::current()) const {
        const auto result = item.randomize(random(), random_backend());
        state_->record_randomization(result, random_backend());
        require_randomized(result, random_backend(), location);
    }

    void randomize_with(
        Randomized& item, Constraint constraint,
        std::source_location location = std::source_location::current()) const {
        const auto result = item.randomize_with(
            random(), std::move(constraint), random_backend());
        state_->record_randomization(result, random_backend());
        require_randomized(result, random_backend(), location);
    }

    explicit TestContext(detail::TestRunStatePtr state)
        : state_(std::move(state)) {}

   private:
    void require_randomized(const RandomizeResult& result,
                            const ConstraintBackend& backend,
                            std::source_location location) const {
        if (result) return;
        std::string message = "randomize() failed using '" +
                              std::string{backend.name()} + "'";
        if (!result.message.empty()) message += ": " + result.message;
        require(message, false, location);
    }

    static FailureRecord boolean_failure(FailureKind kind,
                                         std::string_view label,
                                         std::source_location location) {
        return FailureRecord{
            .kind = kind,
            .label = std::string{label},
            .source_file = std::string{location.file_name()},
            .source_line = location.line(),
        };
    }

    template <typename Actual, typename Expected>
    static FailureRecord comparison_failure(
        FailureKind kind, std::string_view label, const Actual& actual,
        const Expected& expected, std::source_location location) {
        auto actual_text = format_diagnostic(actual);
        auto expected_text = format_diagnostic(expected);
        const bool has_comparison = actual_text.has_value() &&
                                    expected_text.has_value();
        return FailureRecord{
            .kind = kind,
            .label = std::string{label},
            .actual = actual_text ? std::move(*actual_text) : std::string{},
            .expected = expected_text ? std::move(*expected_text) : std::string{},
            .source_file = std::string{location.file_name()},
            .source_line = location.line(),
            .has_comparison = has_comparison,
        };
    }

    detail::TestRunStatePtr state_;
};

namespace detail {

inline void finish_owned_process(void* owner, coro::ExceptionState& exception,
                                 bool cancelled) noexcept {
    auto* provenance = static_cast<ProcessProvenance*>(owner);
    auto& state = *provenance->owner;
    if (!cancelled && exception) {
        try {
            exception.rethrow();
        } catch (const AbortTest&) {
            state.finish(TestStatus::Failed);
        } catch (const SkipTest& skip) {
            state.record_skip(skip.reason, skip.location);
        } catch (const std::exception& error) {
            state.record_exception(error.what(),
                                   std::source_location::current());
        } catch (...) {
            state.record_exception("unknown exception",
                                   std::source_location::current());
        }
    }
    state.release_process(provenance);
}

template <typename Dut>
using TestInvoker =
    std::function<coro::Task<void>(Dut, ::cpptb::TestContext&)>;

inline TestMetadata make_test_metadata(std::string_view base_name,
                                       std::string_view case_name,
                                       const TestOptions& options) {
    TestMetadata metadata{
        .base_name = std::string{base_name},
        .case_name = std::string{case_name},
        .expected_failure = options.expected_failure,
        .expected_failure_reason =
            std::string{options.expected_failure_reason},
        .skip_reason = std::string{options.skip_reason},
        .simulation_timeout = options.simulation_timeout,
    };
    metadata.tags.reserve(options.tags.size());
    for (const auto tag : options.tags) metadata.tags.emplace_back(tag);
    return metadata;
}

template <typename Dut>
struct TestDescriptor {
    std::string name;
    TestInvoker<Dut> function;
    TestMetadata metadata;
    std::source_location declaration;
};

template <typename Dut>
std::vector<TestDescriptor<Dut>>& test_registry() {
    static std::vector<TestDescriptor<Dut>> entries;
    return entries;
}

template <typename Dut>
class TestRegistration {
   public:
    TestRegistration(std::string name, TestInvoker<Dut> function,
                     TestMetadata metadata,
                     std::source_location declaration) {
        test_registry<Dut>().push_back(
            {std::move(name), std::move(function), std::move(metadata),
             declaration});
    }
};

template <typename Dut>
coro::Task<void> invoke_registered_test(
    TestInvoker<Dut> function, Dut dut,
    TestRunStatePtr state,
    std::source_location declaration,
    std::optional<coro::SimTime> simulation_timeout) {
    TestContext context{state};
    try {
        if (simulation_timeout) {
            const auto outcome = co_await coro::with_timeout(
                function(dut, context), *simulation_timeout);
            if (outcome.timed_out()) {
                state->record_timeout(*simulation_timeout, declaration);
                co_return;
            }
        } else {
            co_await function(dut, context);
        }
        state->finish(TestStatus::Passed);
    } catch (const AbortTest&) {
        state->finish(TestStatus::Failed);
    } catch (const SkipTest& skip) {
        state->record_skip(skip.reason, skip.location);
    } catch (const std::exception& error) {
        state->record_exception(error.what(), declaration);
    } catch (...) {
        state->record_exception("unknown exception", declaration);
    }
}

inline void reset_selection_result(TestResult& result) {
    result.checks = 0;
    result.failures = 0;
    result.warnings = 0;
    result.status = TestStatus::NotRun;
    result.test_name.clear();
    result.case_name.clear();
    result.status_reason.clear();
    result.tags.clear();
    result.failure_records.clear();
    result.warning_records.clear();
    result.random_seed.reset();
    result.random_algorithm.clear();
    result.constraint_backend.clear();
    result.constraint_backend_version.clear();
    result.random_sampling_solves = 0;
    result.random_solver_solves = 0;
    result.simulation_time_fs = 0;
    result.wall_time_ns = 0;
    result.finished = false;
}

inline void record_selection_error(TestResult& result,
                                   std::string message,
                                   ResultSink* sink) {
    ++result.failures;
    result.status = TestStatus::Error;
    result.finished = true;
    result.failure_records.push_back(FailureRecord{
        .kind = FailureKind::Selection,
        .label = std::move(message),
    });
    if (sink) {
        sink->failure_recorded(result.failure_records.back());
        sink->test_finished(result);
    }
}

}  // namespace detail

template <typename Dut>
detail::TestRegistration<Dut> register_test(
    const char* name, detail::TestFunction<Dut> function,
    TestOptions options = {},
    std::source_location declaration = std::source_location::current()) {
    return detail::TestRegistration<Dut>{
        std::string{name}, detail::TestInvoker<Dut>{function},
        detail::make_test_metadata(name, {}, options), declaration};
}

template <typename Dut, typename Parameter, typename Value>
    requires std::constructible_from<std::decay_t<Parameter>, Value&&>
detail::TestRegistration<Dut> register_test_case(
    const char* base_name, const char* case_name,
    coro::Task<void> (*function)(Dut, TestContext&, Parameter), Value&& value,
    TestOptions options = {},
    std::source_location declaration = std::source_location::current()) {
    using Stored = std::decay_t<Parameter>;
    auto parameter =
        std::make_shared<Stored>(std::forward<Value>(value));
    detail::TestInvoker<Dut> invoker =
        [function, parameter](Dut dut, TestContext& context) {
            return function(dut, context, *parameter);
        };
    std::string name = std::string{base_name} + "[" + case_name + "]";
    return detail::TestRegistration<Dut>{
        std::move(name), std::move(invoker),
        detail::make_test_metadata(base_name, case_name, options), declaration};
}

template <typename Dut>
std::span<const detail::TestDescriptor<Dut>> registered_tests() {
    return detail::test_registry<Dut>();
}

template <typename Dut>
bool run_registered_test(coro::Testbench& scheduler, Dut dut,
                         TestResult& result, RunRequest request,
                         coro::ClockRegistrar clocks = {},
                         ResultSink* sink = nullptr) {
    detail::reset_selection_result(result);
    const auto entries = registered_tests<Dut>();

    if (request.list_only) {
        for (const auto& entry : entries) {
            std::printf("CPPTB_TEST %s\n", entry.name.c_str());
        }
        result.status = TestStatus::Passed;
        result.finished = true;
        return true;
    }

    if (!request.configuration_error.empty()) {
        const std::string message{request.configuration_error};
        std::fprintf(stderr, "cpptb: %s\n", message.c_str());
        detail::record_selection_error(result, message, sink);
        return false;
    }

    if (!request.test_name.empty()) {
        result.test_name.assign(request.test_name);
    }

    const detail::TestDescriptor<Dut>* selected = nullptr;
    if (!request.test_name.empty()) {
        for (const auto& entry : entries) {
            if (entry.name != request.test_name) continue;
            if (selected) {
                const std::string message =
                    "multiple tests are registered with the selected name '" +
                    std::string{request.test_name} + "'";
                std::fprintf(stderr, "cpptb: %s\n", message.c_str());
                detail::record_selection_error(result, message, sink);
                return false;
            }
            selected = &entry;
        }
        if (!selected) {
            const std::string message = "unknown test '" +
                                        std::string{request.test_name} + "'";
            std::fprintf(stderr, "cpptb: %s; available:", message.c_str());
            for (const auto& entry : entries) {
                std::fprintf(stderr, " %s", entry.name.c_str());
            }
            std::fputc('\n', stderr);
            detail::record_selection_error(result, message, sink);
            return false;
        }
    } else if (entries.size() == 1) {
        selected = &entries.front();
    } else {
        std::string message;
        if (entries.empty()) {
            message = "no test is registered for the selected DUT";
        } else {
            message = "multiple tests are registered; select one with "
                      "CPPTB_TEST";
        }
        std::fprintf(stderr, "cpptb: %s", message.c_str());
        if (!entries.empty()) {
            std::fputs("; available:", stderr);
            for (const auto& entry : entries) {
                std::fprintf(stderr, " %s", entry.name.c_str());
            }
        }
        std::fputc('\n', stderr);
        detail::record_selection_error(result, message, sink);
        return false;
    }

    const auto simulation_timeout = request.simulation_timeout
                                        ? request.simulation_timeout
                                        : selected->metadata.simulation_timeout;
    if (simulation_timeout && simulation_timeout->femtoseconds == 0) {
        const std::string message =
            "simulation-time timeout must be greater than zero";
        std::fprintf(stderr, "cpptb: %s\n", message.c_str());
        detail::record_selection_error(result, message, sink);
        return false;
    }

    detail::TestRunStatePtr state{
        new detail::TestRunState(scheduler, result, clocks, sink)};
    state->begin(selected->name, selected->metadata,
                 request.random_seed.value_or(kDefaultRandomSeed));
    if (!selected->metadata.skip_reason.empty()) {
        state->record_skip(selected->metadata.skip_reason,
                           selected->declaration);
        return true;
    }
    auto* root_provenance =
        state->create_process("root process", selected->declaration);
    auto root = scheduler.spawn(detail::invoke_registered_test(
        selected->function, dut, state, selected->declaration,
        simulation_timeout), static_cast<void*>(root_provenance),
        coro::ProcessCompletionHandler{
            root_provenance, detail::finish_owned_process});
    return true;
}

template <typename Dut>
bool run_registered_test(coro::Testbench& scheduler, Dut dut,
                         TestResult& result,
                         coro::ClockRegistrar clocks = {}) {
    return run_registered_test(scheduler, dut, result,
                               detail::environment_run_request(), clocks);
}

}  // namespace cpptb

#define CPPTB_DETAIL_JOIN_IMPL(Left, Right) Left##Right
#define CPPTB_DETAIL_JOIN(Left, Right) CPPTB_DETAIL_JOIN_IMPL(Left, Right)
#define CPPTB_REGISTER_TEST(TestFunction)                                  \
    [[maybe_unused]] const auto CPPTB_DETAIL_JOIN(                         \
        cpptb_test_registration_, __COUNTER__) =                           \
        ::cpptb::register_test(#TestFunction, TestFunction, {},            \
                               std::source_location::current())

#define CPPTB_REGISTER_TEST_WITH_OPTIONS(TestFunction, Options)             \
    [[maybe_unused]] const auto CPPTB_DETAIL_JOIN(                          \
        cpptb_test_registration_, __COUNTER__) =                            \
        ::cpptb::register_test(#TestFunction, TestFunction, Options,        \
                               std::source_location::current())

#define CPPTB_REGISTER_TEST_CASE(TestFunction, CaseName, CaseValue)         \
    [[maybe_unused]] const auto CPPTB_DETAIL_JOIN(                          \
        cpptb_test_registration_, __COUNTER__) =                            \
        ::cpptb::register_test_case(                                        \
            #TestFunction, #CaseName, TestFunction, CaseValue, {},          \
            std::source_location::current())

#define CPPTB_REGISTER_TEST_CASE_WITH_OPTIONS(                              \
    TestFunction, CaseName, CaseValue, Options)                             \
    [[maybe_unused]] const auto CPPTB_DETAIL_JOIN(                          \
        cpptb_test_registration_, __COUNTER__) =                            \
        ::cpptb::register_test_case(                                        \
            #TestFunction, #CaseName, TestFunction, CaseValue, Options,     \
            std::source_location::current())
