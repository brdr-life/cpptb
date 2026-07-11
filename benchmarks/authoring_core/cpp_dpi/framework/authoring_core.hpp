#pragma once

#include <chrono>
#include <cstdint>

#include "benchmarks/authoring_core/cpp_dpi/generated/authoring_core_dut.hpp"
#include "cpptb/coro_runtime.hpp"
#include "cpptb/test_result.hpp"

#define AUTHORING_CORE_KERNEL_CONTROL 0
#define AUTHORING_CORE_KERNEL_TASK_VALUE 1
#define AUTHORING_CORE_KERNEL_CLOCK_CYCLES 2
#define AUTHORING_CORE_KERNEL_TIMEOUT 3
#define AUTHORING_CORE_KERNEL_WAIT_UNTIL 4
#define AUTHORING_CORE_KERNEL_EVENT 5
#define AUTHORING_CORE_KERNEL_CHANNEL 6
#define AUTHORING_CORE_KERNEL_ALL 7
#define AUTHORING_CORE_KERNEL_TASK_TIMEOUT 8

#ifndef AUTHORING_CORE_KERNEL
#define AUTHORING_CORE_KERNEL AUTHORING_CORE_KERNEL_CONTROL
#endif

namespace cpptb::benchmarks::authoring_core {

struct FeatureCounts {
    uint64_t task_value = 0;
    uint64_t clock_cycles = 0;
    uint64_t timeouts = 0;
    uint64_t timeout_hits = 0;
    uint64_t task_timeouts = 0;
    uint64_t task_timeout_hits = 0;
    uint64_t wait_until = 0;
    uint64_t event_set = 0;
    uint64_t event_wait = 0;
    uint64_t channel_send = 0;
    uint64_t channel_receive = 0;
};

struct BenchResult : cpptb::TestResult {
    FeatureCounts features;
    uint64_t transactions = 0;
    uint32_t checksum = 0x811c9dc5u;
    std::chrono::steady_clock::time_point start;
};

constexpr const char* kernel_name() {
#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_CONTROL
    return "control";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_TASK_VALUE
    return "task_value";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_CLOCK_CYCLES
    return "clock_cycles";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_TIMEOUT
    return "timeout";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_TASK_TIMEOUT
    return "task_timeout";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_WAIT_UNTIL
    return "wait_until";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_EVENT
    return "event";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_CHANNEL
    return "channel";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
    return "all";
#else
#error "Unknown AUTHORING_CORE_KERNEL"
#endif
}

void register_benchmark(coro::Testbench& scheduler, AuthoringCoreDut dut,
                        uint32_t iterations, BenchResult& result);

}  // namespace cpptb::benchmarks::authoring_core
