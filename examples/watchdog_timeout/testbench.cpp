#include <cstdint>

#include "cpptb/cpptb.hpp"
#include "examples/watchdog_timeout/generated/stalling_responder_dut.hpp"

namespace cpptb::examples::watchdog_timeout {
namespace {

using cpptb::generated::stalling_responder::Dut;
using coro::Delay;
using coro::FallingEdge;
using coro::RisingEdge;
using coro::Task;
using coro::TimeoutOutcome;
using namespace coro;

constexpr uint32_t kTransactionCount = 8;
constexpr uint32_t kResponseMask = 0xa5a5'5a5au;

uint32_t next_word(uint32_t& state) {
    state = state * 1'664'525u + 1'013'904'223u;
    return state;
}

Task<void> reset_dut(Dut dut) {
    dut.rst_n.set(0);
    dut.request.set(0);
    dut.stall.set(0);
    dut.latency.set(0);
    dut.request_data.set(0);

    co_await clock_cycles(dut.clk, 2);
    co_await FallingEdge{dut.clk};
    dut.rst_n.set(1);
}

Task<void> drive_request(Dut dut, uint32_t data, uint32_t latency,
                         bool stall) {
    co_await FallingEdge{dut.clk};
    dut.request_data.set(data);
    dut.latency.set(latency);
    dut.stall.set(stall ? 1u : 0u);
    dut.request.set(1);

    co_await RisingEdge{dut.clk};
    co_await Delay{1_ps};
    dut.request.set(0);
}

Task<uint32_t> transaction(Dut dut, uint32_t data, uint32_t latency,
                           bool stall) {
    co_await drive_request(dut, data, latency, stall);
    co_await RisingEdge{dut.response_valid};
    co_await Delay{1_ps};
    co_return dut.response_data.get();
}

Task<void> dormant_monitor(Dut dut) {
    while (true) co_await RisingEdge{dut.response_valid};
}

Task<void> watchdog_sequence(Dut dut, TestContext& test) {
    dut.clk.set(0);
    test.start_clock(dut.clk, 10_ns);

    co_await reset_dut(dut);
    uint32_t state = 0x5566'7788u;

    for (uint32_t index = 0; index < kTransactionCount; ++index) {
        const uint32_t word = next_word(state);
        auto response = co_await with_timeout(
            transaction(dut, word, 2u + index % 3u, false), 200_ns);
        test.expect_eq("transaction completed", response.has_value(), true);
        if (response) {
            test.expect_eq("transaction response", response.value(),
                           word ^ kResponseMask);
        }
    }

    const uint32_t edge_word = next_word(state);
    co_await drive_request(dut, edge_word, 3, false);
    const auto edge_outcome =
        co_await with_timeout(RisingEdge{dut.response_valid}, 100_ns);
    test.expect_eq("response edge beat deadline",
                   edge_outcome == TimeoutOutcome::Triggered, true);
    co_await Delay{1_ps};
    test.expect_eq("response after edge timeout", dut.response_data.get(),
                   edge_word ^ kResponseMask);

    const uint32_t stalled_word = next_word(state);
    auto stalled = co_await with_timeout(
        transaction(dut, stalled_word, 2, true), 60_ns);
    test.expect_eq("stalled transaction timed out", stalled.timed_out(), true);

    auto monitor = test.spawn(dormant_monitor(dut));
    co_await Delay{3_ns};
    monitor.cancel();
    co_await monitor;
    test.expect_eq("cancelled monitor reports cancellation",
                   monitor.cancelled(), true);
}

CPPTB_REGISTER_TEST(watchdog_sequence);

}  // namespace
}  // namespace cpptb::examples::watchdog_timeout
