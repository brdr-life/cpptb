#pragma once

#include <cstdint>
#include <utility>

#include "cpptb/coro_runtime.hpp"
#include "cpptb/test_result.hpp"
#include "examples/counter/generated/counter_dut.hpp"

namespace cpptb::examples::counter {

class CounterTb {
   public:
    CounterDut dut;

    CounterTb(coro::Testbench& scheduler, CounterDut dut_value,
              TestResult& result, uint32_t iterations)
        : dut(dut_value),
          scheduler_(&scheduler),
          result_(&result),
          iterations_(iterations) {}

    void expect_eq(const char* label, uint32_t actual,
                   uint32_t expected) const;

    uint32_t iterations() const { return iterations_; }

    void run(coro::Task<void> task) {
        scheduler_->spawn_detached(std::move(task));
    }

   private:
    coro::Testbench* scheduler_ = nullptr;
    TestResult* result_ = nullptr;
    uint32_t iterations_ = 0;
};

void register_user_testbench(CounterTb& tb);

}  // namespace cpptb::examples::counter
