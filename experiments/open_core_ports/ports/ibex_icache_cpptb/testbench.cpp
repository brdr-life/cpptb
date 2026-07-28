#include "cpptb/cpptb.hpp"
#include "dut.hpp"
namespace {
using cpptb::Dut;
using namespace cpptb::coro;
Task<void> smoke(Dut dut, cpptb::TestContext& test) {
    dut.clk_i.set(0);
    dut.rst_ni.set(1);
    test.start_clock(dut.clk_i, 20_ns);
    dut.scr_key_valid_i.set(1);
    co_await clock_cycles(dut.clk_i, 2);
    dut.rst_ni.set(0);
    co_await clock_cycles(dut.clk_i, 4);
    dut.rst_ni.set(1);
    co_await clock_cycles(dut.clk_i, 10);
    test.expect("ran", true);
}
CPPTB_REGISTER_TEST(smoke);
}
