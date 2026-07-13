#pragma once

#include <cstdint>
#include <utility>

#include "cpptb/coro_runtime.hpp"
#include "cpptb/test_result.hpp"
#include "examples/apb_regfile/generated/apb_regfile_dut.hpp"

namespace cpptb::examples::apb_regfile {

class ApbRegfileTb {
   public:
    ApbRegfileDut dut;

    ApbRegfileTb(coro::Testbench& scheduler, ApbRegfileDut dut_value,
                 TestResult& result, uint32_t iterations)
        : dut(dut_value),
          scheduler_(&scheduler),
          result_(&result),
          iterations_(iterations) {}

    void run(coro::Task<void> task) {
        scheduler_->spawn_detached(std::move(task));
    }

    void expect_eq(const char* label, uint32_t actual,
                   uint32_t expected) const;

    uint32_t iterations() const { return iterations_; }

   private:
    coro::Testbench* scheduler_ = nullptr;
    TestResult* result_ = nullptr;
    uint32_t iterations_ = 0;
};

void register_user_testbench(ApbRegfileTb& tb);

}  // namespace cpptb::examples::apb_regfile
