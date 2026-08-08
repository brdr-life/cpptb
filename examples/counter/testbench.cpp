#include <cstdint>

#include "cpptb/cpptb.hpp"
#include "dut.hpp"

namespace cpptb::examples::counter {
namespace {

using cpptb::Dut;
using coro::NextTimeStep;
using coro::ReadOnly;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

constexpr uint32_t kCountCycles = 8;

Task<void> counter_sequence(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
    test.start_clock(dut.clk, 10_ns);

    dut.rst_n.set(0);
    dut.enable.set(0);

    // The cocotb shape: write straight after the awaited edge; the deferred
    // flush applies it after this edge's own updates, so the release is
    // seen by the next rising edge -- no negedge detour.
    co_await clock_cycles(dut.clk, 2);
    dut.rst_n.set(1);
    dut.enable.set(1);

    for (uint32_t expected = 1; expected <= kCountCycles; ++expected) {
        co_await RisingEdge{dut.clk};
        co_await ReadOnly{};
        test.expect_eq("enabled count", dut.count.get(), expected);
    }

    // Writes are illegal inside ReadOnly (cocotb's rule too); step out of
    // the region first.
    co_await NextTimeStep{};
    dut.enable.set(0);
    co_await RisingEdge{dut.clk};
    co_await ReadOnly{};
    test.expect_eq("disabled count", dut.count.get(), kCountCycles);
}

Task<void> counter_reset_defaults(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
    test.start_clock(dut.clk, 10_ns);

    dut.rst_n.set(0);
    dut.enable.set(1);

    co_await RisingEdge{dut.clk};
    co_await ReadOnly{};
    test.expect_eq("reset count", dut.count.get(), 0u);
}

CPPTB_REGISTER_TEST(counter_sequence);
CPPTB_REGISTER_TEST(counter_reset_defaults);

}  // namespace

}  // namespace cpptb::examples::counter
