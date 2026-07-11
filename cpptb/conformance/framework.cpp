#include "cpptb/conformance/framework.hpp"

#include <cstdio>

namespace cpptb::conformance {

ConformanceTb::ConformanceTb(coro::Testbench& scheduler,
                             SchedulerConformanceDut dut_value,
                             ConformanceResult& result)
    : dut(dut_value), scheduler_(&scheduler), result_(&result) {}

void ConformanceTb::sequence(Process process) const {
    scheduler_->spawn_detached(process(*this));
}

coro::Process ConformanceTb::spawn(coro::Task<void> task) const {
    return scheduler_->spawn(std::move(task));
}

void ConformanceTb::expect_eq(const char* label, uint64_t actual,
                              uint64_t expected) const {
    ++result_->checks;
    if (actual == expected) return;

    ++result_->failures;
    std::printf(
        "CPPTB_CONFORMANCE_MISMATCH %s actual=0x%016llx expected=0x%016llx\n",
        label, static_cast<unsigned long long>(actual),
        static_cast<unsigned long long>(expected));
}

void ConformanceTb::expect_time(const char* label, coro::SimTime actual,
                                coro::SimTime expected) const {
    expect_eq(label, actual.in_femtoseconds(), expected.in_femtoseconds());
}

void ConformanceTb::expect_true(const char* label, bool condition) const {
    expect_eq(label, condition ? 1u : 0u, 1u);
}

void ConformanceTb::record_process(uint32_t marker) const {
    if (result_->process_order_count >= result_->process_order.size()) {
        expect_true("process order trace capacity", false);
        return;
    }
    result_->process_order[result_->process_order_count++] = marker;
}

void ConformanceTb::mark_simultaneous_edge() const {
    ++result_->simultaneous_edges;
}

void ConformanceTb::mark_simultaneous_pair() const {
    ++result_->simultaneous_pairs;
}

void ConformanceTb::mark_lifecycle(uint32_t marker) const {
    result_->lifecycle_markers |= marker;
}

uint32_t ConformanceTb::process_order_count() const {
    return result_->process_order_count;
}

uint32_t ConformanceTb::process_order(uint32_t index) const {
    if (index >= result_->process_order_count) return 0;
    return result_->process_order[index];
}

uint32_t ConformanceTb::simultaneous_edges() const {
    return result_->simultaneous_edges;
}

uint32_t ConformanceTb::simultaneous_pairs() const {
    return result_->simultaneous_pairs;
}

uint32_t ConformanceTb::lifecycle_markers() const {
    return result_->lifecycle_markers;
}

bool ConformanceTb::has_falling_edge_waiters() const {
    return scheduler_->has_falling_edge_waiters();
}

coro::SimTime ConformanceTb::now() const { return scheduler_->now(); }

}  // namespace cpptb::conformance
