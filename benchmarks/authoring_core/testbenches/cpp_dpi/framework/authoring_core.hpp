#pragma once

#include <chrono>
#include <cstdint>

#include "benchmarks/authoring_core/testbenches/cpp_dpi/generated/authoring_core_dut.hpp"
#include "cpptb/coro_runtime.hpp"
#include "cpptb/coverage.hpp"
#include "cpptb/fixed.hpp"
#include "cpptb/test_api.hpp"
#include "cpptb/test_result.hpp"
#include "cpptb_vc/cpptb_vc.hpp"

#define AUTHORING_CORE_KERNEL_CONTROL 0
#define AUTHORING_CORE_KERNEL_TASK_VALUE 1
#define AUTHORING_CORE_KERNEL_CLOCK_CYCLES 2
#define AUTHORING_CORE_KERNEL_TIMEOUT 3
#define AUTHORING_CORE_KERNEL_WAIT_UNTIL 4
#define AUTHORING_CORE_KERNEL_EVENT 5
#define AUTHORING_CORE_KERNEL_QUEUE 6
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
#define AUTHORING_CORE_KERNEL_FORCE_DIRECT 25
#define AUTHORING_CORE_KERNEL_HIER_DATA 26
#define AUTHORING_CORE_KERNEL_TIMING_PHASES 27
#define AUTHORING_CORE_KERNEL_QUEUE_SYNC 28
#define AUTHORING_CORE_KERNEL_TEST_LIFECYCLE 29
#define AUTHORING_CORE_KERNEL_DYNAMIC_SPAWN 30
#define AUTHORING_CORE_KERNEL_DYNAMIC_TASK 31
#define AUTHORING_CORE_KERNEL_DYNAMIC_SPAWN_SCHEDULER 32
#define AUTHORING_CORE_KERNEL_DYNAMIC_SPAWN_SUSPENDING 33
#define AUTHORING_CORE_KERNEL_DYNAMIC_MONITOR 34
#define AUTHORING_CORE_KERNEL_ANALYSIS_FANOUT 35
#define AUTHORING_CORE_KERNEL_RANDOM_STIMULUS 36
#define AUTHORING_CORE_KERNEL_CONSTRAINED_PACKET 37
#define AUTHORING_CORE_KERNEL_CONSTRAINT_EXTENSIONS 38
#define AUTHORING_CORE_KERNEL_COVERAGE_SAMPLING 39
#define AUTHORING_CORE_KERNEL_APB_COMPONENT 40
#define AUTHORING_CORE_KERNEL_PROCESS_PIPELINE 41
#define AUTHORING_CORE_KERNEL_MEMORY_MODEL 42
#define AUTHORING_CORE_KERNEL_MEMORY_MODEL_DIRECT 43
#define AUTHORING_CORE_KERNEL_REGISTER_PREDICTION_VALIDITY 44
#define AUTHORING_CORE_KERNEL_REGISTER_BACKDOOR 45
#define AUTHORING_CORE_KERNEL_REGISTER_HIERARCHY 46
#define AUTHORING_CORE_KERNEL_REGISTER_SPLIT 47
#define AUTHORING_CORE_KERNEL_REGISTER_WIDE 48
#define AUTHORING_CORE_KERNEL_REGISTER_ENUM 49
#define AUTHORING_CORE_KERNEL_REGISTER_MEMORY 50
#define AUTHORING_CORE_KERNEL_REGISTER_SEQUENCES 51
#define AUTHORING_CORE_KERNEL_REGISTER_COVERAGE 52
#define AUTHORING_CORE_KERNEL_REGISTER_MAPS 53
#define AUTHORING_CORE_KERNEL_REGISTER_USER_EFFECTS 54
#define AUTHORING_CORE_KERNEL_STRUCTURED_LOGGING 55
#define AUTHORING_CORE_KERNEL_STRUCTURED_LOG_HISTORY 56
#define AUTHORING_CORE_KERNEL_MIXED_LOGGING 57
#define AUTHORING_CORE_KERNEL_TRANSACTION_RECORDING 58

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
    uint64_t queue_send = 0;
    uint64_t queue_receive = 0;
    uint64_t queue_put = 0;
    uint64_t queue_get = 0;
    uint64_t lock_acquire = 0;
    uint64_t semaphore_acquire = 0;
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
    uint64_t hier_data_reads = 0;
    uint64_t hier_data_deposits = 0;
    uint64_t timing_phases = 0;
    uint64_t test_lifecycle = 0;
    uint64_t dynamic_spawn = 0;
    uint64_t analysis_write = 0;
    uint64_t analysis_delivery = 0;
    uint64_t random_stimulus = 0;
    uint64_t constrained_packet = 0;
    uint64_t constraint_extensions = 0;
    uint64_t coverage_sampling = 0;
    uint64_t apb_component = 0;
    uint64_t memory_model = 0;
    uint64_t memory_model_direct = 0;
    uint64_t register_prediction_validity = 0;
    uint64_t register_backdoor = 0;
    uint64_t register_hierarchy = 0;
    uint64_t register_split = 0;
    uint64_t register_wide = 0;
    uint64_t register_enum = 0;
};

struct BenchResult : cpptb::TestResult {
    FeatureCounts features;
    uint64_t transactions = 0;
    uint64_t spawned_processes = 0;
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
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_QUEUE
    return "queue";
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
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_FORCE_DIRECT
    return "force_direct";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_HIER_DATA
    return "hier_data";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_TIMING_PHASES
#ifdef CPPTB_DEFERRED_WRITES
    // Same kernel, cocotb's write model: the deferred flavor is a separate
    // registry entry so its ratio is certified on its own.
    return "timing_phases_deferred";
#else
    return "timing_phases";
#endif
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_QUEUE_SYNC
    return "queue_sync";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_TEST_LIFECYCLE
    return "test_lifecycle";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_DYNAMIC_SPAWN
    return "dynamic_spawn";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_DYNAMIC_TASK
    return "dynamic_task";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_DYNAMIC_SPAWN_SCHEDULER
    return "dynamic_spawn_scheduler";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_DYNAMIC_SPAWN_SUSPENDING
    return "dynamic_spawn_suspending";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_DYNAMIC_MONITOR
    return "dynamic_monitor";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_PROCESS_PIPELINE
    return "process_pipeline";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ANALYSIS_FANOUT
    return "analysis_fanout";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_RANDOM_STIMULUS
    return "random_stimulus";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_CONSTRAINED_PACKET
    return "constrained_packet";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_CONSTRAINT_EXTENSIONS
    return "constraint_extensions";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_COVERAGE_SAMPLING
    return "coverage_sampling";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_APB_COMPONENT
    return "apb_component";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_TRANSACTION_RECORDING
    return "transaction_recording";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEMORY_MODEL
    return "memory_model";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEMORY_MODEL_DIRECT
    return "memory_model_direct";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_PREDICTION_VALIDITY
    return "register_prediction_validity";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_BACKDOOR
    return "register_backdoor";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_HIERARCHY
    return "register_hierarchy";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_SPLIT
    return "register_split";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_WIDE
    return "register_wide";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_ENUM
    return "register_enum";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_MEMORY
    return "register_memory";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_SEQUENCES
    return "register_sequences";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_COVERAGE
    return "register_coverage";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_MAPS
    return "register_maps";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_USER_EFFECTS
    return "register_user_effects";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_STRUCTURED_LOGGING
    return "structured_logging";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_STRUCTURED_LOG_HISTORY
    return "structured_log_history";
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MIXED_LOGGING
    return "mixed_logging";
#else
#error "Unknown AUTHORING_CORE_KERNEL"
#endif
}

void register_benchmark(coro::Testbench& scheduler, AuthoringCoreDut dut,
                        uint32_t iterations, BenchResult& result,
                        coro::ClockRegistrar clocks,
                        cpptb::detail::SimLogEndpoint& sim_logs);

}  // namespace cpptb::benchmarks::authoring_core
