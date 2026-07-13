#pragma once

#include <array>
#include <cstdint>

#include "tests/conformance/runtime/generated/scheduler_conformance_dut.hpp"
#include "cpptb/coro_runtime.hpp"
#include "cpptb/test_result.hpp"

namespace cpptb::conformance {

struct ConformanceResult : TestResult {
    std::array<uint32_t, 4> process_order{};
    uint32_t process_order_count = 0;
    uint32_t simultaneous_edges = 0;
    uint32_t simultaneous_pairs = 0;
    uint32_t lifecycle_markers = 0;
};

class ConformanceTb {
   public:
    using Process = coro::Task<void> (*)(ConformanceTb);

    SchedulerConformanceDut dut;

    ConformanceTb(coro::Testbench& scheduler, SchedulerConformanceDut dut,
                  ConformanceResult& result);

    void sequence(Process process) const;
    coro::Process spawn(coro::Task<void> task) const;
    void expect_eq(const char* label, uint64_t actual, uint64_t expected) const;
    void expect_time(const char* label, coro::SimTime actual,
                     coro::SimTime expected) const;
    void expect_true(const char* label, bool condition) const;
    void record_process(uint32_t marker) const;
    void mark_simultaneous_edge() const;
    void mark_simultaneous_pair() const;
    void mark_lifecycle(uint32_t marker) const;

    uint32_t process_order_count() const;
    uint32_t process_order(uint32_t index) const;
    uint32_t simultaneous_edges() const;
    uint32_t simultaneous_pairs() const;
    uint32_t lifecycle_markers() const;
    bool has_falling_edge_waiters() const;
    coro::SimTime now() const;

   private:
    coro::Testbench* scheduler_ = nullptr;
    ConformanceResult* result_ = nullptr;
};

void register_user_testbench(ConformanceTb& tb);
void register_subprecision_delay_violation(ConformanceTb& tb);
void register_output_write_violation(ConformanceTb& tb);
void register_zero_delay_violation(ConformanceTb& tb);

}  // namespace cpptb::conformance
