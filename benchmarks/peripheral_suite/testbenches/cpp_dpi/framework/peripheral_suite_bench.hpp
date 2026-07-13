#pragma once

#include <cstdint>

#include "benchmarks/peripheral_suite/testbenches/cpp_dpi/generated/peripheral_suite_dut.hpp"
#include "cpptb/coro_runtime.hpp"
#include "cpptb/test_result.hpp"

namespace cpptb::benchmarks::peripheral_suite {

struct BenchConfig {
    uint32_t iterations = 1000;
};

struct BenchResult : cpptb::TestResult {
    uint32_t active_sequences = 0;
};

void register_benchmark(coro::Testbench& tb, PeripheralSuiteDut dut,
                        BenchConfig config, BenchResult& result);

}  // namespace cpptb::benchmarks::peripheral_suite
