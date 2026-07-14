#include <cstdio>

#include "benchmarks/authoring_core/testbenches/cpp_dpi/framework/authoring_core.hpp"
#include "benchmarks/authoring_core/testbenches/cpp_dpi/generated/authoring_core_binding.hpp"
#include "cpptb/clock_discovery.hpp"
#include "cpptb/hierarchy.hpp"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s CLOCKS.json ACCESS.json\n", argv[0]);
        return 2;
    }
    namespace authoring = cpptb::benchmarks::authoring_core;
    const int result = cpptb::dpi::discover_clocks<
        authoring::AuthoringCoreDut, authoring::kCpptbSignalCount,
        authoring::BenchResult>(
        argv[1], 1'000ULL,
        [](auto make_signal) {
            return authoring::generated::bind_dut_for_clock_discovery(
                make_signal);
        },
        [](cpptb::coro::Testbench& scheduler,
           authoring::AuthoringCoreDut dut, authoring::BenchResult& result,
           cpptb::coro::ClockRegistrar clocks) {
            authoring::register_benchmark(scheduler, dut, 1, result, clocks);
            return true;
        });
    if (result != 0) return result;
    return cpptb::hierarchy::write_discovered_access_plan(argv[2]) ? 0 : 1;
}
