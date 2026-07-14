#pragma once

#include <chrono>
#include <cstdint>

#include "benchmarks/framework_comparison/heavy_suite/testbenches/cpp_dpi/generated/heavy_benchmark_dut.hpp"
#include "cpptb/coro_runtime.hpp"
#include "cpptb/test_result.hpp"

#define HEAVY_WORKLOAD_FIR 0
#define HEAVY_WORKLOAD_CRC32 1
#define HEAVY_WORKLOAD_MATRIX 2

#ifndef HEAVY_WORKLOAD
#define HEAVY_WORKLOAD HEAVY_WORKLOAD_FIR
#endif

namespace cpptb::benchmarks::heavy {

struct BenchResult : cpptb::TestResult {
    uint64_t transactions = 0;
    uint32_t checksum = 0x811c'9dc5u;
    std::chrono::steady_clock::time_point start;
};

constexpr const char* workload_name() {
#if HEAVY_WORKLOAD == HEAVY_WORKLOAD_FIR
    return "streaming_fir";
#elif HEAVY_WORKLOAD == HEAVY_WORKLOAD_CRC32
    return "packet_crc32";
#elif HEAVY_WORKLOAD == HEAVY_WORKLOAD_MATRIX
    return "matrix4x4";
#else
#error "Unknown HEAVY_WORKLOAD"
#endif
}

void register_benchmark(coro::Testbench& scheduler, Dut dut,
                        uint32_t iterations, BenchResult& result,
                        coro::ClockRegistrar clocks);

}  // namespace cpptb::benchmarks::heavy
