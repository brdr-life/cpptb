#include "cpptb/cpptb.hpp"
#include "dut.hpp"

using cpptb::TestContext;
using cpptb::coro::Delay;
using cpptb::coro::RisingEdge;
using cpptb::coro::Task;
using cpptb::coro::operator""_ns;
using cpptb::generated::interface_catalog::Dut;

Task<void> interface_catalog_sequence(Dut dut, TestContext& test) {
    dut.links[0].clk.set(0);
    dut.links[1].clk.set(0);
    dut.links[2].clk.set(0);
    test.start_clock(dut.links[0].clk, 10_ns);
    test.start_clock(dut.links[1].clk, 14_ns);

    dut.links[0].reset_n.set(0);
    dut.links[1].reset_n.set(0);
    dut.links[2].reset_n.set(0);
    dut.links[0].valid.set(0);
    dut.links[1].valid.set(0);
    dut.links[2].valid.set(0);
    dut.links[0].data.set(0);
    dut.links[1].data.set(0);
    dut.links[2].data.set(0);
    dut.gpio_drive.set(0);
    dut.gpio.high_z();
    dut.wide_gpio.high_z();
    dut.links[0].sideband.high_z();
    dut.links[1].sideband.high_z();

    co_await RisingEdge{dut.links[0].clk};
    test.expect_eq("link zero reset ready", dut.links[0].ready.get(), 0u);
    test.expect_eq("link one reset ready", dut.links[1].ready.get(), 0u);

    dut.links[2].clk.set(1);
    co_await Delay{1_ns};
    test.expect_eq("unscheduled interface clock remains directly driven",
                   dut.links[2].clock_seen.get(), 1u);

    dut.links[0].reset_n.set(1);
    dut.links[1].reset_n.set(1);
    dut.links[0].data.set(0x24);
    dut.links[1].data.set(0x35);
    co_await Delay{1_ns};
    test.expect_eq("link zero ready", dut.links[0].ready.get(), 1u);
    test.expect_eq("link one ready", dut.links[1].ready.get(), 1u);
    test.expect_eq("link zero observed", dut.links[0].observed.get(), 0x24u);
    test.expect_eq("link one observed", dut.links[1].observed.get(), 0x36u);

    dut.links[0].sideband.drive(1);
    co_await Delay{1_ns};
    test.expect_eq("testbench drives interface inout",
                   dut.links[0].sideband.get(), 1u);
    dut.links[0].sideband.high_z();
    dut.links[0].valid.set(1);
    dut.links[0].data.set(0x22);
    co_await Delay{1_ns};
    test.expect_eq("DUT drives released interface inout",
                   dut.links[0].sideband.get(), 0u);

    dut.gpio.drive(0x5);
    co_await Delay{1_ns};
    test.expect_eq("testbench drives top inout", dut.gpio_seen.get(), 0x5u);
    dut.gpio.high_z();
    dut.gpio_drive.set(1);
    co_await Delay{1_ns};
    test.expect_eq("DUT drives released top inout", dut.gpio.get(), 0xau);

    const auto wide_drive = cpptb::Bits<65>::from_words(
        {0x89ab'cdefu, 0x0123'4567u, 1u});
    dut.wide_gpio.drive(wide_drive);
    co_await Delay{1_ns};
    test.expect_eq("testbench drives wide top inout",
                   dut.wide_gpio_seen.get(), wide_drive);
    dut.wide_gpio.high_z();
}

CPPTB_REGISTER_TEST(interface_catalog_sequence);
