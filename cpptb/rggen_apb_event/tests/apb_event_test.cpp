#include "cpptb/rggen_apb_event/tests/apb_event_test.hpp"

#include <cstdint>
#include <cstdio>
#include <string_view>

namespace cpptb::rggen_apb_event::tests {
namespace {

using coro::FallingEdge;
using coro::RisingEdge;
using coro::Task;

constexpr uint32_t kIrqEnable = 0x000;
constexpr uint32_t kIrqPending = 0x004;
constexpr uint32_t kIrqSetPending = 0x008;
constexpr uint32_t kIrqClearPending = 0x00c;
constexpr uint32_t kEventEnable = 0x010;
constexpr uint32_t kEventPending = 0x014;
constexpr uint32_t kEventClearPending = 0x01c;
constexpr uint32_t kSleepCtrl = 0x020;
constexpr uint32_t kSleepStatus = 0x024;

Task<void> wait_cycles(coro::Signal clock, uint32_t cycles) {
    for (uint32_t i = 0; i < cycles; ++i) {
        co_await RisingEdge{clock};
    }
}

Task<void> wait_reset_released(ApbEventDut dut) {
    while (dut.HRESETn.get() == 0) {
        co_await RisingEdge{dut.HCLK};
    }
}

class ApbMaster {
   public:
    ApbMaster(coro::Testbench& tb, ApbEventDut dut) : tb_(tb), dut_(dut) {}

    Task<void> write(std::string_view label, uint32_t byte_addr, uint32_t data) const {
        co_await FallingEdge{dut_.HCLK};
        dut_.PADDR.set(byte_addr & 0xfff);
        dut_.PWDATA.set(data);
        dut_.PWRITE.set(1);
        dut_.PSEL.set(1);
        dut_.PENABLE.set(0);

        co_await FallingEdge{dut_.HCLK};
        dut_.PENABLE.set(1);

        co_await RisingEdge{dut_.HCLK};
        tb_.expect_eq(label, dut_.PREADY.get(), 1);
        tb_.expect_eq("APB PSLVERR", dut_.PSLVERR.get(), 0);

        co_await FallingEdge{dut_.HCLK};
        drive_apb_idle(dut_);
    }

    Task<void> read_expect(std::string_view label, uint32_t byte_addr,
                     uint32_t expected) const {
        co_await FallingEdge{dut_.HCLK};
        dut_.PADDR.set(byte_addr & 0xfff);
        dut_.PWDATA.set(0);
        dut_.PWRITE.set(0);
        dut_.PSEL.set(1);
        dut_.PENABLE.set(0);

        co_await FallingEdge{dut_.HCLK};
        dut_.PENABLE.set(1);

        co_await RisingEdge{dut_.HCLK};
        tb_.expect_eq(label, dut_.PRDATA.get(), expected);
        tb_.expect_eq("APB PREADY", dut_.PREADY.get(), 1);
        tb_.expect_eq("APB PSLVERR", dut_.PSLVERR.get(), 0);

        co_await FallingEdge{dut_.HCLK};
        drive_apb_idle(dut_);
    }

   private:
    coro::Testbench& tb_;
    ApbEventDut dut_;
};

Task<void> reset_driver(coro::Testbench& tb, ApbEventDut dut) {
    dut.HRESETn.set(0);
    dut.irq_i.set(0);
    dut.event_i.set(0);
    dut.fetch_enable_i.set(1);
    dut.core_busy_i.set(0);
    drive_apb_idle(dut);

    co_await wait_cycles(dut.HCLK, 4);
    dut.HRESETn.set(1);
    tb.log("reset released");
    co_await wait_cycles(dut.HCLK, 3);
}

Task<void> apb_register_sequence(coro::Testbench& tb, ApbEventDut dut) {
    co_await wait_reset_released(dut);
    co_await wait_cycles(dut.HCLK, 2);

    const ApbMaster apb{tb, dut};

    co_await apb.read_expect("irq enable reset", kIrqEnable, 0);
    co_await apb.read_expect("irq pending reset", kIrqPending, 0);

    co_await apb.write("write irq enable", kIrqEnable, 0x0000'002a);
    dut.irq_i.set(0x0000'0008);
    co_await wait_cycles(dut.HCLK, 3);
    co_await apb.read_expect("irq pending from irq_i", kIrqPending, 0x0000'0008);

    dut.irq_i.set(0);
    co_await apb.write("write irq set pending", kIrqSetPending, 0x0000'0002);
    co_await wait_cycles(dut.HCLK, 2);
    co_await apb.read_expect("irq pending after set", kIrqPending, 0x0000'000a);

    co_await apb.write("write irq clear pending", kIrqClearPending, 0x0000'000a);
    co_await wait_cycles(dut.HCLK, 2);
    co_await apb.read_expect("irq pending after clear", kIrqPending, 0);

    co_await apb.write("write irq pending direct", kIrqPending, 0x8000'0000);
    co_await wait_cycles(dut.HCLK, 2);
    co_await apb.read_expect("irq pending direct", kIrqPending, 0x8000'0000);
    tb.expect_eq("irq_o direct pending", dut.irq_o.get(), 0x8000'0000);

    co_await apb.write("write event enable", kEventEnable, 0x0000'0011);
    dut.event_i.set(0x0000'0010);
    co_await wait_cycles(dut.HCLK, 3);
    co_await apb.read_expect("event pending from event_i", kEventPending, 0x0000'0010);

    dut.event_i.set(0);
    co_await apb.write("write event clear pending", kEventClearPending, 0x0000'0010);
    co_await wait_cycles(dut.HCLK, 2);
    co_await apb.read_expect("event pending after clear", kEventPending, 0);

    co_await apb.write("sleep enable blocked by irq", kSleepCtrl, 0x0000'0001);
    co_await wait_cycles(dut.HCLK, 4);
    co_await apb.read_expect("sleep status while irq pending", kSleepStatus, 0);

    co_await apb.write("clear direct irq pending", kIrqClearPending, 0x8000'0000);
    co_await wait_cycles(dut.HCLK, 4);
    co_await apb.read_expect("sleep status after irq clear", kSleepStatus, 1);
    tb.expect_eq("clock gated in sleep", dut.clk_gate_core_o.get(), 0);
    tb.expect_eq("fetch disabled in sleep", dut.fetch_enable_o.get(), 0);

    dut.event_i.set(0x0000'0001);
    co_await wait_cycles(dut.HCLK, 4);
    co_await apb.read_expect("sleep ctrl after wake", kSleepCtrl, 0);
    tb.expect_eq("clock ungated after wake", dut.clk_gate_core_o.get(), 1);
    dut.event_i.set(0);
}

Task<void> irq_monitor(coro::Testbench& tb, ApbEventDut dut) {
    co_await wait_reset_released(dut);

    uint32_t previous = 0xffff'ffff;
    for (uint32_t cycle = 0; cycle < 170; ++cycle) {
        co_await RisingEdge{dut.HCLK};
        const auto irq = dut.irq_o.get();
        if (irq != previous) {
            std::printf("cpptb[%llu]: irq_o -> 0x%08x\n",
                        static_cast<unsigned long long>(
                            tb.now().in_nanoseconds()),
                        irq);
            previous = irq;
        }

        if (irq != 0 && (irq & (irq - 1)) != 0) {
            tb.expect_eq("irq_o remains one-hot", irq & (irq - 1), 0);
        }
    }
}

Task<void> sleep_monitor(coro::Testbench& tb, ApbEventDut dut) {
    co_await wait_reset_released(dut);

    for (uint32_t cycle = 0; cycle < 170; ++cycle) {
        co_await RisingEdge{dut.HCLK};
        const auto clock_gate = dut.clk_gate_core_o.get();
        const auto fetch_enable = dut.fetch_enable_o.get();
        if (clock_gate == 0 && fetch_enable != 0) {
            tb.expect_eq("fetch disabled when core clock is gated", fetch_enable, 0);
        }
    }
}

}  // namespace

void register_tests(coro::Testbench& tb, ApbEventDut dut) {
    tb.spawn_detached(reset_driver(tb, dut));
    tb.spawn_detached(apb_register_sequence(tb, dut));
    tb.spawn_detached(irq_monitor(tb, dut));
    tb.spawn_detached(sleep_monitor(tb, dut));
}

int32_t done(coro::Testbench& tb, ApbEventDut dut) {
    const auto failures = tb.failures();
    if (failures == 0) {
        std::printf("cpptb: PASS apb_event_unit_peakrdl irq_o=0x%08x clk_gate_core_o=0x%08x\n",
                    dut.irq_o.get(), dut.clk_gate_core_o.get());
        return 0;
    }

    std::printf("cpptb: FAIL apb_event_unit_peakrdl failures=%u\n", failures);
    return 1;
}

}  // namespace cpptb::rggen_apb_event::tests
