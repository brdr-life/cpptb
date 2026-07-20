#include <cstdint>

#include "benchmarks/framework_comparison/heavy_suite/testbenches/cpp_dpi/framework/heavy_benchmark.hpp"
#include "benchmarks/framework_comparison/heavy_suite/testbenches/cpp_dpi/generated/heavy_benchmark_binding.hpp"
#include "cpptb/dpi_runtime.hpp"

namespace cpptb::benchmarks::heavy {

struct DpiAdapter {
    using Dut = heavy::Dut;
    using Result = BenchResult;

    static constexpr uint32_t signal_count = kCpptbSignalCount;
    static constexpr bool compact_input_transport =
        generated::kCompactInputTransport;
    inline static constexpr auto driven_signal_spans =
        generated::kDrivenSignalSpans;
    inline static constexpr auto observed_signal_word_ids =
        generated::kObservedSignalWordIds;
    inline static constexpr auto driven_signal_word_ids =
        generated::kDrivenSignalWordIds;
    inline static constexpr auto clock_signal_ids = generated::kClockSignalIds;
    inline static constexpr auto registered_clock_configs =
        generated::kRegisteredClockConfigs;
    inline static constexpr auto edge_observer_signal_ids =
        generated::kEdgeObserverSignalIds;
    static constexpr const char* result_name = "CPP_DPI_HEAVY_RUNTIME";

    template <typename MakeSignal>
    static Dut bind_dut(MakeSignal make_signal) {
        return generated::bind_dut(make_signal);
    }

    static void register_testbench(coro::Testbench& scheduler, Dut dut,
                                   uint32_t iterations, Result& result,
                                   coro::ClockRegistrar clocks) {
        register_benchmark(scheduler, dut, iterations, result, clocks);
    }

    static bool timed_out(coro::SimTime, uint64_t sim_cycles,
                          uint32_t iterations) {
        return sim_cycles > static_cast<uint64_t>(iterations) * 128u + 2'000u;
    }
};

}  // namespace cpptb::benchmarks::heavy

CPPTB_DEFINE_NAMED_DPI_RUNTIME_WITH_STARVATION(
                               cpptb::benchmarks::heavy::DpiAdapter,
                               heavy_dpi_init, heavy_dpi_step,
                               heavy_dpi_pull_outputs,
                               heavy_dpi_next_timer_deadline,
                               heavy_dpi_edge_interest,
                               heavy_dpi_report_starvation)
CPPTB_DEFINE_NAMED_DPI_CLOCK_API(heavy_dpi_clock_config)
