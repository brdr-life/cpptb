#include "examples/multiclock/framework.hpp"

namespace cpptb::examples::dpi_multiclock {
namespace {

using coro::Edge;
using coro::Delay;
using coro::First;
using coro::Join;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

Task<void> reset_dut(MulticlockTb tb) {
    tb.dut.rst_n.set(0);
    tb.dut.write.valid.set(0);
    tb.dut.write.data.set(0);
    tb.dut.read.ready.set(0);
    tb.dut.probe.in.set(0);

    co_await Delay{20_ns};
    tb.expect_eq("reset delay deadline",
                 static_cast<uint32_t>(tb.now().in_nanoseconds()), 20);
    tb.dut.rst_n.set(1);
}

Task<void> wait_reset_write(MulticlockTb tb) {
    while (tb.dut.rst_n.get() == 0) {
        co_await RisingEdge{tb.dut.write.clk};
    }
}

Task<void> wait_reset_read(MulticlockTb tb) {
    while (tb.dut.rst_n.get() == 0) {
        co_await RisingEdge{tb.dut.read.clk};
    }
}

Task<void> producer(MulticlockTb tb) {
    co_await wait_reset_write(tb);

    for (uint32_t value = 0; value < tb.iterations(); ++value) {
        while (true) {
            co_await RisingEdge{tb.dut.write.clk};
            co_await Delay{1_ps};
            if (tb.dut.write.ready.get() != 0) break;
        }

        tb.dut.write.data.set((0x40u + value) & 0xffu);
        tb.dut.write.valid.set(1);

        co_await RisingEdge{tb.dut.write.clk};
        co_await Delay{1_ps};
        tb.dut.write.valid.set(0);
    }
}

Task<void> consumer(MulticlockTb tb) {
    co_await wait_reset_read(tb);

    for (uint32_t expected = 0; expected < tb.iterations(); ++expected) {
        while (true) {
            co_await RisingEdge{tb.dut.read.clk};
            co_await Delay{1_ps};
            if (tb.dut.read.valid.get() != 0) break;
        }

        tb.expect_eq("mailbox payload", tb.dut.read.data.get(),
                     (0x40u + expected) & 0xffu);
        tb.dut.read.ready.set(1);

        co_await RisingEdge{tb.dut.read.clk};
        co_await Delay{1_ps};
        tb.dut.read.ready.set(0);
    }
}

Task<void> traffic(MulticlockTb tb) {
    co_await Join{producer(tb), consumer(tb)};
    tb.expect_eq("write count", tb.dut.write.count.get(), tb.iterations());
    tb.expect_eq("read count", tb.dut.read.count.get(), tb.iterations());
}

Task<void> trigger_and_phase_probe(MulticlockTb tb) {
    co_await Delay{7_ns};
    tb.expect_eq("independent delay",
                 static_cast<uint32_t>(tb.now().in_nanoseconds()), 7);

    const auto winner =
        co_await First{RisingEdge{tb.dut.read.clk}, Delay{100_ns}};
    tb.expect_eq("First chose read clock", static_cast<uint32_t>(winner), 0);

    co_await Edge{tb.dut.write.clk};
    co_await Delay{1_ps};
    tb.dut.probe.in.set(0xa5);
    co_await Delay{1_ps};
    tb.expect_eq("delay settles combinational output",
                 tb.dut.probe.echo.get(), 0xa5);

    tb.dut.probe.in.set(0x3c);
    co_await Delay{1_ps};
    tb.expect_eq("successive delay settles next drive",
                 tb.dut.probe.echo.get(), 0x3c);
}

}  // namespace

void register_user_testbench(MulticlockTb& tb) {
    tb.sequence(reset_dut);
    tb.sequence(traffic);
    tb.sequence(trigger_and_phase_probe);
}

}  // namespace cpptb::examples::dpi_multiclock
