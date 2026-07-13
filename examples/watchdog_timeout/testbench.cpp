#include "examples/watchdog_timeout/framework.hpp"

namespace cpptb::examples::watchdog_timeout {
namespace {

using coro::Delay;
using coro::FallingEdge;
using coro::RisingEdge;
using coro::Task;
using coro::TimeoutOutcome;
using namespace coro;

constexpr uint32_t kResponseMask = 0xa5a5'5a5au;

uint32_t next_word(uint32_t& state) {
    state = state * 1'664'525u + 1'013'904'223u;
    return state;
}

Task<void> reset_dut(WatchdogTimeoutTb tb) {
    tb.dut.rst_n.set(0);
    tb.dut.request.set(0);
    tb.dut.stall.set(0);
    tb.dut.latency.set(0);
    tb.dut.request_payload.data.set(0);

    co_await clock_cycles(tb.dut.clk, 2);
    co_await FallingEdge{tb.dut.clk};
    tb.dut.rst_n.set(1);
}

Task<void> drive_request(WatchdogTimeoutTb tb, uint32_t data,
                         uint32_t latency, bool stall) {
    co_await FallingEdge{tb.dut.clk};
    tb.dut.request_payload.data.set(data);
    tb.dut.latency.set(latency);
    tb.dut.stall.set(stall ? 1u : 0u);
    tb.dut.request.set(1);

    co_await RisingEdge{tb.dut.clk};
    co_await Delay{1_ps};
    tb.dut.request.set(0);
}

Task<uint32_t> transaction(WatchdogTimeoutTb tb, uint32_t data,
                           uint32_t latency, bool stall) {
    co_await drive_request(tb, data, latency, stall);
    co_await RisingEdge{tb.dut.response.valid};
    co_await Delay{1_ps};
    co_return tb.dut.response.data.get();
}

Task<void> dormant_monitor(WatchdogTimeoutTb tb) {
    // The stalled DUT cannot respond; this process exists to demonstrate
    // explicit cancellation of a permanently parked monitor.
    while (true) co_await RisingEdge{tb.dut.response.valid};
}

Task<void> watchdog_sequence(WatchdogTimeoutTb tb) {
    co_await reset_dut(tb);
    uint32_t state = 0x5566'7788u;

    for (uint32_t index = 0; index < tb.iterations(); ++index) {
        const uint32_t word = next_word(state);
        auto response = co_await with_timeout(
            transaction(tb, word, 2u + index % 3u, false), 200_ns);
        tb.expect_eq("transaction completed", response.has_value(), 1);
        if (response) {
            tb.expect_eq("transaction response", response.value(),
                         word ^ kResponseMask);
        }
    }

    const uint32_t edge_word = next_word(state);
    co_await drive_request(tb, edge_word, 3, false);
    const auto edge_outcome =
        co_await with_timeout(RisingEdge{tb.dut.response.valid}, 100_ns);
    tb.expect_eq("response edge beat deadline",
                 edge_outcome == TimeoutOutcome::Triggered, 1);
    co_await Delay{1_ps};
    tb.expect_eq("response after edge timeout", tb.dut.response.data.get(),
                 edge_word ^ kResponseMask);

    const uint32_t stalled_word = next_word(state);
    auto stalled = co_await with_timeout(
        transaction(tb, stalled_word, 2, true), 60_ns);
    tb.expect_eq("stalled transaction timed out", stalled.timed_out(), 1);

    auto monitor = tb.spawn(dormant_monitor(tb));
    co_await Delay{3_ns};
    monitor.cancel();
    co_await monitor;
    tb.expect_eq("cancelled monitor reports cancellation",
                 monitor.cancelled(), 1);
}

}  // namespace

void register_user_testbench(WatchdogTimeoutTb& tb) {
    tb.run(watchdog_sequence(tb));
}

}  // namespace cpptb::examples::watchdog_timeout
