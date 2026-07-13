#include "examples/counter/framework.hpp"

namespace cpptb::examples::counter {
namespace {

using coro::Delay;
using coro::FallingEdge;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

Task<void> counter_sequence(CounterTb tb) {
    tb.dut.rst_n.set(0);
    tb.dut.enable.set(0);

    co_await clock_cycles(tb.dut.clk, 2);
    co_await FallingEdge{tb.dut.clk};

    tb.dut.rst_n.set(1);
    tb.dut.enable.set(1);

    for (uint32_t expected = 1; expected <= tb.iterations(); ++expected) {
        co_await RisingEdge{tb.dut.clk};
        co_await Delay{1_ps};
        tb.expect_eq("enabled count", tb.dut.count.get(), expected);
    }

    tb.dut.enable.set(0);
    co_await RisingEdge{tb.dut.clk};
    co_await Delay{1_ps};
    tb.expect_eq("disabled count", tb.dut.count.get(), tb.iterations());
}

}  // namespace

void register_user_testbench(CounterTb& tb) {
    tb.run(counter_sequence(tb));
}

}  // namespace cpptb::examples::counter
