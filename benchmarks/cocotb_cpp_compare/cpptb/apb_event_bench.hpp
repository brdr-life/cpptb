#pragma once

#include <cstdint>

#include "cpptb/coro_runtime.hpp"
#include "cpptb/rggen_apb_event/apb_event_dut.hpp"

namespace cpptb::benchmarks::apb_event {

struct BenchConfig {
    uint32_t iterations = 1000;
};

struct BenchResult {
    uint64_t checks = 0;
    uint32_t failures = 0;
    bool sequence_done = false;
};

void register_benchmark(coro::Testbench& tb, rggen_apb_event::ApbEventDut dut,
                        BenchConfig config, BenchResult& result);

int32_t done(BenchResult& result, rggen_apb_event::ApbEventDut dut);

}  // namespace cpptb::benchmarks::apb_event
