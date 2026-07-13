#include <cstdint>

#include "cpptb/dpi_runtime.hpp"
#include "cpptb/examples/dpi_timer_only/framework.hpp"
#include "cpptb/examples/dpi_timer_only/generated/timer_only_probe_binding.hpp"

namespace cpptb::examples::dpi_timer_only {

struct DpiAdapter {
    using Dut = TimerOnlyProbeDut;
    using Result = TestResult;

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
    static constexpr const char* result_name = "CPP_DPI_TIMER_ONLY_RESULT";

    template <typename MakeSignal>
    static Dut bind_dut(MakeSignal make_signal) {
        return generated::bind_dut(make_signal);
    }

    static void register_testbench(coro::Testbench& scheduler, Dut dut,
                                   uint32_t iterations, Result& result) {
        TimerOnlyTb tb{scheduler, dut, result, iterations};
        register_user_testbench(tb);
    }

    static bool timed_out(coro::SimTime sim_time, uint64_t,
                          uint32_t iterations) {
        const uint64_t timeout_ns =
            100u + static_cast<uint64_t>(iterations) * 20u;
        return sim_time > coro::SimTime{timeout_ns * 1'000'000u};
    }
};

}  // namespace cpptb::examples::dpi_timer_only

CPPTB_DEFINE_DPI_RUNTIME(cpptb::examples::dpi_timer_only::DpiAdapter)
