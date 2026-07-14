#pragma once

#include <chrono>
#include <cstdint>

#include "benchmarks/framework_comparison/open_cores/testbenches/cpp_dpi/generated/open_cores_dut.hpp"
#include "cpptb/coro_runtime.hpp"
#include "cpptb/test_result.hpp"

#define OPEN_CORE_PICORV32 0
#define OPEN_CORE_AES128 1
#define OPEN_CORE_FCS64 2

#ifndef OPEN_CORE_WORKLOAD
#define OPEN_CORE_WORKLOAD OPEN_CORE_PICORV32
#endif

namespace cpptb::benchmarks::open_cores {

struct BenchResult : cpptb::TestResult {
    uint64_t transactions = 0;
    uint32_t checksum = 0x811c'9dc5u;
    std::chrono::steady_clock::time_point start;
};

constexpr const char* workload_name() {
#if OPEN_CORE_WORKLOAD == OPEN_CORE_PICORV32
    return "picorv32_firmware";
#elif OPEN_CORE_WORKLOAD == OPEN_CORE_AES128
    return "secworks_aes128";
#elif OPEN_CORE_WORKLOAD == OPEN_CORE_FCS64
    return "ethernet_fcs64";
#else
#error "Unknown OPEN_CORE_WORKLOAD"
#endif
}

void register_benchmark(coro::Testbench& scheduler, Dut dut,
                        uint32_t iterations, BenchResult& result,
                        coro::ClockRegistrar clocks);

}  // namespace cpptb::benchmarks::open_cores
