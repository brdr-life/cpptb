#include <cstdint>

#include "cpptb/cpptb.hpp"
#include "examples/counter/generated/counter_dut.hpp"

namespace cpptb::examples::counter {
namespace {

using cpptb::generated::counter::Dut;
using coro::Delay;
using coro::FallingEdge;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

constexpr uint32_t kCountCycles = 8;

Task<void> counter_sequence(Dut dut, TestContext& test) {
    dut.clk.set(0);
    test.start_clock(dut.clk, 10_ns);

    dut.rst_n.set(0);
    dut.enable.set(0);

    co_await clock_cycles(dut.clk, 2);
    co_await FallingEdge{dut.clk};

    dut.rst_n.set(1);
    dut.enable.set(1);

    for (uint32_t expected = 1; expected <= kCountCycles; ++expected) {
        co_await RisingEdge{dut.clk};
        co_await Delay{1_ps};
        test.expect_eq("enabled count", dut.count.get(), expected);
    }

    dut.enable.set(0);
    co_await RisingEdge{dut.clk};
    co_await Delay{1_ps};
    test.expect_eq("disabled count", dut.count.get(), kCountCycles);
}

CPPTB_REGISTER_TEST(counter_sequence);

}  // namespace

}  // namespace cpptb::examples::counter
