#include <cstdint>

#include "benchmarks/authoring_core/testbenches/cpp_dpi/framework/authoring_core.hpp"
#include "benchmarks/authoring_core/testbenches/cpp_dpi/generated/authoring_core_binding.hpp"
#include "cpptb/dpi_runtime.hpp"

namespace cpptb::benchmarks::authoring_core {

struct DpiAdapter {
    using Dut = AuthoringCoreDut;
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
    inline static constexpr auto clock_signal_ids =
        generated::kClockSignalIds;
    inline static constexpr auto registered_clock_configs =
        generated::kRegisteredClockConfigs;
    inline static constexpr auto edge_observer_signal_ids =
        generated::kEdgeObserverSignalIds;
    static constexpr const char* result_name = "CPP_DPI_AUTHORING_CORE_RUNTIME";

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
        return sim_cycles > static_cast<uint64_t>(iterations) * 40u + 1'000u;
    }
};

}  // namespace cpptb::benchmarks::authoring_core

CPPTB_DEFINE_NAMED_DPI_RUNTIME(
    cpptb::benchmarks::authoring_core::DpiAdapter,
    authoring_core_dpi_init,
    authoring_core_dpi_step,
    authoring_core_dpi_pull_outputs,
    authoring_core_dpi_next_timer_deadline,
    authoring_core_dpi_edge_interest)
CPPTB_DEFINE_NAMED_DPI_CLOCK_API(authoring_core_dpi_clock_config)
