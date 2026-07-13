#pragma once

#include <cstdint>
#include <utility>

#include "cpptb/coro_runtime.hpp"
#include "cpptb/test_result.hpp"
#include "examples/watchdog_timeout/generated/stalling_responder_dut.hpp"

namespace cpptb::examples::watchdog_timeout {

class WatchdogTimeoutTb {
   public:
    StallingResponderDut dut;

    WatchdogTimeoutTb(coro::Testbench& scheduler,
                      StallingResponderDut dut_value, TestResult& result,
                      uint32_t iterations)
        : dut(dut_value),
          scheduler_(&scheduler),
          result_(&result),
          iterations_(iterations) {}

    void run(coro::Task<void> task) {
        scheduler_->spawn_detached(std::move(task));
    }

    coro::Process spawn(coro::Task<void> task) {
        return scheduler_->spawn(std::move(task));
    }

    void expect_eq(const char* label, uint32_t actual,
                   uint32_t expected) const;

    uint32_t iterations() const { return iterations_; }

   private:
    coro::Testbench* scheduler_ = nullptr;
    TestResult* result_ = nullptr;
    uint32_t iterations_ = 0;
};

void register_user_testbench(WatchdogTimeoutTb& tb);

}  // namespace cpptb::examples::watchdog_timeout
