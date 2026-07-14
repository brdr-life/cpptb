#include <cstdio>

#include "benchmarks/framework_comparison/open_cores/testbenches/cpp_dpi/framework/open_cores_benchmark.hpp"
#include "benchmarks/framework_comparison/open_cores/testbenches/cpp_dpi/generated/open_cores_binding.hpp"
#include "cpptb/clock_discovery.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s OUTPUT.json\n", argv[0]);
        return 2;
    }
    namespace bench = cpptb::benchmarks::open_cores;
    return cpptb::dpi::discover_clocks<bench::Dut, bench::kCpptbSignalCount,
                                       bench::BenchResult>(
        argv[1], 1'000ULL,
        [](auto make_signal) {
            return bench::generated::bind_dut_for_clock_discovery(make_signal);
        },
        [](cpptb::coro::Testbench& scheduler, bench::Dut dut,
           bench::BenchResult& result, cpptb::coro::ClockRegistrar clocks) {
            bench::register_benchmark(scheduler, dut, 1, result, clocks);
            return true;
        });
}
