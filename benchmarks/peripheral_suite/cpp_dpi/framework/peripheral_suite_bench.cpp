#include "benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_bench.hpp"

#include "benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_fixture.hpp"

namespace cpptb::benchmarks::peripheral_suite {

void register_benchmark(coro::Testbench& scheduler, PeripheralSuiteDut dut,
                        BenchConfig config, BenchResult& result) {
    result = BenchResult{};

    PeripheralTb tb{scheduler, dut, config, result};
    register_user_testbench(tb);
}

}  // namespace cpptb::benchmarks::peripheral_suite
