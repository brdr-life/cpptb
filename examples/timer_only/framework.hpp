#pragma once

#include <cstdint>

#include "cpptb/coro_runtime.hpp"
#include "examples/timer_only/generated/timer_only_probe_dut.hpp"
#include "cpptb/test_result.hpp"

namespace cpptb::examples::dpi_timer_only {

using TestResult = cpptb::TestResult;

class TimerOnlyTb {
   public:
    using Process = coro::Task<void> (*)(TimerOnlyTb);

    TimerOnlyProbeDut dut;

    TimerOnlyTb(coro::Testbench& scheduler, TimerOnlyProbeDut dut_value,
                TestResult& result, uint32_t iterations)
        : dut(dut_value),
          scheduler_(&scheduler),
          result_(&result),
          iterations_(iterations) {}

    void sequence(Process process) {
        scheduler_->spawn_detached(process(*this));
    }

    void expect_eq(const char* label, uint64_t actual, uint64_t expected) const;

    coro::SimTime now() const { return scheduler_->now(); }

    uint32_t iterations() const { return iterations_; }

   private:
    coro::Testbench* scheduler_ = nullptr;
    TestResult* result_ = nullptr;
    uint32_t iterations_ = 0;
};

void register_user_testbench(TimerOnlyTb& tb);

}  // namespace cpptb::examples::dpi_timer_only
