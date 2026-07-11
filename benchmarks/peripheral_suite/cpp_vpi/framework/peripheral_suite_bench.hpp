#pragma once

#include <cstdint>

#include "benchmarks/peripheral_suite/cpp_vpi/framework/peripheral_suite_dut.hpp"
#include "cpptb/coro_runtime.hpp"

namespace cpptb::benchmarks::peripheral_suite {

struct BenchConfig {
    uint32_t iterations = 1000;
};

struct BenchResult {
    uint64_t checks = 0;
    uint32_t failures = 0;
    uint32_t active_sequences = 0;
};

void register_benchmark(coro::Testbench& tb, PeripheralSuiteDut dut,
                        BenchConfig config, BenchResult& result);

int32_t done(BenchResult& result);

}  // namespace cpptb::benchmarks::peripheral_suite
