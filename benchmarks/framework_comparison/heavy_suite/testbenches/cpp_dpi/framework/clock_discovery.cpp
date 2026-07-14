#include <cstdio>

#include "benchmarks/framework_comparison/heavy_suite/testbenches/cpp_dpi/framework/heavy_benchmark.hpp"
#include "benchmarks/framework_comparison/heavy_suite/testbenches/cpp_dpi/generated/heavy_benchmark_binding.hpp"
#include "cpptb/clock_discovery.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s OUTPUT.json\n", argv[0]);
        return 2;
    }
    namespace heavy = cpptb::benchmarks::heavy;
    return cpptb::dpi::discover_clocks<heavy::Dut, heavy::kCpptbSignalCount,
                                       heavy::BenchResult>(
        argv[1], 1'000ULL,
        [](auto make_signal) {
            return heavy::generated::bind_dut_for_clock_discovery(make_signal);
        },
        [](cpptb::coro::Testbench& scheduler, heavy::Dut dut,
           heavy::BenchResult& result, cpptb::coro::ClockRegistrar clocks) {
            heavy::register_benchmark(scheduler, dut, 1, result, clocks);
            return true;
        });
}
