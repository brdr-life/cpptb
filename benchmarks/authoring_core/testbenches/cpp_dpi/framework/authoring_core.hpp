#pragma once

#include <chrono>
#include <cstdint>

#include "benchmarks/authoring_core/testbenches/cpp_dpi/generated/authoring_core_dut.hpp"
#include "cpptb/coro_runtime.hpp"
#include "cpptb/fixed.hpp"
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
#define AUTHORING_CORE_KERNEL_WIDE64 9
#define AUTHORING_CORE_KERNEL_WIDE_ECHO_137 10
#define AUTHORING_CORE_KERNEL_WIDE_SLICE 11
#define AUTHORING_CORE_KERNEL_FIXED_MAC 12
#define AUTHORING_CORE_KERNEL_ARRAY_INDEX 13
#define AUTHORING_CORE_KERNEL_ARRAY_WIDE 14
#define AUTHORING_CORE_KERNEL_MEM_RW 15
#define AUTHORING_CORE_KERNEL_HIER_PROBE 16
#define AUTHORING_CORE_KERNEL_MEM_BACKDOOR 17
#define AUTHORING_CORE_KERNEL_MEM_PROBE_READ 18
#define AUTHORING_CORE_KERNEL_MEM_PROBE_DEPOSIT 19
#define AUTHORING_CORE_KERNEL_MEM_PROBE_READ_DEPOSIT 20
#define AUTHORING_CORE_KERNEL_SIGNAL_EDGE 21
#define AUTHORING_CORE_KERNEL_ARRAY_MULTIDIM 22
#define AUTHORING_CORE_KERNEL_FORCE_RELEASE 23
#define AUTHORING_CORE_KERNEL_PACKED_VIEW 24

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
    uint64_t wide64 = 0;
    uint64_t wide_echo_137 = 0;
    uint64_t wide_slice = 0;
    uint64_t fixed_mac = 0;
    uint64_t array_index = 0;
    uint64_t array_wide = 0;
    uint64_t array_multidim = 0;
    uint64_t mem_rw = 0;
    uint64_t hier_probe_reads = 0;
    uint64_t hier_probe_deposits = 0;
    uint64_t mem_backdoor_reads = 0;
    uint64_t mem_backdoor_deposits = 0;
    uint64_t probe_diag_reads = 0;
    uint64_t probe_diag_deposits = 0;
    uint64_t signal_edges = 0;
    uint64_t force_release = 0;
    uint64_t packed_view = 0;
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
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_WIDE64
    return "wide64";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_WIDE_ECHO_137
    return "wide_echo_137";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_WIDE_SLICE
    return "wide_slice";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_FIXED_MAC
    return "fixed_mac";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ARRAY_INDEX
    return "array_index";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ARRAY_WIDE
    return "array_wide";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEM_RW
    return "mem_rw";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_HIER_PROBE
    return "hier_probe";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEM_BACKDOOR
    return "mem_backdoor";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEM_PROBE_READ
    return "mem_probe_read";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEM_PROBE_DEPOSIT
    return "mem_probe_deposit";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEM_PROBE_READ_DEPOSIT
    return "mem_probe_read_deposit";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_SIGNAL_EDGE
    return "signal_edge";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ARRAY_MULTIDIM
    return "array_multidim";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_FORCE_RELEASE
    return "force_release";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_PACKED_VIEW
    return "packed_view";
#else
#error "Unknown AUTHORING_CORE_KERNEL"
#endif
}

void register_benchmark(coro::Testbench& scheduler, AuthoringCoreDut dut,
                        uint32_t iterations, BenchResult& result);

}  // namespace cpptb::benchmarks::authoring_core
