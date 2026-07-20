#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "cpptb/test_api.hpp"
#include "cpptb/test_reporting.hpp"

namespace test_types {

enum class SymbolicState : uint8_t { Idle = 1, Running = 2 };

inline constexpr std::string_view cpptb_diagnostic_name(SymbolicState value) {
    switch (value) {
        case SymbolicState::Idle:
            return "IDLE";
        case SymbolicState::Running:
            return "RUNNING";
    }
    return {};
}

struct Transaction {
    uint32_t address = 0;
    uint32_t data = 0;
    friend bool operator==(const Transaction&, const Transaction&) = default;
};

struct PackedView {
    cpptb::Bits<12> bits;
    cpptb::Bits<12> raw_bits() const { return bits; }
    friend bool operator==(const PackedView&, const PackedView&) = default;
};

}  // namespace test_types

namespace cpptb {

template <>
struct DiagnosticFormatter<test_types::Transaction> {
    static std::string format(const test_types::Transaction& value) {
        return "Transaction{address=" + std::to_string(value.address) +
               ", data=" + std::to_string(value.data) + "}";
    }
};

}  // namespace cpptb

namespace {

struct UniqueDut {
    uint32_t value;
};

struct AmbiguousDut {};

struct MissingDut {};

struct RequirementDut {
    bool* continued;
};

struct ChildFailureDut {
    bool* root_completed;
    bool* sibling_completed;
};

struct ExceptionDut {
    bool* root_completed;
};

struct DuplicateDut {};

struct WarningDut {};
struct LoggingDut {
    uint32_t* disabled_factories;
    uint32_t* enabled_factories;
    bool* debug_enabled;
    bool* error_enabled;
};
struct RetainedLoggingDut {
    std::optional<cpptb::Logger>* logger;
};
struct ThrowingLoggingDut {};
struct SimLoggingDut {
    cpptb::detail::SimLogEndpoint* endpoint;
};
struct SkipDut {
    bool* continued;
};
struct StaticSkipDut {};
struct ExpectedFailureDut {};
struct UnexpectedPassDut {};
struct TimeoutDut {
    bool* completed;
};
struct ParameterizedDut {
    uint32_t* observed;
};
struct SpawnStressDut {};
struct RandomDut {
    uint64_t* root_value;
    uint64_t* child_value;
};

class ContextRandomized final : public cpptb::Randomized {
   public:
    cpptb::Rand<uint8_t> value{*this, "value", 0, 7};

    ContextRandomized() { constraint(value == 3); }
};

class FailingRandomBackend final : public cpptb::ConstraintBackend {
   public:
    std::string_view name() const noexcept override { return "test-backend"; }
    cpptb::RandomizeResult solve(const cpptb::ConstraintProblem&,
                                 cpptb::Random&) override {
        return {.status = cpptb::RandomizeStatus::Unsatisfiable,
                .message = "deliberate contradiction"};
    }
};

struct ParameterCase {
    uint32_t value;
};

struct ClockCapture {
    uint32_t signal_id = 0;
    cpptb::coro::SimTime period{};
    cpptb::coro::SimTime phase{};
    uint32_t calls = 0;
};

void capture_clock(void* context, cpptb::coro::Signal signal,
                   cpptb::coro::SimTime period,
                   cpptb::coro::SimTime phase) {
    auto& capture = *static_cast<ClockCapture*>(context);
    capture.signal_id = signal.id;
    capture.period = period;
    capture.phase = phase;
    ++capture.calls;
}

cpptb::coro::Task<void> set_value(uint32_t& value, uint32_t next) {
    value = next;
    co_return;
}

cpptb::coro::Task<void> set_value_detached(uint32_t& value, uint32_t next,
                                           cpptb::coro::Event& done) {
    value = next;
    done.set();
    co_return;
}

cpptb::coro::Task<void> passing_test(UniqueDut dut,
                                     cpptb::TestContext& test) {
    uint32_t joined = 0;
    auto process = test.spawn(set_value(joined, 11));
    co_await process;
    test.expect_eq("spawn and join", joined, 11u);

    uint32_t detached = 0;
    cpptb::coro::Event detached_done;
    test.spawn_detached(
        set_value_detached(detached, 17, detached_done));
    co_await detached_done;
    test.expect_eq("detached spawn", detached, 17u);
    test.expect_eq("typed DUT value", dut.value, 42u);
    test.expect("explicit boolean check", true);
    co_return;
}

cpptb::coro::Task<void> first_test(AmbiguousDut,
                                   cpptb::TestContext&) {
    co_return;
}

cpptb::coro::Task<void> second_test(AmbiguousDut,
                                    cpptb::TestContext&) {
    co_return;
}

cpptb::coro::Task<void> requirement_test(RequirementDut dut,
                                         cpptb::TestContext& test) {
    test.expect("nonfatal failure", false);
    try {
        test.require_eq("fatal comparison", 7u, 8u);
    } catch (const std::exception&) {
        *dut.continued = true;
    }
    *dut.continued = true;
    co_return;
}

cpptb::coro::Task<void> failing_child(cpptb::TestContext test) {
    co_await cpptb::coro::Delay{cpptb::coro::operator""_ns(1)};
    test.require("child prerequisite", false);
}

cpptb::coro::Task<void> slow_sibling(bool& completed) {
    co_await cpptb::coro::Delay{cpptb::coro::operator""_ns(100)};
    completed = true;
}

cpptb::coro::Task<void> child_failure_test(ChildFailureDut dut,
                                           cpptb::TestContext& test) {
    test.spawn_detached(failing_child(test));
    test.spawn_detached(slow_sibling(*dut.sibling_completed));
    co_await cpptb::coro::Delay{cpptb::coro::operator""_ns(100)};
    *dut.root_completed = true;
}

cpptb::coro::Task<void> throwing_child() {
    co_await cpptb::coro::Delay{cpptb::coro::operator""_ns(1)};
    throw std::runtime_error{"child process exploded"};
}

cpptb::coro::Task<void> exception_test(ExceptionDut dut,
                                       cpptb::TestContext& test) {
    test.spawn_detached(throwing_child());
    co_await cpptb::coro::Delay{cpptb::coro::operator""_ns(100)};
    *dut.root_completed = true;
}

cpptb::coro::Task<void> warning_child(cpptb::TestContext test) {
    test.warn("child is using a fallback response");
    co_return;
}

cpptb::coro::Task<void> warning_test(WarningDut,
                                     cpptb::TestContext& test) {
    auto child = test.spawn(warning_child(test));
    co_await child;
}

cpptb::coro::Task<void> logging_child(LoggingDut dut,
                                      cpptb::TestContext test) {
    co_await cpptb::coro::Delay{cpptb::coro::operator""_ns(2)};
    auto log = test.logger("driver.child");
    *dut.debug_enabled = log.enabled(cpptb::LogLevel::Debug);
    *dut.error_enabled = log.enabled(cpptb::LogLevel::Error);
    log.debug([&] {
        ++*dut.disabled_factories;
        return std::string{"disabled child detail"};
    });
    log.info([&] {
        ++*dut.enabled_factories;
        return std::string{"accepted transaction 17"};
    });
    log.warning("retry threshold reached");
    log.error("protocol error observed");
}

cpptb::coro::Task<void> logging_peer(cpptb::TestContext test) {
    co_await cpptb::coro::Delay{cpptb::coro::operator""_ns(2)};
    test.logger("monitor.peer").info("peer observed transaction");
}

cpptb::coro::Task<void> logging_test(LoggingDut dut,
                                     cpptb::TestContext& test) {
    auto log = test.logger("environment");
    log.info("logging started");
    log.trace([&] {
        ++*dut.disabled_factories;
        return std::string{"disabled root detail"};
    });
    auto child = test.spawn(logging_child(dut, test));
    auto peer = test.spawn(logging_peer(test));
    co_await child;
    co_await peer;
    log.info("logging child complete");
}

cpptb::coro::Task<void> retained_logging_test(RetainedLoggingDut dut,
                                              cpptb::TestContext& test) {
    dut.logger->emplace(test.logger("retained"));
    co_return;
}

cpptb::coro::Task<void> throwing_logging_test(ThrowingLoggingDut,
                                              cpptb::TestContext& test) {
    test.logger("throwing-sink").info("sink failure");
    co_return;
}

cpptb::coro::Task<void> sim_logging_test(SimLoggingDut dut,
                                         cpptb::TestContext& test) {
    test.logger("cpp.driver").info("C++ request");
    dut.endpoint->emit({
        .level = cpptb::LogLevel::Info,
        .message = "SV response",
        .scope = "rtl.monitor",
        .source_file = "nested_monitor.sv",
        .hierarchy = "TOP.dpi_test.i_dut.u_monitor",
        .simulation_time_fs = 4'000'000,
        .source_line = 27,
    });
    dut.endpoint->emit({
        .level = cpptb::LogLevel::Debug,
        .message = "filtered SV detail",
        .source_file = "nested_monitor.sv",
        .hierarchy = "TOP.dpi_test.i_dut.u_monitor",
        .simulation_time_fs = 4'000'000,
        .source_line = 28,
    });
    co_return;
}

cpptb::coro::Task<void> suspended_sim_logging_test(SimLoggingDut,
                                                   cpptb::TestContext&) {
    co_await cpptb::coro::Delay{cpptb::coro::operator""_ns(1)};
}

cpptb::coro::Task<void> skip_test(SkipDut dut,
                                  cpptb::TestContext& test) {
    test.skip("feature is unavailable");
    *dut.continued = true;
}

cpptb::coro::Task<void> static_skip_test(StaticSkipDut,
                                         cpptb::TestContext&) {
    co_return;
}

cpptb::coro::Task<void> expected_failure_test(ExpectedFailureDut,
                                               cpptb::TestContext& test) {
    test.expect_eq("known mismatch", 7u, 8u);
    co_return;
}

cpptb::coro::Task<void> unexpected_pass_test(UnexpectedPassDut,
                                              cpptb::TestContext&) {
    co_return;
}

cpptb::coro::Task<void> timeout_test(TimeoutDut dut,
                                     cpptb::TestContext&) {
    co_await cpptb::coro::Delay{cpptb::coro::operator""_ns(10)};
    *dut.completed = true;
}

cpptb::coro::Task<void> parameterized_test(
    ParameterizedDut dut, cpptb::TestContext& test,
    const ParameterCase& parameter) {
    *dut.observed = parameter.value;
    test.expect_eq("case value", *dut.observed, parameter.value);
    co_return;
}

cpptb::coro::Task<void> spawn_stress_child(cpptb::TestContext test,
                                           uint32_t iteration) {
    test.expect_eq("spawn iteration", iteration, iteration);
    if (iteration == 4'095) test.warn("last recycled process");
    co_return;
}

cpptb::coro::Task<void> spawn_stress_test(SpawnStressDut,
                                          cpptb::TestContext& test) {
    for (uint32_t iteration = 0; iteration < 4'096; ++iteration) {
        auto child = test.spawn(spawn_stress_child(test, iteration));
        co_await child;
    }
}

cpptb::coro::Task<void> random_child(cpptb::TestContext test,
                                     uint64_t& value) {
    value = test.random().next_u64();
    co_return;
}

cpptb::coro::Task<void> random_test(RandomDut dut,
                                    cpptb::TestContext& test) {
    *dut.root_value = test.random().next_u64();
    auto child = test.spawn(random_child(test, *dut.child_value));
    co_await child;
}

cpptb::coro::Task<void> duplicate_first(DuplicateDut,
                                        cpptb::TestContext&) {
    co_return;
}

cpptb::coro::Task<void> duplicate_second(DuplicateDut,
                                         cpptb::TestContext&) {
    co_return;
}

struct CaptureSink : cpptb::ResultSink {
    uint32_t starts = 0;
    uint32_t failures = 0;
    uint32_t warnings = 0;
    uint32_t finishes = 0;

    void test_started(const cpptb::TestResult&) override { ++starts; }
    void failure_recorded(const cpptb::FailureRecord&) override {
        ++failures;
    }
    void warning_recorded(const cpptb::WarningRecord&) override { ++warnings; }
    void test_finished(const cpptb::TestResult&) override { ++finishes; }
};

struct CapturedLog {
    cpptb::LogLevel level;
    cpptb::LogOrigin origin;
    std::string message;
    std::string scope;
    std::string test_name;
    std::string source_file;
    std::string hierarchy;
    std::string process;
    std::string process_source_file;
    uint64_t sequence;
    uint64_t simulation_time_fs;
    uint64_t process_id;
    uint32_t source_line;
    uint32_t process_source_line;
};

struct CaptureLogSink : cpptb::LogSink {
    std::vector<CapturedLog> records;

    void emit(const cpptb::LogRecord& record) override {
        records.push_back({
            .level = record.level,
            .origin = record.origin,
            .message = std::string{record.message},
            .scope = std::string{record.scope},
            .test_name = std::string{record.test_name},
            .source_file = std::string{record.source_file},
            .hierarchy = std::string{record.hierarchy},
            .process = std::string{record.process},
            .process_source_file = std::string{record.process_source_file},
            .sequence = record.sequence,
            .simulation_time_fs = record.simulation_time_fs,
            .process_id = record.process_id,
            .source_line = record.source_line,
            .process_source_line = record.process_source_line,
        });
    }
};

struct ThrowingLogSink : cpptb::LogSink {
    void emit(const cpptb::LogRecord&) override {
        throw std::runtime_error{"log sink failure"};
    }
};

bool expect(const char* label, uint64_t actual, uint64_t expected) {
    if (actual == expected) return true;
    std::fprintf(stderr, "%s: actual=%llu expected=%llu\n", label,
                 static_cast<unsigned long long>(actual),
                 static_cast<unsigned long long>(expected));
    return false;
}

}  // namespace

int main() {
    bool passed = true;

    const auto unique = cpptb::register_test("passing_test", passing_test);
    cpptb::coro::Testbench scheduler;
    cpptb::TestResult result;
    passed &= cpptb::run_registered_test(scheduler, UniqueDut{42}, result);
    passed &= expect("registered test completed", scheduler.done(), true);
    passed &= expect("check count", result.checks, 4);
    passed &= expect("failure count", result.failures, 0);
    passed &= expect("log level name",
                     cpptb::log_level_name(cpptb::LogLevel::Warning) ==
                         "warning",
                     true);
    passed &= expect("warn level alias",
                     cpptb::parse_log_level("warn") ==
                         cpptb::LogLevel::Warning,
                     true);
    passed &= expect("invalid log level",
                     cpptb::parse_log_level("verbose").has_value(), false);
    FILE* log_stream = std::tmpfile();
    passed &= expect("temporary log stream created", log_stream != nullptr,
                     true);
    if (log_stream) {
        cpptb::StderrLogSink stream_sink{log_stream};
        stream_sink.emit(cpptb::LogRecord{.sequence = 3, .process_id = 7});
        stream_sink.emit(cpptb::LogRecord{
            .level = cpptb::LogLevel::Warning,
            .message = "all fields",
            .scope = "apb.driver",
            .source_file = "testbench.cpp",
            .process = "request driver",
            .sequence = 4,
            .simulation_time_fs = 2'000'000,
            .process_id = 2,
            .source_line = 17,
        });
        stream_sink.emit(cpptb::LogRecord{
            .origin = cpptb::LogOrigin::SystemVerilog,
            .message = "RTL event",
            .scope = "rtl.monitor",
            .source_file = "monitor.sv",
            .hierarchy = "TOP.dpi_test.i_dut.u_monitor",
            .sequence = 5,
            .simulation_time_fs = 3'000'000,
            .source_line = 29,
        });
        std::fflush(log_stream);
        std::rewind(log_stream);
        std::array<char, 512> output{};
        const auto output_size =
            std::fread(output.data(), 1, output.size(), log_stream);
        passed &= expect(
            "stderr sink formats empty views",
            std::string_view{output.data(), output_size} ==
                "cpptb: 0 fs #3: info [process 7: ]: \n"
                "cpptb: testbench.cpp:17: 2000000 fs #4: warning "
                "[apb.driver] [process 2: request driver]: all fields\n"
                "cpptb: monitor.sv:29: 3000000 fs #5: info [rtl.monitor] "
                "[sv TOP.dpi_test.i_dut.u_monitor]: RTL event\n",
            true);
        std::fclose(log_stream);
    }
    setenv("CPPTB_LOG_LEVEL", "debug", 1);
    const auto debug_environment = cpptb::detail::environment_run_request();
    passed &= expect("environment log level",
                     debug_environment.logging.minimum_level ==
                         cpptb::LogLevel::Debug,
                     true);
    setenv("CPPTB_LOG_LEVEL", "verbose", 1);
    const auto invalid_log_environment =
        cpptb::detail::environment_run_request();
    passed &= expect("invalid environment log level",
                     invalid_log_environment.configuration_error ==
                         "CPPTB_LOG_LEVEL must be trace, debug, info, warning, "
                         "error, or off",
                     true);
    setenv("CPPTB_RANDOM_SEED", "invalid", 1);
    const auto multiple_invalid_environment =
        cpptb::detail::environment_run_request();
    passed &= expect(
        "first environment configuration error is retained",
        multiple_invalid_environment.configuration_error ==
            "CPPTB_RANDOM_SEED must be a decimal or 0x-prefixed unsigned "
            "64-bit integer",
        true);
    unsetenv("CPPTB_RANDOM_SEED");
    unsetenv("CPPTB_LOG_LEVEL");
    const cpptb::RunRequest positional_request{
        {}, false, std::nullopt, std::nullopt, "positional configuration"};
    passed &= expect("RunRequest positional compatibility",
                     positional_request.configuration_error ==
                         "positional configuration",
                     true);

    const auto random_registration =
        cpptb::register_test("random_test", random_test);
    uint64_t first_root_random = 0;
    uint64_t first_child_random = 0;
    cpptb::coro::Testbench first_random_scheduler;
    cpptb::TestResult first_random_result;
    passed &= cpptb::run_registered_test(
        first_random_scheduler,
        RandomDut{&first_root_random, &first_child_random},
        first_random_result, cpptb::RunRequest{.random_seed = 0x1234});
    passed &= expect("explicit seed recorded",
                     first_random_result.random_seed.value_or(0), 0x1234);
    passed &= expect("random algorithm recorded",
                     first_random_result.random_algorithm ==
                         cpptb::kRandomAlgorithm,
                     true);
    passed &= expect("process streams differ",
                     first_root_random != first_child_random, true);

    uint64_t parsed_seed = 0;
    passed &= expect("decimal seed parsing",
                     cpptb::detail::parse_random_seed("012", parsed_seed) &&
                         parsed_seed == 12,
                     true);
    passed &= expect("hex seed parsing",
                     cpptb::detail::parse_random_seed("0Xfeed", parsed_seed) &&
                         parsed_seed == 0xfeed,
                     true);
    passed &= expect("whitespace-prefixed negative seed rejected",
                     cpptb::detail::parse_random_seed(" -1", parsed_seed),
                     false);
    passed &= expect("overflowing seed rejected",
                     cpptb::detail::parse_random_seed(
                         "18446744073709551616", parsed_seed),
                     false);

    cpptb::coro::Testbench invalid_seed_scheduler;
    cpptb::TestResult invalid_seed_result;
    passed &= !cpptb::run_registered_test(
        invalid_seed_scheduler,
        RandomDut{&first_root_random, &first_child_random},
        invalid_seed_result,
        cpptb::RunRequest{
            .configuration_error = "invalid CPPTB_RANDOM_SEED"});
    passed &= expect("invalid seed is a selection error",
                     invalid_seed_result.status == cpptb::TestStatus::Error,
                     true);

    uint64_t replay_root_random = 0;
    uint64_t replay_child_random = 0;
    cpptb::coro::Testbench replay_random_scheduler;
    cpptb::TestResult replay_random_result;
    passed &= cpptb::run_registered_test(
        replay_random_scheduler,
        RandomDut{&replay_root_random, &replay_child_random},
        replay_random_result, cpptb::RunRequest{.random_seed = 0x1234});
    passed &= expect("root process random replay", replay_root_random,
                     first_root_random);
    passed &= expect("child process random replay", replay_child_random,
                     first_child_random);

    cpptb::coro::Testbench checking_scheduler;
    checking_scheduler.set_time(37);
    cpptb::TestResult checking_result;
    ClockCapture clock_capture;
    const cpptb::coro::ClockRegistrar clocks{&clock_capture, capture_clock};
    cpptb::TestContext checking_context{checking_scheduler, checking_result,
                                        clocks};
    passed &= expect("context now",
                     checking_context.now().in_nanoseconds(), 37);
    checking_context.start_clock(
        cpptb::coro::Signal{.id = 7, .name = "bus_clk"},
        cpptb::coro::operator""_ns(8), cpptb::coro::operator""_ns(1));
    passed &= expect("clock registration count", clock_capture.calls, 1);
    passed &= expect("clock signal", clock_capture.signal_id, 7);
    passed &= expect("clock period", clock_capture.period.in_nanoseconds(), 8);
    passed &= expect("clock phase", clock_capture.phase.in_nanoseconds(), 1);

    cpptb::coro::Testbench randomized_scheduler;
    cpptb::TestResult randomized_result;
    cpptb::TestContext randomized_context{randomized_scheduler,
                                           randomized_result};
    ContextRandomized randomized;
    randomized_context.randomize(randomized);
    passed &= expect("context randomize assignment", randomized.value.get(), 3);
    passed &= expect("context randomize backend metadata",
                     randomized_result.constraint_backend == "adaptive" &&
                         randomized_result.random_sampling_solves == 1 &&
                         randomized_result.random_solver_solves == 0,
                     true);
    FailingRandomBackend failing_random_backend;
    randomized_context.set_random_backend(failing_random_backend);
    passed &= expect("context backend selection",
                     &randomized_context.random_backend() ==
                         &failing_random_backend,
                     true);
    bool randomize_aborted = false;
    try {
        randomized_context.randomize(randomized);
    } catch (...) {
        randomize_aborted = true;
    }
    passed &= expect("failed context randomize aborts", randomize_aborted, true);
    passed &= expect(
        "failed context randomize diagnostic",
        !randomized_result.failure_records.empty() &&
            randomized_result.failure_records.back().label.find(
                "test-backend") != std::string::npos &&
            randomized_result.failure_records.back().label.find(
                "deliberate contradiction") != std::string::npos,
        true);

    FILE* diagnostic = std::tmpfile();
    cpptb::detail::print_diagnostic_integral(diagnostic, -7);
    std::rewind(diagnostic);
    char diagnostic_text[256]{};
    std::fread(diagnostic_text, 1, sizeof(diagnostic_text) - 1, diagnostic);
    std::fclose(diagnostic);
    passed &= expect(
        "signed mismatch diagnostic",
        std::strcmp(diagnostic_text, "-7") == 0,
        true);

    const auto bits_text = cpptb::format_diagnostic(
        cpptb::Bits<12>::from_hex("abc"));
    passed &= expect("Bits diagnostic",
                     bits_text && *bits_text == "12'habc", true);
    const auto logic_text = cpptb::format_diagnostic(
        cpptb::LogicBits<4>::from_string("10XZ"));
    passed &= expect("LogicBits diagnostic",
                     logic_text && *logic_text == "4'b10XZ", true);
    const auto array_text = cpptb::format_diagnostic(
        std::array<int, 3>{1, -2, 3});
    passed &= expect("array diagnostic",
                     array_text && *array_text == "[1, -2, 3]", true);
    const auto packed_view_text = cpptb::format_diagnostic(
        test_types::PackedView{cpptb::Bits<12>::from_hex("321")});
    passed &= expect("packed view diagnostic",
                     packed_view_text && *packed_view_text == "raw=12'h321",
                     true);
    const auto enum_text =
        cpptb::format_diagnostic(test_types::SymbolicState::Running);
    passed &= expect("symbolic enum diagnostic",
                     enum_text && *enum_text == "RUNNING", true);
    const auto transaction_text = cpptb::format_diagnostic(
        test_types::Transaction{.address = 4, .data = 9});
    passed &= expect(
        "custom transaction diagnostic",
        transaction_text &&
            *transaction_text == "Transaction{address=4, data=9}",
        true);
    const auto empty_string_text = cpptb::format_diagnostic(std::string{});
    passed &= expect("empty string remains formattable",
                     empty_string_text && empty_string_text->empty(), true);

    checking_context.expect_eq("signed mismatch", -7, 8);
    passed &= expect("failed check count", checking_result.checks, 1);
    passed &= expect("failed check failure count", checking_result.failures, 1);

    const char* json_path = "test_result_test.json";
    cpptb::TestResult json_result{
        .checks = 3,
        .failures = 1,
        .warnings = 2,
        .status = cpptb::TestStatus::Failed,
        .test_name = "quoted\"test",
        .case_name = "case-a",
        .status_reason = "demonstration",
        .tags = {"smoke", "json"},
        .failure_records = {{
            .kind = cpptb::FailureKind::Requirement,
            .label = "line one\nline two",
            .actual = "7",
            .expected = "8",
            .source_file = "test\tfile.cpp",
            .process = "root process",
            .simulation_time_fs = 125,
            .source_line = 73,
            .has_comparison = true,
        }},
        .warning_records = {{
            .label = "warning text",
            .source_file = "warning.cpp",
            .process = "spawned process",
            .process_source_file = "test.cpp",
            .simulation_time_fs = 200,
            .process_id = 2,
            .source_line = 19,
            .process_source_line = 17,
        }},
        .random_seed = 0xfeedbeef,
        .random_algorithm = cpptb::kRandomAlgorithm,
        .constraint_backend = "adaptive",
        .constraint_backend_version = "4.13.3",
        .random_sampling_solves = 11,
        .random_solver_solves = 2,
        .simulation_time_fs = 250,
        .wall_time_ns = 500,
        .finished = true,
    };
    passed &= expect("result JSON written",
                     cpptb::write_test_result_json(json_path, json_result),
                     true);
    FILE* json_file = std::fopen(json_path, "r");
    std::string json_text;
    if (json_file) {
        char buffer[512];
        while (const size_t bytes = std::fread(buffer, 1, sizeof(buffer),
                                                json_file)) {
            json_text.append(buffer, bytes);
        }
        std::fclose(json_file);
    }
    passed &= expect("result JSON readable", !json_text.empty(), true);
    passed &= expect("result JSON schema",
                     json_text.find("\"schema_version\":4") !=
                         std::string::npos,
                     true);
    passed &= expect("result JSON status",
                     json_text.find("\"status\":\"failed\"") !=
                         std::string::npos,
                     true);
    passed &= expect("result JSON escaping",
                     json_text.find("quoted\\\"test") != std::string::npos &&
                         json_text.find("line one\\nline two") !=
                             std::string::npos &&
                         json_text.find("test\\tfile.cpp") !=
                             std::string::npos,
                     true);
    passed &= expect("result JSON failure kind",
                     json_text.find("\"kind\":\"requirement\"") !=
                         std::string::npos,
                     true);
    passed &= expect("result JSON metadata",
                     json_text.find("\"case_name\":\"case-a\"") !=
                             std::string::npos &&
                         json_text.find("\"tags\":[\"smoke\",\"json\"]") !=
                             std::string::npos,
                     true);
    passed &= expect("result JSON replay metadata",
                     json_text.find("\"random_seed\":4276993775") !=
                             std::string::npos &&
                         json_text.find(
                             "\"random_algorithm\":\"xoshiro256ss-v1\"") !=
                             std::string::npos,
                     true);
    passed &= expect(
        "result JSON constraint backend metadata",
        json_text.find("\"constraint_backend\":\"adaptive\"") !=
                std::string::npos &&
            json_text.find(
                "\"constraint_backend_version\":\"4.13.3\"") !=
                std::string::npos &&
            json_text.find("\"random_sampling_solves\":11") !=
                std::string::npos &&
            json_text.find("\"random_solver_solves\":2") !=
                std::string::npos,
        true);
    passed &= expect("result JSON warning record",
                     json_text.find("\"label\":\"warning text\"") !=
                             std::string::npos &&
                         json_text.find("\"process_id\":2") !=
                             std::string::npos,
                     true);
    std::remove(json_path);

    cpptb::coro::Testbench missing_scheduler;
    cpptb::TestResult missing_result;
    passed &= !cpptb::run_registered_test(
        missing_scheduler, MissingDut{}, missing_result);
    passed &= expect("missing registration failure", missing_result.failures,
                     1);
    passed &= expect("missing registration did not schedule",
                     missing_scheduler.done(), true);

    const auto first = cpptb::register_test("first_test", first_test);
    const auto second = cpptb::register_test("second_test", second_test);
    cpptb::coro::Testbench ambiguous_scheduler;
    cpptb::TestResult ambiguous_result;
    passed &= !cpptb::run_registered_test(
        ambiguous_scheduler, AmbiguousDut{}, ambiguous_result);
    passed &= expect("ambiguous registration failure",
                     ambiguous_result.failures, 1);
    passed &= expect("ambiguous registration did not schedule",
                     ambiguous_scheduler.done(), true);

    const auto ambiguous_catalog = cpptb::registered_tests<AmbiguousDut>();
    passed &= expect("catalog size", ambiguous_catalog.size(), 2);
    passed &= expect("catalog first name",
                     std::string_view{ambiguous_catalog[0].name} ==
                         "first_test",
                     true);

    cpptb::coro::Testbench selected_scheduler;
    cpptb::TestResult selected_result;
    passed &= cpptb::run_registered_test(
        selected_scheduler, AmbiguousDut{}, selected_result,
        cpptb::RunRequest{.test_name = "second_test"});
    passed &= expect("selected test status",
                     selected_result.status == cpptb::TestStatus::Passed,
                     true);
    passed &= expect("selected test name",
                     selected_result.test_name == "second_test", true);
    passed &= expect("selected test finished", selected_result.finished, true);

    cpptb::coro::Testbench list_scheduler;
    cpptb::TestResult list_result;
    passed &= cpptb::run_registered_test(
        list_scheduler, AmbiguousDut{}, list_result,
        cpptb::RunRequest{.list_only = true});
    passed &= expect("list request status",
                     list_result.status == cpptb::TestStatus::Passed, true);
    passed &= expect("list request schedules nothing", list_scheduler.done(),
                     true);

    cpptb::coro::Testbench unknown_scheduler;
    cpptb::TestResult unknown_result;
    passed &= !cpptb::run_registered_test(
        unknown_scheduler, AmbiguousDut{}, unknown_result,
        cpptb::RunRequest{.test_name = "missing_test"});
    passed &= expect("unknown selection status",
                     unknown_result.status == cpptb::TestStatus::Error, true);
    passed &= expect("unknown selection retains requested name",
                     unknown_result.test_name == "missing_test", true);
    passed &= expect("unknown selection record",
                     unknown_result.failure_records.size(), 1);

    bool requirement_continued = false;
    const auto requirement = cpptb::register_test(
        "requirement_test", requirement_test);
    cpptb::coro::Testbench requirement_scheduler;
    cpptb::TestResult requirement_result;
    CaptureSink requirement_sink;
    passed &= cpptb::run_registered_test(
        requirement_scheduler, RequirementDut{&requirement_continued},
        requirement_result, cpptb::RunRequest{}, {}, &requirement_sink);
    passed &= expect("requirement test stopped", requirement_continued, false);
    passed &= expect("requirement test status",
                     requirement_result.status == cpptb::TestStatus::Failed,
                     true);
    passed &= expect("requirement checks", requirement_result.checks, 2);
    passed &= expect("requirement failures", requirement_result.failures, 2);
    passed &= expect("requirement records",
                     requirement_result.failure_records.size(), 2);
    passed &= expect(
        "requirement kind",
        requirement_result.failure_records[1].kind ==
            cpptb::FailureKind::Requirement,
        true);
    passed &= expect("requirement comparison captured",
                     requirement_result.failure_records[1].has_comparison,
                     true);
    passed &= expect("requirement source captured",
                     requirement_result.failure_records[1].source_line != 0,
                     true);
    passed &= expect("result sink starts", requirement_sink.starts, 1);
    passed &= expect("result sink failures", requirement_sink.failures, 2);
    passed &= expect("result sink finishes", requirement_sink.finishes, 1);

    bool child_root_completed = false;
    bool child_sibling_completed = false;
    const auto child_failure = cpptb::register_test(
        "child_failure_test", child_failure_test);
    cpptb::coro::Testbench child_scheduler;
    cpptb::TestResult child_result;
    passed &= cpptb::run_registered_test(
        child_scheduler,
        ChildFailureDut{&child_root_completed, &child_sibling_completed},
        child_result);
    passed &= expect("child test initially active", child_scheduler.done(),
                     false);
    child_scheduler.set_time(1);
    passed &= expect("child requirement cancels test", child_scheduler.done(),
                     true);
    passed &= expect("child requirement status",
                     child_result.status == cpptb::TestStatus::Failed, true);
    passed &= expect("child requirement root cancelled", child_root_completed,
                     false);
    passed &= expect("child requirement sibling cancelled",
                     child_sibling_completed, false);
    passed &= expect("child requirement process attributed",
                     child_result.failure_records[0].process_id != 0, true);
    passed &= expect("child requirement spawn source attributed",
                     child_result.failure_records[0].process_source_line != 0,
                     true);

    bool exception_root_completed = false;
    const auto exception = cpptb::register_test("exception_test",
                                                exception_test);
    cpptb::coro::Testbench exception_scheduler;
    cpptb::TestResult exception_result;
    passed &= cpptb::run_registered_test(
        exception_scheduler, ExceptionDut{&exception_root_completed},
        exception_result);
    exception_scheduler.set_time(1);
    passed &= expect("child exception cancels test", exception_scheduler.done(),
                     true);
    passed &= expect("child exception status",
                     exception_result.status == cpptb::TestStatus::Error, true);
    passed &= expect("child exception root cancelled", exception_root_completed,
                     false);
    passed &= expect("child exception process captured",
                     exception_result.failure_records[0].process ==
                         "spawned process",
                     true);
    passed &= expect("child exception process id captured",
                     exception_result.failure_records[0].process_id != 0,
                     true);
    passed &= expect("child exception spawn file captured",
                     !exception_result.failure_records[0]
                          .process_source_file.empty(),
                     true);

    const auto warning = cpptb::register_test("warning_test", warning_test);
    cpptb::coro::Testbench warning_scheduler;
    cpptb::TestResult warning_result;
    CaptureSink warning_sink;
    passed &= cpptb::run_registered_test(
        warning_scheduler, WarningDut{}, warning_result,
        cpptb::RunRequest{}, {}, &warning_sink);
    passed &= expect("warning test passed",
                     warning_result.status == cpptb::TestStatus::Passed, true);
    passed &= expect("warning count", warning_result.warnings, 1);
    passed &= expect("warning record count",
                     warning_result.warning_records.size(), 1);
    passed &= expect("warning process attributed",
                     warning_result.warning_records[0].process_id != 0, true);
    passed &= expect("warning sink callback", warning_sink.warnings, 1);

    const auto logging = cpptb::register_test("logging_test", logging_test);
    uint32_t disabled_log_factories = 0;
    uint32_t enabled_log_factories = 0;
    bool debug_log_enabled = true;
    bool error_log_enabled = false;
    cpptb::coro::Testbench logging_scheduler;
    cpptb::TestResult logging_result;
    CaptureLogSink logging_sink;
    cpptb::LogHistory logging_history;
    passed &= cpptb::run_registered_test(
        logging_scheduler,
        LoggingDut{&disabled_log_factories, &enabled_log_factories,
                   &debug_log_enabled, &error_log_enabled},
        logging_result,
        cpptb::RunRequest{
            .logging = {.minimum_level = cpptb::LogLevel::Info,
                        .sink = &logging_sink,
                        .history = &logging_history}});
    passed &= expect("logging waits for child", logging_scheduler.done(),
                     false);
    logging_scheduler.set_time(2);
    passed &= expect("logging test completed", logging_scheduler.done(), true);
    passed &= expect("disabled log factories are lazy", disabled_log_factories,
                     0);
    passed &= expect("enabled log factory invoked once", enabled_log_factories,
                     1);
    passed &= expect("debug threshold query", debug_log_enabled, false);
    passed &= expect("error threshold query", error_log_enabled, true);
    passed &= expect("structured log output count", logging_sink.records.size(),
                     6);
    passed &= expect("structured log history count", logging_history.size(), 6);
    passed &= expect("structured log history is not empty",
                     logging_history.empty(), false);
    uint64_t ordered_records = 0;
    uint64_t previous_time = 0;
    uint64_t process_mask_at_2ns = 0;
    for (std::size_t index = 0; index < logging_history.size(); ++index) {
        const auto& record = logging_history[index];
        if (record.sequence == index + 1 &&
            (index == 0 || record.simulation_time_fs >= previous_time)) {
            ++ordered_records;
        }
        previous_time = record.simulation_time_fs;
        if (record.simulation_time_fs == 2'000'000 && record.process_id < 64) {
            process_mask_at_2ns |= uint64_t{1} << record.process_id;
        }
        passed &= expect("forwarded sequence matches history",
                         logging_sink.records[index].sequence, record.sequence);
    }
    passed &= expect("history follows simulation and emission order",
                     ordered_records, logging_history.size());
    passed &= expect("same-time logs retain all process identities",
                     process_mask_at_2ns, 0b1110);
    passed &= expect("root start log sequence", logging_history[0].sequence, 1);
    passed &= expect("root start log time",
                     logging_history[0].simulation_time_fs, 0);
    passed &= expect("root start log message",
                     logging_history[0].message == "logging started", true);
    passed &= expect("child log level",
                     logging_history[1].level == cpptb::LogLevel::Info,
                     true);
    passed &= expect("child log message",
                     logging_history[1].message ==
                         "accepted transaction 17",
                     true);
    passed &= expect("child log scope",
                     logging_history[1].scope == "driver.child", true);
    passed &= expect("child log test name",
                     logging_history[1].test_name == "logging_test",
                     true);
    passed &= expect("child log simulation time",
                     logging_history[1].simulation_time_fs, 2'000'000);
    passed &= expect("child log process id",
                     logging_history[1].process_id, 2);
    passed &= expect("child log process description",
                     logging_history[1].process == "spawned process",
                     true);
    passed &= expect("child log source captured",
                     !logging_history[1].source_file.empty() &&
                         logging_history[1].source_line != 0,
                     true);
    passed &= expect("child spawn source captured",
                     !logging_history[1].process_source_file.empty() &&
                         logging_history[1].process_source_line != 0,
                     true);
    passed &= expect("warning log remains observational",
                     logging_result.warnings, 0);
    passed &= expect("error log remains observational",
                     logging_result.failures, 0);
    passed &= expect("root log process id",
                     logging_history[5].process_id, 1);
    passed &= expect("root log scope",
                     logging_history[5].scope == "environment", true);
    logging_history.clear();
    passed &= expect("cleared log history is empty", logging_history.empty(),
                     true);

    cpptb::coro::Testbench external_logging_scheduler;
    external_logging_scheduler.set_time(7);
    cpptb::TestResult external_logging_result;
    external_logging_result.test_name = "external_logging";
    cpptb::LogHistory external_logging_history;
    {
        cpptb::TestContext external_logging_context{
            external_logging_scheduler, external_logging_result, {}, nullptr,
            cpptb::LoggingOptions{
                .minimum_level = cpptb::LogLevel::Info,
                .sink = &external_logging_history,
                .history = &external_logging_history}};
        external_logging_context.logger("harness").info("outside process");
    }
    passed &= expect("history-as-sink emits once",
                     external_logging_history.size(), 1);
    passed &= expect("external log keeps simulation time",
                     external_logging_history[0].simulation_time_fs,
                     7'000'000);
    passed &= expect("external log has no process id",
                     external_logging_history[0].process_id, 0);
    passed &= expect("external log keeps scope",
                     external_logging_history[0].scope == "harness", true);
    passed &= expect("external log keeps test name",
                     external_logging_history[0].test_name ==
                         "external_logging",
                     true);

    const auto sim_logging =
        cpptb::register_test("sim_logging_test", sim_logging_test);
    CaptureLogSink sim_logging_sink;
    CaptureLogSink unowned_sim_logging_sink;
    cpptb::LogHistory sim_logging_history;
    cpptb::detail::SimLogEndpoint sim_log_endpoint{
        &unowned_sim_logging_sink};
    sim_log_endpoint.emit({
        .level = cpptb::LogLevel::Info,
        .message = "time-zero RTL initialization",
        .scope = "rtl.init",
        .source_file = "nested_monitor.sv",
        .hierarchy = "TOP.dpi_test.i_dut.u_monitor",
        .simulation_time_fs = 0,
        .source_line = 11,
    });
    passed &= expect("pre-test SV log is buffered",
                     sim_log_endpoint.pending_size(), 1);
    cpptb::coro::Testbench sim_logging_scheduler;
    sim_logging_scheduler.set_time(4);
    cpptb::TestResult sim_logging_result;
    passed &= cpptb::run_registered_test(
        sim_logging_scheduler, SimLoggingDut{&sim_log_endpoint},
        sim_logging_result,
        cpptb::RunRequest{
            .logging = {.minimum_level = cpptb::LogLevel::Info,
                        .sink = &sim_logging_sink,
                        .history = &sim_logging_history}},
        {}, nullptr, &sim_log_endpoint);
    passed &= expect("mixed-language logging test completed",
                     sim_logging_scheduler.done(), true);
    passed &= expect("mixed-language output count",
                     sim_logging_sink.records.size(), 3);
    passed &= expect("mixed-language history count",
                     sim_logging_history.size(), 3);
    passed &= expect("time-zero SV log drains first",
                     sim_logging_history[0].message ==
                         "time-zero RTL initialization",
                     true);
    passed &= expect("time-zero SV log has first sequence",
                     sim_logging_history[0].sequence, 1);
    passed &= expect("C++ log retains C++ origin",
                     sim_logging_history[1].origin == cpptb::LogOrigin::Cpp,
                     true);
    passed &= expect("C++ log retains root process",
                     sim_logging_history[1].process_id, 1);
    passed &= expect(
        "SV log retains SV origin",
        sim_logging_history[2].origin == cpptb::LogOrigin::SystemVerilog,
        true);
    passed &= expect("SV log retains hierarchy",
                     sim_logging_history[2].hierarchy ==
                         "TOP.dpi_test.i_dut.u_monitor",
                     true);
    passed &= expect("SV log retains source file",
                     sim_logging_history[2].source_file ==
                         "nested_monitor.sv",
                     true);
    passed &= expect("SV log retains source line",
                     sim_logging_history[2].source_line, 27);
    passed &= expect("SV log has no C++ process",
                     sim_logging_history[2].process_id, 0);
    passed &= expect("SV log shares sequence stream",
                     sim_logging_history[2].sequence, 3);
    passed &= expect("SV log keeps explicit simulation time",
                     sim_logging_history[2].simulation_time_fs, 4'000'000);
    passed &= expect("filtered SV log does not emit",
                     sim_logging_history.size(), 3);
    sim_log_endpoint.emit({
        .level = cpptb::LogLevel::Debug,
        .message = "late filtered RTL detail",
        .source_file = "nested_monitor.sv",
        .hierarchy = "TOP.dpi_test.i_dut.u_monitor",
        .simulation_time_fs = 5'000'000,
        .source_line = 31,
    });
    sim_log_endpoint.emit({
        .level = cpptb::LogLevel::Warning,
        .message = "late unowned RTL warning",
        .source_file = "nested_monitor.sv",
        .hierarchy = "TOP.dpi_test.i_dut.u_monitor",
        .simulation_time_fs = 5'000'000,
        .source_line = 32,
    });
    passed &= expect("post-test SV filtering remains active",
                     unowned_sim_logging_sink.records.size(), 1);
    passed &= expect("post-test SV log is unowned",
                     unowned_sim_logging_sink.records[0].sequence, 0);
    passed &= expect("post-test SV log has no test name",
                     unowned_sim_logging_sink.records[0].test_name.empty(),
                     true);

    CaptureLogSink selection_failure_sink;
    cpptb::detail::SimLogEndpoint selection_failure_logs{
        &selection_failure_sink};
    selection_failure_logs.emit({
        .level = cpptb::LogLevel::Debug,
        .message = "filtered elaboration detail",
        .source_file = "selection_monitor.sv",
        .hierarchy = "TOP.selection_failure.i_dut",
        .source_line = 8,
    });
    selection_failure_logs.emit({
        .level = cpptb::LogLevel::Warning,
        .message = "elaboration warning before selection",
        .source_file = "selection_monitor.sv",
        .hierarchy = "TOP.selection_failure.i_dut",
        .source_line = 9,
    });
    cpptb::coro::Testbench selection_failure_scheduler;
    cpptb::TestResult selection_failure_result;
    const bool selection_started = cpptb::run_registered_test(
        selection_failure_scheduler,
        SimLoggingDut{&selection_failure_logs}, selection_failure_result,
        cpptb::RunRequest{
            .test_name = "missing_sim_logging_test",
            .logging = {.minimum_level = cpptb::LogLevel::Info}},
        {}, nullptr, &selection_failure_logs);
    passed &= expect("selection failure does not start a test",
                     selection_started, false);
    passed &= expect("selection failure drains buffered SV logs",
                     selection_failure_logs.pending_size(), 0);
    passed &= expect("selection failure applies requested SV log level",
                     selection_failure_sink.records.size(), 1);
    passed &= expect("selection failure preserves buffered SV diagnostic",
                     selection_failure_sink.records[0].message ==
                         "elaboration warning before selection",
                     true);
    selection_failure_logs.emit({
        .level = cpptb::LogLevel::Error,
        .message = "RTL diagnostic after selection failure",
        .source_file = "selection_monitor.sv",
        .hierarchy = "TOP.selection_failure.i_dut",
        .source_line = 12,
    });
    passed &= expect("selection failure routes later SV logs unowned",
                     selection_failure_sink.records.size(), 2);
    passed &= expect("selection failure emits no owned sequence",
                     selection_failure_sink.records[1].sequence, 0);
    passed &= expect("selection failure emits no test name",
                     selection_failure_sink.records[1].test_name.empty(),
                     true);

    CaptureLogSink parent_runtime_sink;
    CaptureLogSink child_runtime_sink;
    cpptb::detail::SimLogEndpoint parent_runtime_logs{&parent_runtime_sink};
    cpptb::detail::SimLogEndpoint child_runtime_logs{&child_runtime_sink};
    int parent_runtime_owner = 0;
    int child_runtime_owner = 0;
    constexpr auto discard_sim_log =
        +[](void*, const cpptb::detail::SimLogMessage&) {};
    parent_runtime_logs.bind(&parent_runtime_owner, discard_sim_log,
                             cpptb::LogLevel::Info);
    parent_runtime_logs.unbind(&parent_runtime_owner, true);
    child_runtime_logs.bind(&child_runtime_owner, discard_sim_log,
                            cpptb::LogLevel::Error);
    child_runtime_logs.unbind(&child_runtime_owner, true);
    cpptb::detail::register_sim_log_runtime(&parent_runtime_owner,
                                            parent_runtime_logs);
    cpptb::detail::register_sim_log_runtime(&child_runtime_owner,
                                            child_runtime_logs);
    cpptb::detail::update_sim_log_runtime_hierarchy(&parent_runtime_owner,
                                                    "TOP.design");
    cpptb::detail::update_sim_log_runtime_hierarchy(&child_runtime_owner,
                                                    "TOP.design.child");
    passed &= expect(
        "SV logs select longest matching runtime hierarchy",
        cpptb::detail::find_sim_log_endpoint("TOP.design.child.block") ==
            &child_runtime_logs,
        true);
    passed &= expect(
        "SV logs select parent runtime hierarchy",
        cpptb::detail::find_sim_log_endpoint("TOP.design.peer") ==
            &parent_runtime_logs,
        true);
    passed &= expect("SV package uses least restrictive runtime level",
                     cpptb::detail::sim_log_minimum_level() ==
                         cpptb::LogLevel::Info,
                     true);
    cpptb::detail::emit_sim_log({
        .level = cpptb::LogLevel::Info,
        .message = "parent message",
        .hierarchy = "TOP.design.peer",
    });
    cpptb::detail::emit_sim_log({
        .level = cpptb::LogLevel::Error,
        .message = "child message",
        .hierarchy = "TOP.design.child.block",
    });
    passed &= expect("parent runtime receives only its SV log",
                     parent_runtime_sink.records.size(), 1);
    passed &= expect("child runtime receives only its SV log",
                     child_runtime_sink.records.size(), 1);
    cpptb::detail::unregister_sim_log_runtime(&child_runtime_owner);
    cpptb::detail::unregister_sim_log_runtime(&parent_runtime_owner);

    const auto suspended_sim_logging = cpptb::register_test(
        "suspended_sim_logging_test", suspended_sim_logging_test);
    cpptb::coro::Testbench throwing_sim_log_scheduler;
    cpptb::TestResult throwing_sim_log_result;
    ThrowingLogSink throwing_sim_log_sink;
    cpptb::LogHistory throwing_sim_log_history;
    cpptb::detail::SimLogEndpoint throwing_sim_log_endpoint;
    passed &= cpptb::run_registered_test(
        throwing_sim_log_scheduler,
        SimLoggingDut{&throwing_sim_log_endpoint}, throwing_sim_log_result,
        cpptb::RunRequest{
            .test_name = "suspended_sim_logging_test",
            .logging = {.minimum_level = cpptb::LogLevel::Info,
                        .sink = &throwing_sim_log_sink,
                        .history = &throwing_sim_log_history}},
        {}, nullptr, &throwing_sim_log_endpoint);
    passed &= expect("SV sink failure test initially suspends",
                     throwing_sim_log_scheduler.done(), false);
    throwing_sim_log_endpoint.emit({
        .level = cpptb::LogLevel::Info,
        .message = "RTL message reaching throwing sink",
        .scope = "rtl.monitor",
        .source_file = "throwing_monitor.sv",
        .hierarchy = "TOP.dpi_test.i_dut.u_monitor",
        .simulation_time_fs = 750'000,
        .source_line = 41,
    });
    passed &= expect("SV sink failure terminates the test cleanly",
                     throwing_sim_log_scheduler.done(), true);
    passed &= expect("SV sink failure reports test error",
                     throwing_sim_log_result.status == cpptb::TestStatus::Error,
                     true);
    passed &= expect("SV sink failure records one failure",
                     throwing_sim_log_result.failure_records.size(), 1);
    passed &= expect("SV sink failure keeps SV source file",
                     throwing_sim_log_result.failure_records[0].source_file ==
                         "throwing_monitor.sv",
                     true);
    passed &= expect("SV sink failure keeps SV source line",
                     throwing_sim_log_result.failure_records[0].source_line,
                     41);
    passed &= expect("SV history precedes throwing sink",
                     throwing_sim_log_history.size(), 1);

    disabled_log_factories = 0;
    enabled_log_factories = 0;
    cpptb::coro::Testbench logging_off_scheduler;
    cpptb::TestResult logging_off_result;
    CaptureLogSink logging_off_sink;
    cpptb::LogHistory logging_off_history;
    passed &= cpptb::run_registered_test(
        logging_off_scheduler,
        LoggingDut{&disabled_log_factories, &enabled_log_factories,
                   &debug_log_enabled, &error_log_enabled},
        logging_off_result,
        cpptb::RunRequest{
            .logging = {.minimum_level = cpptb::LogLevel::Off,
                        .sink = &logging_off_sink,
                        .history = &logging_off_history}});
    logging_off_scheduler.set_time(2);
    passed &= expect("off logging emits nothing",
                     logging_off_sink.records.size(), 0);
    passed &= expect("off logging accumulates nothing",
                     logging_off_history.size(), 0);
    passed &= expect("off logging invokes no factories",
                     disabled_log_factories + enabled_log_factories, 0);
    passed &= expect("off disables every level",
                     debug_log_enabled || error_log_enabled, false);

    const auto retained_logging = cpptb::register_test(
        "retained_logging_test", retained_logging_test);
    cpptb::coro::Testbench retained_logging_scheduler;
    cpptb::TestResult retained_logging_result;
    CaptureLogSink retained_logging_sink;
    std::optional<cpptb::Logger> retained_logger;
    passed &= cpptb::run_registered_test(
        retained_logging_scheduler, RetainedLoggingDut{&retained_logger},
        retained_logging_result,
        cpptb::RunRequest{
            .logging = {.minimum_level = cpptb::LogLevel::Trace,
                        .sink = &retained_logging_sink}});
    passed &= expect("retained logging test completed",
                     retained_logging_result.finished, true);
    passed &= expect("finished logger reports disabled",
                     retained_logger->enabled(cpptb::LogLevel::Error), false);
    uint32_t finished_log_factories = 0;
    retained_logger->debug([&] {
        ++finished_log_factories;
        return std::string{"late debug"};
    });
    retained_logger->error("late error");
    passed &= expect("finished logger invokes no factories",
                     finished_log_factories, 0);
    passed &= expect("finished logger emits no records",
                     retained_logging_sink.records.size(), 0);
    retained_logger.reset();

    const auto throwing_logging = cpptb::register_test(
        "throwing_logging_test", throwing_logging_test);
    cpptb::coro::Testbench throwing_logging_scheduler;
    cpptb::TestResult throwing_logging_result;
    ThrowingLogSink throwing_log_sink;
    cpptb::LogHistory throwing_log_history;
    passed &= cpptb::run_registered_test(
        throwing_logging_scheduler, ThrowingLoggingDut{},
        throwing_logging_result,
        cpptb::RunRequest{
            .logging = {.minimum_level = cpptb::LogLevel::Info,
                        .sink = &throwing_log_sink,
                        .history = &throwing_log_history}});
    passed &= expect("throwing log sink reports test error",
                     throwing_logging_result.status == cpptb::TestStatus::Error,
                     true);
    passed &= expect("throwing log sink records exception",
                     throwing_logging_result.failure_records.size(), 1);
    passed &= expect("throwing log sink preserves message",
                     throwing_logging_result.failure_records[0].label ==
                         "log sink failure",
                     true);
    passed &= expect("history preserves record before sink failure",
                     throwing_log_history.size(), 1);
    passed &= expect("history preserves failing sink message",
                     throwing_log_history[0].message == "sink failure", true);

    bool skip_continued = false;
    const auto skipped = cpptb::register_test("skip_test", skip_test);
    cpptb::coro::Testbench skip_scheduler;
    cpptb::TestResult skip_result;
    passed &= cpptb::run_registered_test(
        skip_scheduler, SkipDut{&skip_continued}, skip_result);
    passed &= expect("dynamic skip status",
                     skip_result.status == cpptb::TestStatus::Skipped, true);
    passed &= expect("dynamic skip stops body", skip_continued, false);
    passed &= expect("skip is successful outcome",
                     cpptb::test_status_successful(skip_result.status), true);
    passed &= expect("dynamic skip reason",
                     skip_result.status_reason == "feature is unavailable",
                     true);

    const auto static_skipped = cpptb::register_test(
        "static_skip_test", static_skip_test,
        cpptb::TestOptions{.skip_reason = "unsupported configuration"});
    cpptb::coro::Testbench static_skip_scheduler;
    cpptb::TestResult static_skip_result;
    passed &= cpptb::run_registered_test(
        static_skip_scheduler, StaticSkipDut{}, static_skip_result);
    passed &= expect("static skip status",
                     static_skip_result.status == cpptb::TestStatus::Skipped,
                     true);
    passed &= expect("static skip schedules nothing",
                     static_skip_scheduler.done(), true);

    const auto expected_failure = cpptb::register_test(
        "expected_failure_test", expected_failure_test,
        cpptb::TestOptions{
            .expected_failure = true,
            .expected_failure_reason = "known RTL issue",
        });
    cpptb::coro::Testbench expected_failure_scheduler;
    cpptb::TestResult expected_failure_result;
    passed &= cpptb::run_registered_test(
        expected_failure_scheduler, ExpectedFailureDut{},
        expected_failure_result);
    passed &= expect(
        "expected failure status",
        expected_failure_result.status == cpptb::TestStatus::ExpectedFailure,
        true);
    passed &= expect("expected failure remains successful",
                     cpptb::test_status_successful(
                         expected_failure_result.status),
                     true);

    const auto unexpected_pass = cpptb::register_test(
        "unexpected_pass_test", unexpected_pass_test,
        cpptb::TestOptions{.expected_failure = true});
    cpptb::coro::Testbench unexpected_pass_scheduler;
    cpptb::TestResult unexpected_pass_result;
    passed &= cpptb::run_registered_test(
        unexpected_pass_scheduler, UnexpectedPassDut{},
        unexpected_pass_result);
    passed &= expect(
        "unexpected pass status",
        unexpected_pass_result.status == cpptb::TestStatus::UnexpectedPass,
        true);
    passed &= expect("unexpected pass is not successful",
                     cpptb::test_status_successful(
                         unexpected_pass_result.status),
                     false);
    passed &= expect(
        "unexpected pass diagnostic",
        unexpected_pass_result.failure_records[0].kind ==
            cpptb::FailureKind::UnexpectedPass,
        true);

    bool timeout_completed = false;
    const auto timeout = cpptb::register_test(
        "timeout_test", timeout_test,
        cpptb::TestOptions{
            .simulation_timeout = cpptb::coro::operator""_ns(1),
        });
    cpptb::coro::Testbench timeout_scheduler;
    cpptb::TestResult timeout_result;
    passed &= cpptb::run_registered_test(
        timeout_scheduler, TimeoutDut{&timeout_completed}, timeout_result);
    passed &= expect("timeout initially active", timeout_scheduler.done(),
                     false);
    timeout_scheduler.set_time(1);
    passed &= expect("timeout status",
                     timeout_result.status == cpptb::TestStatus::TimedOut,
                     true);
    passed &= expect("timeout cancels body", timeout_completed, false);
    passed &= expect("timeout failure kind",
                     timeout_result.failure_records[0].kind ==
                         cpptb::FailureKind::Timeout,
                     true);

    const auto small_case = cpptb::register_test_case(
        "parameterized_test", "small", parameterized_test,
        ParameterCase{.value = 3},
        cpptb::TestOptions{.tags = {"smoke", "parameterized"}});
    const auto large_case = cpptb::register_test_case(
        "parameterized_test", "large", parameterized_test,
        ParameterCase{.value = 99},
        cpptb::TestOptions{.tags = {"parameterized"}});
    const auto parameterized_catalog =
        cpptb::registered_tests<ParameterizedDut>();
    passed &= expect("parameterized catalog size",
                     parameterized_catalog.size(), 2);
    passed &= expect("parameterized stable name",
                     parameterized_catalog[0].name ==
                         "parameterized_test[small]",
                     true);
    passed &= expect("parameterized case metadata",
                     parameterized_catalog[0].metadata.case_name == "small",
                     true);
    passed &= expect("parameterized tag metadata",
                     parameterized_catalog[0].metadata.tags.size(), 2);
    uint32_t parameterized_observed = 0;
    cpptb::coro::Testbench parameterized_scheduler;
    cpptb::TestResult parameterized_result;
    passed &= cpptb::run_registered_test(
        parameterized_scheduler, ParameterizedDut{&parameterized_observed},
        parameterized_result,
        cpptb::RunRequest{.test_name = "parameterized_test[large]"});
    passed &= expect("parameterized selected value", parameterized_observed,
                     99);
    passed &= expect("parameterized result case name",
                     parameterized_result.case_name == "large", true);
    passed &= expect("parameterized result tags",
                     parameterized_result.tags.size(), 1);

    const auto spawn_stress = cpptb::register_test(
        "spawn_stress_test", spawn_stress_test);
    cpptb::coro::Testbench spawn_stress_scheduler;
    cpptb::TestResult spawn_stress_result;
    passed &= cpptb::run_registered_test(
        spawn_stress_scheduler, SpawnStressDut{}, spawn_stress_result);
    passed &= expect("spawn stress status",
                     spawn_stress_result.status == cpptb::TestStatus::Passed,
                     true);
    passed &= expect("spawn stress checks", spawn_stress_result.checks, 4'096);
    passed &= expect("spawn stress warning count",
                     spawn_stress_result.warning_records.size(), 1);
    passed &= expect("recycled provenance keeps monotonic process ids",
                     spawn_stress_result.warning_records[0].process_id, 4'097);
    passed &= expect("recycled provenance keeps spawn source",
                     spawn_stress_result.warning_records[0]
                             .process_source_line != 0,
                     true);

    cpptb::coro::Testbench dropped_context_scheduler;
    cpptb::TestResult dropped_context_result;
    bool dropped_context_child_completed = false;
    {
        cpptb::TestContext context{dropped_context_scheduler,
                                   dropped_context_result};
        context.spawn_detached(
            slow_sibling(dropped_context_child_completed));
    }
    dropped_context_scheduler.set_time(100'000'000);
    passed &= expect("dropped context cancels unowned child",
                     dropped_context_child_completed, false);
    passed &= expect("dropped context leaves scheduler idle",
                     dropped_context_scheduler.done(), true);

    const auto duplicate_one = cpptb::register_test(
        "duplicate", duplicate_first);
    const auto duplicate_two = cpptb::register_test(
        "duplicate", duplicate_second);
    cpptb::coro::Testbench duplicate_scheduler;
    cpptb::TestResult duplicate_result;
    passed &= !cpptb::run_registered_test(
        duplicate_scheduler, DuplicateDut{}, duplicate_result,
        cpptb::RunRequest{.test_name = "duplicate"});
    passed &= expect("duplicate selection status",
                     duplicate_result.status == cpptb::TestStatus::Error,
                     true);

    return passed ? 0 : 1;
}
