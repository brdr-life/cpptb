#include <cstdint>

#include "benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_bench.hpp"
#include "benchmarks/peripheral_suite/cpp_dpi/generated/peripheral_suite_binding.hpp"
#include "cpptb/dpi_runtime.hpp"

namespace cpptb::benchmarks::peripheral_suite {

struct DpiAdapter {
    using Dut = PeripheralSuiteDut;
    using Result = BenchResult;

    static constexpr uint32_t signal_count = kSignalCount;
    static constexpr bool compact_input_transport =
        generated::kCompactInputTransport;
    inline static constexpr auto driven_signal_spans =
        generated::kDrivenSignalSpans;
    inline static constexpr auto observed_signal_word_ids =
        generated::kObservedSignalWordIds;
    inline static constexpr auto driven_signal_word_ids =
        generated::kDrivenSignalWordIds;
    inline static constexpr auto clock_signal_ids =
        generated::kClockSignalIds;
    inline static constexpr auto edge_observer_signal_ids =
        generated::kEdgeObserverSignalIds;
    static constexpr const char* result_name = "CPP_DPI_PERIPHERAL_RESULT";

    template <typename MakeSignal>
    static Dut bind_dut(MakeSignal make_signal) {
        return generated::bind_dut(make_signal);
    }

    static void register_testbench(coro::Testbench& scheduler, Dut dut,
                                   uint32_t iterations, Result& result) {
        register_benchmark(scheduler, dut, BenchConfig{iterations}, result);
    }

    static bool timed_out(coro::SimTime, uint64_t sim_cycles,
                          uint32_t iterations) {
        return sim_cycles >
               static_cast<uint64_t>(iterations) * 160u + 1'000u;
    }
};

}  // namespace cpptb::benchmarks::peripheral_suite

CPPTB_DEFINE_DPI_RUNTIME(cpptb::benchmarks::peripheral_suite::DpiAdapter)
