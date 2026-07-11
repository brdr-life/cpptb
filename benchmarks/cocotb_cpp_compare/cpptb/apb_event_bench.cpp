#include "benchmarks/cocotb_cpp_compare/cpptb/apb_event_bench.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace cpptb::benchmarks::apb_event {
namespace {

using coro::FallingEdge;
using coro::RisingEdge;
using coro::Task;
using rggen_apb_event::ApbEventDut;

constexpr uint32_t kIrqEnable = 0x000;
constexpr uint32_t kIrqPending = 0x004;
constexpr uint32_t kIrqSetPending = 0x008;
constexpr uint32_t kIrqClearPending = 0x00c;
constexpr uint32_t kEventEnable = 0x010;
constexpr uint32_t kEventPending = 0x014;
constexpr uint32_t kEventClearPending = 0x01c;
constexpr uint32_t kSleepCtrl = 0x020;
constexpr uint32_t kSleepStatus = 0x024;

void check(BenchResult& result, uint32_t actual, uint32_t expected) {
    ++result.checks;
    if (actual == expected) return;

    ++result.failures;
    if (result.failures <= 8) {
        std::printf("CPPTB_BENCH_MISMATCH actual=0x%08x expected=0x%08x\n",
                    actual, expected);
    }
}

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
    ApbMaster(ApbEventDut dut, BenchResult& result)
        : dut_(dut), result_(result) {}

    Task<void> write(uint32_t byte_addr, uint32_t data) const {
        co_await FallingEdge{dut_.HCLK};
        dut_.PADDR.set(byte_addr & 0xfff);
        dut_.PWDATA.set(data);
        dut_.PWRITE.set(1);
        dut_.PSEL.set(1);
        dut_.PENABLE.set(0);

        co_await FallingEdge{dut_.HCLK};
        dut_.PENABLE.set(1);

        co_await RisingEdge{dut_.HCLK};
        check(result_, dut_.PREADY.get(), 1);
        check(result_, dut_.PSLVERR.get(), 0);

        co_await FallingEdge{dut_.HCLK};
        drive_apb_idle(dut_);
    }

    Task<void> read_expect(uint32_t byte_addr, uint32_t expected) const {
        co_await FallingEdge{dut_.HCLK};
        dut_.PADDR.set(byte_addr & 0xfff);
        dut_.PWDATA.set(0);
        dut_.PWRITE.set(0);
        dut_.PSEL.set(1);
        dut_.PENABLE.set(0);

        co_await FallingEdge{dut_.HCLK};
        dut_.PENABLE.set(1);

        co_await RisingEdge{dut_.HCLK};
        check(result_, dut_.PRDATA.get(), expected);
        check(result_, dut_.PREADY.get(), 1);
        check(result_, dut_.PSLVERR.get(), 0);

        co_await FallingEdge{dut_.HCLK};
        drive_apb_idle(dut_);
    }

   private:
    ApbEventDut dut_;
    BenchResult& result_;
};

Task<void> reset_driver(ApbEventDut dut) {
    dut.HRESETn.set(0);
    dut.irq_i.set(0);
    dut.event_i.set(0);
    dut.fetch_enable_i.set(1);
    dut.core_busy_i.set(0);
    drive_apb_idle(dut);

    co_await wait_cycles(dut.HCLK, 5);
    dut.HRESETn.set(1);
    co_await wait_cycles(dut.HCLK, 4);
}

Task<void> traffic_sequence(ApbEventDut dut, BenchConfig config, BenchResult& result) {
    co_await wait_reset_released(dut);
    co_await wait_cycles(dut.HCLK, 2);

    const ApbMaster apb{dut, result};

    co_await apb.read_expect(kIrqEnable, 0);
    co_await apb.read_expect(kIrqPending, 0);
    co_await apb.read_expect(kEventPending, 0);

    for (uint32_t i = 0; i < config.iterations; ++i) {
        const uint32_t irq_bit = 1u << ((i % 30u) + 1u);
        const uint32_t sw_bit = 1u << ((i * 7u) % 31u);
        const uint32_t event_bit = 1u << (i % 16u);

        co_await apb.write(kIrqPending, 0);
        co_await apb.write(kIrqEnable, irq_bit | sw_bit);

        dut.irq_i.set(irq_bit);
        co_await wait_cycles(dut.HCLK, 2);
        co_await apb.read_expect(kIrqPending, irq_bit);

        dut.irq_i.set(0);
        co_await apb.write(kIrqSetPending, sw_bit);
        co_await wait_cycles(dut.HCLK, 1);
        co_await apb.read_expect(kIrqPending, irq_bit | sw_bit);

        co_await apb.write(kIrqClearPending, irq_bit | sw_bit);
        co_await wait_cycles(dut.HCLK, 1);
        co_await apb.read_expect(kIrqPending, 0);

        co_await apb.write(kEventEnable, event_bit);
        dut.event_i.set(event_bit);
        co_await wait_cycles(dut.HCLK, 2);
        co_await apb.read_expect(kEventPending, event_bit);

        dut.event_i.set(0);
        co_await apb.write(kEventClearPending, event_bit);
        co_await wait_cycles(dut.HCLK, 1);
        co_await apb.read_expect(kEventPending, 0);

        if ((i % 4u) == 0) {
            co_await apb.write(kSleepCtrl, 1);
            co_await wait_cycles(dut.HCLK, 4);
            co_await apb.read_expect(kSleepStatus, 1);
            check(result, dut.clk_gate_core_o.get(), 0);
            check(result, dut.fetch_enable_o.get(), 0);

            dut.event_i.set(event_bit);
            co_await wait_cycles(dut.HCLK, 3);
            co_await apb.read_expect(kSleepCtrl, 0);
            check(result, dut.clk_gate_core_o.get(), 1);

            dut.event_i.set(0);
            co_await apb.write(kEventClearPending, event_bit);
            co_await wait_cycles(dut.HCLK, 1);
            co_await apb.read_expect(kEventPending, 0);
        }
    }

    result.sequence_done = true;
}

Task<void> irq_monitor(ApbEventDut dut, BenchResult& result) {
    co_await wait_reset_released(dut);

    while (!result.sequence_done) {
        co_await RisingEdge{dut.HCLK};
        const auto irq = dut.irq_o.get();
        check(result, irq == 0 ? 0 : ((irq & (irq - 1)) == 0 ? 1 : 0), irq == 0 ? 0 : 1);
    }
}

Task<void> sleep_monitor(ApbEventDut dut, BenchResult& result) {
    co_await wait_reset_released(dut);

    while (!result.sequence_done) {
        co_await RisingEdge{dut.HCLK};
        if (dut.clk_gate_core_o.get() == 0) {
            check(result, dut.fetch_enable_o.get(), 0);
        }
    }
}

}  // namespace

void register_benchmark(coro::Testbench& tb, ApbEventDut dut, BenchConfig config,
                        BenchResult& result) {
    result = BenchResult{};
    tb.spawn_detached(reset_driver(dut));
    tb.spawn_detached(traffic_sequence(dut, config, result));
    tb.spawn_detached(irq_monitor(dut, result));
    tb.spawn_detached(sleep_monitor(dut, result));
}

int32_t done(BenchResult& result, ApbEventDut dut) {
    if (result.failures == 0) {
        return 0;
    }

    std::printf("CPPTB_BENCH_FAIL failures=%u irq_o=0x%08x clk_gate_core_o=0x%08x\n",
                result.failures, dut.irq_o.get(), dut.clk_gate_core_o.get());
    return 1;
}

}  // namespace cpptb::benchmarks::apb_event
