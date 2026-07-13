#include "benchmarks/peripheral_suite/testbenches/cpp_dpi/framework/peripheral_suite.hpp"

namespace cpptb::benchmarks::peripheral_suite {
namespace {

using coro::Delay;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

namespace timer {
constexpr uint32_t kValue = 0;
constexpr uint32_t kControl = 1;
constexpr uint32_t kCompare = 2;

uint32_t addr(uint32_t timer_index, uint32_t reg_index) {
    return ((timer_index & 1u) << 4) | ((reg_index & 3u) << 2);
}
}  // namespace timer

namespace spi {
constexpr uint32_t kStatus = 0;
constexpr uint32_t kClockDiv = 1;
constexpr uint32_t kCommand = 2;
constexpr uint32_t kAddress = 3;
constexpr uint32_t kLength = 4;
constexpr uint32_t kDummy = 5;
constexpr uint32_t kTxFifo = 6;
constexpr uint32_t kRxFifo = 8;
constexpr uint32_t kInterruptConfig = 9;
}  // namespace spi

namespace i2c {
constexpr uint32_t kPrescaler = 0;
constexpr uint32_t kControl = 1;
constexpr uint32_t kStatus = 3;
constexpr uint32_t kTx = 4;
constexpr uint32_t kCommand = 5;
}  // namespace i2c

uint32_t word_addr(uint32_t word_index) { return word_index << 2; }

Task<void> reset_dut(PeripheralTb tb) {
    tb.dut.HRESETn.set(0);
    drive_apb_idle(tb.dut.timer.apb);
    drive_apb_idle(tb.dut.spi.apb);
    drive_apb_idle(tb.dut.i2c.apb);

    tb.dut.spi.status.set(0);
    tb.dut.spi.data_tx_ready.set(1);
    tb.dut.spi.data_rx.set(0);
    tb.dut.spi.data_rx_valid.set(1);
    tb.dut.i2c.scl_pad_i.set(1);
    tb.dut.i2c.sda_pad_i.set(1);

    co_await tb.wait_cycles(8);
    tb.dut.HRESETn.set(1);
    co_await tb.wait_cycles(8);
}

Task<void> timer_register_sequence(PeripheralTb tb) {
    co_await tb.wait_reset_released();
    co_await tb.wait_cycles(2);

    auto apb = tb.timer_apb();
    for (uint32_t i = 0; i < tb.iterations(); ++i) {
        const uint32_t timer_index = i & 1u;
        const uint32_t compare_value = 3u + (i & 7u);
        const uint32_t prescale = (i >> 2) & 3u;
        const uint32_t control = 1u | (prescale << 3);

        co_await apb.write(timer::addr(timer_index, timer::kValue), 0);
        co_await apb.write(timer::addr(timer_index, timer::kCompare),
                           compare_value);
        co_await apb.write(timer::addr(timer_index, timer::kControl), control);
        co_await tb.wait_timer_irq(timer_index * 2u + 1u,
                                   48u + prescale * compare_value * 4u);
        co_await apb.write(timer::addr(timer_index, timer::kControl), 0);

        if ((i % 8u) == 0) {
            co_await apb.write(timer::addr(timer_index, timer::kCompare), 0);
            co_await apb.write(timer::addr(timer_index, timer::kValue),
                               0xffff'fffe);
            co_await apb.write(timer::addr(timer_index, timer::kControl), 1);
            co_await tb.wait_timer_irq(timer_index * 2u, 8);
            co_await apb.write(timer::addr(timer_index, timer::kControl), 0);
        }
    }
}

Task<void> spi_register_sequence(PeripheralTb tb) {
    co_await tb.wait_reset_released();
    co_await tb.wait_cycles(4);

    auto apb = tb.spi_apb();
    for (uint32_t i = 0; i < tb.iterations(); ++i) {
        const uint32_t div = (i * 13u + 7u) & 0xffu;
        const uint32_t command = 0x1100'0000u ^ (i * 0x0101'0101u);
        const uint32_t address = 0x5500'0000u ^ (i * 0x0011'0021u);
        const uint32_t length = ((i & 0xffu) << 24) |
                                (((i + 3u) & 0xffu) << 16) |
                                (((i + 5u) & 0x3fu) << 8) |
                                ((i + 7u) & 0x3fu);
        const uint32_t dummy =
            (0xab00u | (i & 0xffu)) << 16 | (0x1200u | (i & 0xffu));
        const uint32_t interrupt_config =
            0x8000'0000u | ((i & 0x1fu) << 24) |
            (((i + 1u) & 0x1fu) << 16) | (((i + 2u) & 0x1fu) << 8) |
            ((i + 3u) & 0x1fu);
        const uint32_t status = 0xa500'0000u | ((i & 0xfu) << 8) | (i & 0xffu);
        const uint32_t rx_data = 0xcafe'0000u ^ (i * 0x1021u);

        tb.dut.spi.status.set(status);
        tb.dut.spi.data_rx.set(rx_data);
        tb.dut.spi.data_rx_valid.set(1);
        tb.dut.spi.data_tx_ready.set((i & 3u) == 0 ? 0u : 1u);

        co_await apb.write(word_addr(spi::kStatus),
                           ((i & 0xfu) << 8) | (i & 0xfu));
        co_await Delay{1_ps};
        tb.expect_eq("spi csreg", tb.dut.spi.csreg.get(), i & 0xfu);

        co_await apb.write(word_addr(spi::kClockDiv), div);
        co_await Delay{1_ps};
        tb.expect_eq("spi clk_div", tb.dut.spi.clk_div.get(), div);

        co_await apb.write(word_addr(spi::kCommand), command);
        co_await apb.write(word_addr(spi::kAddress), address);
        co_await apb.write(word_addr(spi::kLength), length);
        co_await apb.write(word_addr(spi::kDummy), dummy);
        co_await apb.write(word_addr(spi::kInterruptConfig), interrupt_config);

        co_await Delay{1_ps};
        tb.expect_eq("spi command", tb.dut.spi.cmd.get(), command);
        tb.expect_eq("spi address", tb.dut.spi.addr.get(), address);
        tb.expect_eq("spi cmd_len", tb.dut.spi.cmd_len.get(), length & 0x3fu);
        tb.expect_eq("spi addr_len", tb.dut.spi.addr_len.get(),
                     (length >> 8) & 0x3fu);
        tb.expect_eq("spi data_len", tb.dut.spi.data_len.get(),
                     (length >> 16) & 0xffffu);
        tb.expect_eq("spi dummy_rd", tb.dut.spi.dummy_rd.get(),
                     dummy & 0xffffu);
        tb.expect_eq("spi dummy_wr", tb.dut.spi.dummy_wr.get(),
                     (dummy >> 16) & 0xffffu);

        co_await apb.write(word_addr(spi::kTxFifo), 0x1357'0000u | i);
        co_await apb.read_expect(word_addr(spi::kRxFifo), rx_data);
        co_await apb.read_expect(word_addr(spi::kStatus), status);
        co_await apb.read_expect(word_addr(spi::kCommand), command);
        co_await apb.read_expect(word_addr(spi::kAddress), address);
        co_await apb.read_expect(word_addr(spi::kLength), length & 0xffff'3f3fu);
        co_await apb.read_expect(word_addr(spi::kDummy), dummy);
    }
}

Task<void> i2c_register_sequence(PeripheralTb tb) {
    co_await tb.wait_reset_released();
    co_await tb.wait_cycles(6);

    auto apb = tb.i2c_apb();
    for (uint32_t i = 0; i < tb.iterations(); ++i) {
        const uint32_t prescaler = 4u + (i & 7u);
        const uint32_t tx_data = 0x40u | (i & 0x3fu);

        tb.dut.i2c.scl_pad_i.set(1);
        tb.dut.i2c.sda_pad_i.set(1);

        co_await apb.write(word_addr(i2c::kPrescaler), prescaler);
        co_await apb.write(word_addr(i2c::kControl), 0xc0);
        co_await apb.write(word_addr(i2c::kTx), tx_data);
        co_await apb.read_expect(word_addr(i2c::kPrescaler), prescaler);
        co_await apb.read_expect(word_addr(i2c::kControl), 0xc0);
        co_await apb.read_expect(word_addr(i2c::kTx), tx_data);

        co_await apb.write(word_addr(i2c::kCommand), 0x90);
        co_await tb.wait_cycles(20u + (i & 0xfu));
        co_await apb.read_ok(word_addr(i2c::kStatus));
        co_await apb.write(word_addr(i2c::kCommand), 0x01);

        if ((i & 3u) == 0) {
            tb.dut.i2c.sda_pad_i.set(0);
            co_await tb.wait_cycles(4);
            tb.dut.i2c.sda_pad_i.set(1);
            co_await tb.wait_cycles(4);
        }
    }
}

Task<void> timer_irq_monitor(PeripheralTb tb) {
    co_await tb.wait_reset_released();
    while (tb.sequences_running()) {
        co_await RisingEdge{tb.dut.HCLK};
        co_await Delay{1_ps};
        tb.expect_eq("timer irq high bits", tb.dut.timer.irq.get() & ~0xfu, 0);
    }
}

Task<void> spi_handshake_monitor(PeripheralTb tb) {
    co_await tb.wait_reset_released();
    while (tb.sequences_running()) {
        co_await RisingEdge{tb.dut.HCLK};
        co_await Delay{1_ps};
        tb.expect_eq("spi clk_div_valid", tb.dut.spi.clk_div_valid.get() <= 1 ? 1u : 0u,
                     1);
        tb.expect_eq("spi data_tx_valid", tb.dut.spi.data_tx_valid.get() <= 1 ? 1u : 0u,
                     1);
        tb.expect_eq("spi data_rx_ready", tb.dut.spi.data_rx_ready.get() <= 1 ? 1u : 0u,
                     1);
    }
}

Task<void> i2c_pad_monitor(PeripheralTb tb) {
    co_await tb.wait_reset_released();
    while (tb.sequences_running()) {
        co_await RisingEdge{tb.dut.HCLK};
        co_await Delay{1_ps};
        tb.expect_eq("i2c interrupt", tb.dut.i2c.interrupt.get() <= 1 ? 1u : 0u, 1);
        tb.expect_eq("i2c scl_padoen", tb.dut.i2c.scl_padoen_o.get() <= 1 ? 1u : 0u,
                     1);
        tb.expect_eq("i2c sda_padoen", tb.dut.i2c.sda_padoen_o.get() <= 1 ? 1u : 0u,
                     1);
    }
}

}  // namespace

void register_user_testbench(PeripheralTb& tb) {
    tb.reset(reset_dut);

    tb.sequence(timer_register_sequence);
    tb.sequence(spi_register_sequence);
    tb.sequence(i2c_register_sequence);

    tb.monitor(timer_irq_monitor);
    tb.monitor(spi_handshake_monitor);
    tb.monitor(i2c_pad_monitor);
}

}  // namespace cpptb::benchmarks::peripheral_suite
