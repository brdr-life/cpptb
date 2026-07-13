#include <cstdint>

#include "cpptb/dpi_runtime.hpp"
#include "examples/fifo_scoreboard/framework.hpp"
#include "examples/fifo_scoreboard/generated/stream_fifo_binding.hpp"

namespace cpptb::examples::fifo_scoreboard {

struct DpiAdapter {
    using Dut = StreamFifoDut;
    using Result = TestResult;

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
    static constexpr const char* result_name =
        "CPP_DPI_FIFO_SCOREBOARD_RESULT";

    template <typename MakeSignal>
    static Dut bind_dut(MakeSignal make_signal) {
        return generated::bind_dut(make_signal);
    }

    static void register_testbench(coro::Testbench& scheduler, Dut dut,
                                   uint32_t iterations, Result& result) {
        FifoScoreboardTb tb{scheduler, dut, result, iterations};
        register_user_testbench(tb);
    }

    static bool timed_out(coro::SimTime, uint64_t sim_cycles,
                          uint32_t iterations) {
        return sim_cycles > static_cast<uint64_t>(iterations) * 12u + 100u;
    }
};

}  // namespace cpptb::examples::fifo_scoreboard

CPPTB_DEFINE_DPI_RUNTIME(cpptb::examples::fifo_scoreboard::DpiAdapter)
