#include "benchmarks/peripheral_suite/testbenches/cpp_vpi/framework/peripheral_suite_bench.hpp"

#include <cstdio>

#include "benchmarks/peripheral_suite/testbenches/cpp_vpi/framework/peripheral_suite_fixture.hpp"

namespace cpptb::benchmarks::peripheral_suite {

void register_benchmark(coro::Testbench& scheduler, PeripheralSuiteDut dut,
                        BenchConfig config, BenchResult& result) {
    result = BenchResult{};

    PeripheralTb tb{scheduler, dut, config, result};
    register_user_testbench(tb);
}

int32_t done(BenchResult& result) {
    if (result.failures == 0) {
        return 0;
    }

    std::printf("PERIPHERAL_SUITE_FAIL failures=%u checks=%llu\n", result.failures,
                static_cast<unsigned long long>(result.checks));
    return 1;
}

}  // namespace cpptb::benchmarks::peripheral_suite
