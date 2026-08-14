#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "cpptb/cpptb.hpp"
#include "cpptb_vc/cpptb_vc.hpp"
#include "dut.hpp"
#include "ipxact_regs.hpp"

namespace cpptb::examples::ipxact_regfile {
namespace {

using cpptb::Dut;
using coro::Task;
using namespace coro;
using namespace cpptb::vc;

auto make_apb_bus(Dut dut) {
    return ApbBus{dut.clk,           dut.apb_select,    dut.apb_enable,
                  dut.apb_write,     dut.apb_address,   dut.apb_write_data,
                  dut.apb_read_data, dut.apb_ready,     dut.apb_error,
                  dut.apb_strobe};
}

using Bus = decltype(make_apb_bus(std::declval<Dut>()));
using Master = ApbMaster<
    decltype(std::declval<Bus>().clock),
    decltype(std::declval<Bus>().select),
    decltype(std::declval<Bus>().enable),
    decltype(std::declval<Bus>().write),
    decltype(std::declval<Bus>().address),
    decltype(std::declval<Bus>().write_data),
    decltype(std::declval<Bus>().read_data),
    decltype(std::declval<Bus>().ready),
    decltype(std::declval<Bus>().error),
    decltype(std::declval<Bus>().strobe)>;

Task<void> reset_dut(Dut dut) {
    dut.rst_n.set(0);
    dut.apb_select.set(0);
    dut.apb_enable.set(0);
    dut.apb_write.set(0);
    dut.apb_address.set(0);
    dut.apb_write_data.set(0);
    dut.apb_strobe.set(0);
    co_await clock_cycles(dut.clk, 2);
    dut.rst_n.set(1);
}

Task<void> ipxact_register_model_test(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
    test.start_clock(dut.clk, 10_ns);
    co_await reset_dut(dut);

    const auto bus = make_apb_bus(dut);
    Master master{bus};
    ipxact_regs::RegModel regs{test, master};

    const auto reset_control = co_await regs.registers.control.read();
    test.require("control reset read", reset_control.okay());
    test.expect_eq("control reset", reset_control.data, 0u);

    const auto control_write = co_await regs.registers.control.write(0x5u);
    test.require("control write", control_write.okay());
    const auto control_read = co_await regs.registers.control.read();
    test.expect_eq("control readback", control_read.data, 0x5u);

    regs.registers.control.enable.stage(1u);
    regs.registers.control.mode.stage(
        ipxact_regs::mode_enum_t::STREAM);
    const auto field_update = co_await regs.registers.control.update();
    test.require("control field update", field_update.okay());

    const auto status = co_await regs.registers.status.read();
    test.expect_eq("live status", status.data, 0x3u);

    const auto threshold_write =
        co_await regs.registers.threshold.at<2>().write(0x1234u);
    test.require("threshold write", threshold_write.okay());
    const auto threshold =
        co_await regs.registers.threshold.at<2>().read();
    test.expect_eq("threshold readback", threshold.data, 0x1234u);

    constexpr std::array<uint32_t, 3> packet{
        0x1122'3344u, 0xa5a5'5a5au, 0xcafe'babeu};
    const auto memory_write = co_await regs.scratchpad.write(
        4, std::span<const uint32_t>{packet});
    test.require("scratchpad write", memory_write.okay());

    std::array<uint32_t, packet.size()> readback{};
    const auto memory_read = co_await regs.scratchpad.read(
        4, std::span<uint32_t>{readback});
    test.require("scratchpad read", memory_read.okay());
    test.expect_eq("scratchpad readback", readback, packet);

    test.expect_eq("register path", regs.registers.control.path(),
                   std::string_view{"peripheral.registers.control"});
    test.expect_eq("memory path", regs.scratchpad.path(),
                   std::string_view{"peripheral.scratchpad"});
}

CPPTB_REGISTER_TEST(ipxact_register_model_test);

}  // namespace
}  // namespace cpptb::examples::ipxact_regfile
