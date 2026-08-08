#include "cpptb/cpptb.hpp"
#include "dut.hpp"

using cpptb::Dut;
using cpptb::TestContext;
using cpptb::coro::Delay;
using cpptb::coro::RisingEdge;
using cpptb::coro::Task;
using cpptb::coro::operator""_ns;

Task<void> interface_test(Dut dut, TestContext& test) {
    dut.links[0].clk.set(0);
    dut.links[1].clk.set(0);
    test.start_clock(dut.links[0].clk, 10_ns);
    test.start_clock(dut.links[1].clk, 14_ns);

    for (int index = 0; index < 2; ++index) {
        dut.links[index].reset_n.set(0);
        dut.links[index].valid.set(0);
        dut.links[index].data.set(0);
        dut.links[index].sideband.high_z();
    }
    dut.gpio_drive.set(0);
    dut.gpio.high_z();

    co_await RisingEdge{dut.links[0].clk};
    test.expect_eq("link zero reset ready", dut.links[0].ready.get(), 0u);
    test.expect_eq("link one reset ready", dut.links[1].ready.get(), 0u);

    dut.links[0].reset_n.set(1);
    dut.links[1].reset_n.set(1);
    dut.links[0].data.set(0x24);
    dut.links[1].data.set(0x35);
    // Combinational pacing through absolute delays: each step gives the
    // links a nanosecond to settle, the same shape a cocotb Timer gives.
    co_await Delay{1_ns};
    test.expect_eq("link zero observed", dut.links[0].observed.get(), 0x24u);
    test.expect_eq("link one observed", dut.links[1].observed.get(), 0x36u);

    dut.links[0].sideband.drive(1);
    co_await Delay{1_ns};
    test.expect_eq("testbench interface drive",
                   dut.links[0].sideband.get(), 1u);
    dut.links[0].sideband.high_z();
    dut.links[0].valid.set(1);
    dut.links[0].data.set(0x22);
    co_await Delay{1_ns};
    test.expect_eq("released interface drive",
                   dut.links[0].sideband.get(), 0u);

    dut.gpio.drive(0x5);
    co_await Delay{1_ns};
    test.expect_eq("testbench top-level inout", dut.gpio_seen.get(), 0x5u);
    dut.gpio.high_z();
    dut.gpio_drive.set(1);
    co_await Delay{1_ns};
    test.expect_eq("released top-level inout", dut.gpio.get(), 0xau);
}

CPPTB_REGISTER_TEST(interface_test);
