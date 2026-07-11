#include <cstdint>

#include "cpptb/conformance/framework.hpp"
#include "cpptb/conformance/generated/scheduler_conformance_binding.hpp"
#include "cpptb/dpi_runtime.hpp"

namespace cpptb::conformance {

struct DpiAdapter {
    using Dut = SchedulerConformanceDut;
    using Result = ConformanceResult;

    static constexpr uint32_t signal_count = kSignalCount;
    inline static constexpr auto driven_signal_ids =
        generated::kDrivenSignalIds;
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

    static bool timed_out(coro::SimTime sim_time, uint64_t sim_cycles,
                          uint32_t) {
        return sim_time > coro::SimTime{100'000'000u} || sim_cycles > 32u;
    }
};

}  // namespace cpptb::conformance

CPPTB_DEFINE_DPI_RUNTIME(cpptb::conformance::DpiAdapter)
