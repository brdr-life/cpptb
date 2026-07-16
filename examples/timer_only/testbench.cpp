#include <cstdint>

#include "cpptb/cpptb.hpp"
#include "dut.hpp"

namespace cpptb::examples::dpi_timer_only {
namespace {

using cpptb::Dut;
using coro::Delay;
using coro::Join;
using coro::Task;
using namespace coro;

constexpr uint32_t kCadenceSamples = 9;

Task<void> fast_cadence(Dut dut, TestContext& test) {
    for (uint32_t index = 0; index < kCadenceSamples; ++index) {
        co_await Delay{index == 0 ? 7_ns : 6'999_ps};
        const uint32_t value = 0x1000u + index * 17u;
        dut.fast_value.set(value);
        co_await Delay{1_ps};

        const uint64_t expected_time_ps =
            static_cast<uint64_t>(index + 1u) * 7'000u + 1u;
        test.expect_eq("fast cadence exact settle time",
                       test.now().in_picoseconds(), expected_time_ps);
        test.expect_eq("fast cadence settled echo", dut.fast_echo.get(),
                       value ^ 0x1357'9bdfu);
    }
}

Task<void> slow_cadence(Dut dut, TestContext& test) {
    for (uint32_t index = 0; index < kCadenceSamples; ++index) {
        co_await Delay{index == 0 ? 11_ns : 10'999_ps};
        const uint32_t value = 0x2000u + index * 29u;
        dut.slow_value.set(value);
        co_await Delay{1_ps};

        const uint64_t expected_time_ps =
            static_cast<uint64_t>(index + 1u) * 11'000u + 1u;
        test.expect_eq("slow cadence exact settle time",
                       test.now().in_picoseconds(), expected_time_ps);
        test.expect_eq("slow cadence settled echo", dut.slow_echo.get(),
                       value + 0x0102'0304u);
    }
}

Task<void> timer_only_test(Dut dut, TestContext& test) {
    co_await Join{fast_cadence(dut, test), slow_cadence(dut, test)};

    constexpr uint32_t last = kCadenceSamples - 1u;
    test.expect_eq("timer-only final absolute time",
                   test.now().in_picoseconds(),
                   static_cast<uint64_t>(kCadenceSamples) * 11'000u + 1u);
    test.expect_eq("timer-only final fast value", dut.fast_echo.get(),
                   (0x1000u + last * 17u) ^ 0x1357'9bdfu);
    test.expect_eq("timer-only final slow value", dut.slow_echo.get(),
                   (0x2000u + last * 29u) + 0x0102'0304u);
}

CPPTB_REGISTER_TEST(timer_only_test);

}  // namespace
}  // namespace cpptb::examples::dpi_timer_only
