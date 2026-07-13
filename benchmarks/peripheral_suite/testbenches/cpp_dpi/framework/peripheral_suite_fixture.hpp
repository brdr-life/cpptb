#pragma once

#include <cstdint>

#include "benchmarks/peripheral_suite/testbenches/cpp_dpi/framework/peripheral_suite_bench.hpp"
#include "benchmarks/peripheral_suite/testbenches/cpp_dpi/generated/peripheral_suite_dut.hpp"
#include "cpptb/coro_runtime.hpp"

namespace cpptb::benchmarks::peripheral_suite {

inline void drive_apb_idle(ApbBus bus) {
    bus.PADDR.set(0);
    bus.PWDATA.set(0);
    bus.PWRITE.set(0);
    bus.PSEL.set(0);
    bus.PENABLE.set(0);
}

class ApbMaster {
   public:
    ApbMaster(ApbBus bus, coro::Signal clock, BenchResult& result);

    coro::Task<void> write(uint32_t byte_addr, uint32_t data) const;
    coro::Task<void> read_expect(uint32_t byte_addr, uint32_t expected) const;
    coro::Task<void> read_ok(uint32_t byte_addr) const;

   private:
    ApbBus bus_;
    coro::Signal clock_;
    BenchResult* result_ = nullptr;
};

class PeripheralTb {
   public:
    using Process = coro::Task<void> (*)(PeripheralTb);

    PeripheralSuiteDut dut;

    PeripheralTb(coro::Testbench& scheduler, PeripheralSuiteDut dut,
                 BenchConfig config, BenchResult& result);

    uint32_t iterations() const;
    bool sequences_running() const;

    ApbMaster timer_apb() const;
    ApbMaster spi_apb() const;
    ApbMaster i2c_apb() const;

    void reset(Process process);
    void sequence(Process process);
    void monitor(Process process);

    void expect_eq(const char* label, uint32_t actual, uint32_t expected) const;
    void expect_apb(const char* label, uint32_t byte_addr, uint32_t actual,
                    uint32_t expected) const;

    coro::Task<void> wait_cycles(uint32_t cycles) const;
    coro::Task<void> wait_reset_released() const;
    coro::Task<void> wait_timer_irq(uint32_t bit, uint32_t max_cycles) const;

   private:
    coro::Testbench* scheduler_ = nullptr;
    BenchConfig config_;
    BenchResult* result_ = nullptr;

    void begin_sequence() const;
    void end_sequence() const;

    friend coro::Task<void> run_sequence(PeripheralTb tb, Process process);
};

void register_user_testbench(PeripheralTb& tb);

}  // namespace cpptb::benchmarks::peripheral_suite
