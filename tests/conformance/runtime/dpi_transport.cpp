#include <cstdint>

#include "tests/conformance/runtime/framework.hpp"
#include "tests/conformance/runtime/generated/scheduler_conformance_binding.hpp"
#include "cpptb/dpi_runtime.hpp"

extern "C" void cpptb_dpi_phase_dispatch(unsigned int phase);

namespace cpptb::conformance {

struct DpiAdapter {
    using Dut = SchedulerConformanceDut;
    using Result = ConformanceResult;

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
    inline static constexpr auto edge_observer_signal_ids =
        generated::kEdgeObserverSignalIds;
    static constexpr const char* result_name = "CPPTB_CONFORMANCE_RESULT";

    template <typename MakeSignal>
    static Dut bind_dut(MakeSignal make_signal) {
        return generated::bind_dut(make_signal);
    }

    static void register_testbench(coro::Testbench& scheduler, Dut dut,
                                   uint32_t iterations, Result& result) {
        ConformanceTb tb{scheduler, dut, result};
        if (iterations == 2) {
            register_subprecision_delay_violation(tb);
        } else if (iterations == 3) {
            register_output_write_violation(tb);
        } else if (iterations == 4) {
            register_zero_delay_violation(tb);
        } else {
            register_user_testbench(tb);
        }
    }

    static void dispatch_phase(uint32_t phase) {
        cpptb_dpi_phase_dispatch(phase);
    }

    static bool timed_out(coro::SimTime sim_time, uint64_t sim_cycles,
                          uint32_t) {
        return sim_time > coro::SimTime{100'000'000u} || sim_cycles > 32u;
    }
};

}  // namespace cpptb::conformance

CPPTB_DEFINE_DPI_RUNTIME(cpptb::conformance::DpiAdapter)
