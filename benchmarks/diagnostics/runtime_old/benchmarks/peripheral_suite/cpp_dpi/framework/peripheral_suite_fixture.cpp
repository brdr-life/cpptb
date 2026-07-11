#include "benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_fixture.hpp"

#include <cstdlib>
#include <cstdio>

namespace cpptb::benchmarks::peripheral_suite {
namespace {

using coro::Delay;
using coro::FallingEdge;
using coro::RisingEdge;
using namespace coro;

void check(BenchResult& result, const char* label, uint32_t actual,
           uint32_t expected) {
    ++result.checks;
    if (actual == expected) return;

    ++result.failures;
    if (result.failures <= 8) {
        std::printf("PERIPHERAL_SUITE_MISMATCH %s actual=0x%08x expected=0x%08x\n",
                    label, actual, expected);
    }
}

void check_apb(BenchResult& result, const char* label, uint32_t byte_addr,
               uint32_t actual, uint32_t expected) {
    ++result.checks;
    if (actual == expected) return;

    ++result.failures;
    if (result.failures <= 8) {
        std::printf(
            "PERIPHERAL_SUITE_MISMATCH %s addr=0x%03x actual=0x%08x expected=0x%08x\n",
            label, byte_addr, actual, expected);
    }
}

void launch(coro::Testbench& scheduler, coro::Task task) {
    static const bool use_detached_spawn =
        std::getenv("CPPTB_BENCH_DETACHED_SPAWN") != nullptr;
    if (use_detached_spawn) {
        scheduler.spawn_detached(std::move(task));
    } else {
        (void)scheduler.spawn(std::move(task));
    }
}

}  // namespace

ApbMaster::ApbMaster(ApbBus bus, coro::Signal clock, BenchResult& result)
    : bus_(bus), clock_(clock), result_(&result) {}

coro::Task ApbMaster::write(uint32_t byte_addr, uint32_t data) const {
    co_await FallingEdge{clock_};
    bus_.PADDR.set(byte_addr & 0xfff);
    bus_.PWDATA.set(data);
    bus_.PWRITE.set(1);
    bus_.PSEL.set(1);
    bus_.PENABLE.set(0);

    co_await FallingEdge{clock_};
    bus_.PENABLE.set(1);

    co_await RisingEdge{clock_};
    co_await Delay{1_ps};
    check_apb(*result_, "write PREADY", byte_addr, bus_.PREADY.get(), 1);
    check_apb(*result_, "write PSLVERR", byte_addr, bus_.PSLVERR.get(), 0);

    co_await FallingEdge{clock_};
    drive_apb_idle(bus_);
}

coro::Task ApbMaster::read_expect(uint32_t byte_addr,
                                  uint32_t expected) const {
    co_await FallingEdge{clock_};
    bus_.PADDR.set(byte_addr & 0xfff);
    bus_.PWDATA.set(0);
    bus_.PWRITE.set(0);
    bus_.PSEL.set(1);
    bus_.PENABLE.set(0);

    co_await FallingEdge{clock_};
    bus_.PENABLE.set(1);

    co_await RisingEdge{clock_};
    co_await Delay{1_ps};
    check_apb(*result_, "read PRDATA", byte_addr, bus_.PRDATA.get(), expected);
    check_apb(*result_, "read PREADY", byte_addr, bus_.PREADY.get(), 1);
    check_apb(*result_, "read PSLVERR", byte_addr, bus_.PSLVERR.get(), 0);

    co_await FallingEdge{clock_};
    drive_apb_idle(bus_);
}

coro::Task ApbMaster::read_ok(uint32_t byte_addr) const {
    co_await FallingEdge{clock_};
    bus_.PADDR.set(byte_addr & 0xfff);
    bus_.PWDATA.set(0);
    bus_.PWRITE.set(0);
    bus_.PSEL.set(1);
    bus_.PENABLE.set(0);

    co_await FallingEdge{clock_};
    bus_.PENABLE.set(1);

    co_await RisingEdge{clock_};
    co_await Delay{1_ps};
    check_apb(*result_, "read-ok PREADY", byte_addr, bus_.PREADY.get(), 1);
    check_apb(*result_, "read-ok PSLVERR", byte_addr, bus_.PSLVERR.get(), 0);

    co_await FallingEdge{clock_};
    drive_apb_idle(bus_);
}

PeripheralTb::PeripheralTb(coro::Testbench& scheduler, PeripheralSuiteDut dut_value,
                           BenchConfig config, BenchResult& result)
    : dut(dut_value), scheduler_(&scheduler), config_(config), result_(&result) {}

uint32_t PeripheralTb::iterations() const { return config_.iterations; }

bool PeripheralTb::sequences_running() const {
    return result_->active_sequences != 0;
}

ApbMaster PeripheralTb::timer_apb() const {
    return ApbMaster{dut.timer.apb, dut.HCLK, *result_};
}

ApbMaster PeripheralTb::spi_apb() const {
    return ApbMaster{dut.spi.apb, dut.HCLK, *result_};
}

ApbMaster PeripheralTb::i2c_apb() const {
    return ApbMaster{dut.i2c.apb, dut.HCLK, *result_};
}

void PeripheralTb::reset(Process process) {
    launch(*scheduler_, process(*this));
}

coro::Task run_sequence(PeripheralTb tb, PeripheralTb::Process process) {
    co_await process(tb);
    tb.end_sequence();
}

void PeripheralTb::sequence(Process process) {
    begin_sequence();
    launch(*scheduler_, run_sequence(*this, process));
}

void PeripheralTb::monitor(Process process) {
    launch(*scheduler_, process(*this));
}

void PeripheralTb::expect_eq(const char* label, uint32_t actual,
                             uint32_t expected) const {
    check(*result_, label, actual, expected);
}

void PeripheralTb::expect_apb(const char* label, uint32_t byte_addr,
                              uint32_t actual, uint32_t expected) const {
    check_apb(*result_, label, byte_addr, actual, expected);
}

coro::Task PeripheralTb::wait_cycles(uint32_t cycles) const {
    for (uint32_t i = 0; i < cycles; ++i) {
        co_await RisingEdge{dut.HCLK};
    }
}

coro::Task PeripheralTb::wait_reset_released() const {
    while (dut.HRESETn.get() == 0) {
        co_await RisingEdge{dut.HCLK};
    }
}

coro::Task PeripheralTb::wait_timer_irq(uint32_t bit,
                                        uint32_t max_cycles) const {
    bool seen = false;
    for (uint32_t i = 0; i < max_cycles; ++i) {
        co_await RisingEdge{dut.HCLK};
        co_await Delay{1_ps};
        if (((dut.timer.irq.get() >> bit) & 1u) != 0) {
            seen = true;
            break;
        }
    }
    expect_eq("timer irq wait", seen ? 1u : 0u, 1u);
}

void PeripheralTb::begin_sequence() const { ++result_->active_sequences; }

void PeripheralTb::end_sequence() const { --result_->active_sequences; }

}  // namespace cpptb::benchmarks::peripheral_suite
