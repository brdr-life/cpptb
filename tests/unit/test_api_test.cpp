#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>

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

    const char* json_path = "build/cpptb/test_result_test.json";
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
                     json_text.find("\"schema_version\":2") !=
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
