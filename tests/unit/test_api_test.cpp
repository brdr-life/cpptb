#include <cstdint>
#include <cstdio>
#include <cstring>

#include "cpptb/test_api.hpp"

namespace {

struct UniqueDut {
    uint32_t value;
};

struct AmbiguousDut {};

struct MissingDut {};

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

    checking_context.expect_eq("signed mismatch", -7, 8);
    passed &= expect("failed check count", checking_result.checks, 1);
    passed &= expect("failed check failure count", checking_result.failures, 1);

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

    return passed ? 0 : 1;
}
