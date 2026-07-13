#include "cpptb/examples/dpi_timer_only/framework.hpp"

namespace cpptb::examples::dpi_timer_only {
namespace {

using coro::Delay;
using coro::Join;
using coro::Task;
using namespace coro;

Task<void> fast_cadence(TimerOnlyTb tb) {
    for (uint32_t index = 0; index < tb.iterations(); ++index) {
        co_await Delay{index == 0 ? 7_ns : 6'999_ps};
        const uint32_t value = 0x1000u + index * 17u;
        tb.dut.fast.value.set(value);
        co_await Delay{1_ps};

        const uint64_t expected_time_ps =
            static_cast<uint64_t>(index + 1u) * 7'000u + 1u;
        tb.expect_eq("fast cadence exact settle time",
                     tb.now().in_picoseconds(), expected_time_ps);
        tb.expect_eq("fast cadence settled echo", tb.dut.fast.echo.get(),
                     value ^ 0x1357'9bdfu);
    }
}

Task<void> slow_cadence(TimerOnlyTb tb) {
    for (uint32_t index = 0; index < tb.iterations(); ++index) {
        co_await Delay{index == 0 ? 11_ns : 10'999_ps};
        const uint32_t value = 0x2000u + index * 29u;
        tb.dut.slow.value.set(value);
        co_await Delay{1_ps};

        const uint64_t expected_time_ps =
            static_cast<uint64_t>(index + 1u) * 11'000u + 1u;
        tb.expect_eq("slow cadence exact settle time",
                     tb.now().in_picoseconds(), expected_time_ps);
        tb.expect_eq("slow cadence settled echo", tb.dut.slow.echo.get(),
                     value + 0x0102'0304u);
    }
}

Task<void> timer_only_contract(TimerOnlyTb tb) {
    co_await Join{fast_cadence(tb), slow_cadence(tb)};

    const uint32_t last = tb.iterations() - 1u;
    tb.expect_eq("timer-only final absolute time", tb.now().in_picoseconds(),
                 static_cast<uint64_t>(tb.iterations()) * 11'000u + 1u);
    tb.expect_eq("timer-only final fast value", tb.dut.fast.echo.get(),
                 (0x1000u + last * 17u) ^ 0x1357'9bdfu);
    tb.expect_eq("timer-only final slow value", tb.dut.slow.echo.get(),
                 (0x2000u + last * 29u) + 0x0102'0304u);
}

}  // namespace

void register_user_testbench(TimerOnlyTb& tb) {
    tb.sequence(timer_only_contract);
}

}  // namespace cpptb::examples::dpi_timer_only
