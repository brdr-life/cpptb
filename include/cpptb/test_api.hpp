#pragma once

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "cpptb/coro_runtime.hpp"
#include "cpptb/test_result.hpp"

namespace cpptb {

namespace detail {

template <typename Value>
void print_diagnostic_integral(FILE* stream, Value value) {
    const auto normalized = [&] {
        if constexpr (std::is_enum_v<Value>) {
            return static_cast<std::underlying_type_t<Value>>(value);
        } else {
            return value;
        }
    }();
    if constexpr (std::is_signed_v<decltype(normalized)>) {
        std::fprintf(stream, "%lld", static_cast<long long>(normalized));
    } else {
        std::fprintf(stream, "%llu",
                     static_cast<unsigned long long>(normalized));
    }
}

}  // namespace detail

class TestContext {
   public:
    TestContext(coro::Testbench& scheduler, TestResult& result,
                coro::ClockRegistrar clocks = {})
        : scheduler_(&scheduler), result_(&result), clocks_(clocks) {}

    template <typename Actual, typename Expected>
        requires requires(const Actual& actual, const Expected& expected) {
            { actual == expected } -> std::convertible_to<bool>;
        }
    void expect_eq(std::string_view label, const Actual& actual,
                   const Expected& expected) const {
        ++result_->checks;
        if (actual == expected) return;

        ++result_->failures;
        if constexpr ((std::is_integral_v<Actual> || std::is_enum_v<Actual>) &&
                      (std::is_integral_v<Expected> ||
                       std::is_enum_v<Expected>)) {
            std::fprintf(stderr, "cpptb: %.*s: actual=",
                         static_cast<int>(label.size()), label.data());
            detail::print_diagnostic_integral(stderr, actual);
            std::fputs(" expected=", stderr);
            detail::print_diagnostic_integral(stderr, expected);
            std::fputc('\n', stderr);
        } else {
            std::fprintf(stderr, "cpptb: check failed: %.*s\n",
                         static_cast<int>(label.size()), label.data());
        }
    }

    void expect(std::string_view label, bool condition) const {
        ++result_->checks;
        if (condition) return;
        ++result_->failures;
        std::fprintf(stderr, "cpptb: check failed: %.*s\n",
                     static_cast<int>(label.size()), label.data());
    }

    coro::Process spawn(coro::Task<void> task) const {
        return scheduler_->spawn(std::move(task));
    }

    void spawn_detached(coro::Task<void> task) const {
        scheduler_->spawn_detached(std::move(task));
    }

    template <typename Signal>
        requires std::convertible_to<Signal, coro::Signal>
    void start_clock(Signal signal, coro::SimTime period,
                     coro::SimTime phase = {}) const {
        clocks_.start(static_cast<coro::Signal>(signal), period, phase);
    }

    coro::SimTime now() const { return scheduler_->now(); }

   private:
    coro::Testbench* scheduler_;
    TestResult* result_;
    coro::ClockRegistrar clocks_;
};

namespace detail {

template <typename Dut>
using TestFunction = coro::Task<void> (*)(Dut, TestContext&);

template <typename Dut>
struct TestRegistrationEntry {
    const char* name;
    TestFunction<Dut> function;
};

template <typename Dut>
std::vector<TestRegistrationEntry<Dut>>& test_registry() {
    static std::vector<TestRegistrationEntry<Dut>> entries;
    return entries;
}

template <typename Dut>
class TestRegistration {
   public:
    TestRegistration(const char* name, TestFunction<Dut> function) {
        test_registry<Dut>().push_back({name, function});
    }
};

template <typename Dut>
coro::Task<void> invoke_registered_test(TestFunction<Dut> function, Dut dut,
                                        coro::Testbench& scheduler,
                                        TestResult& result,
                                        coro::ClockRegistrar clocks) {
    TestContext context{scheduler, result, clocks};
    co_await function(dut, context);
}

}  // namespace detail

template <typename Dut>
detail::TestRegistration<Dut> register_test(
    const char* name, detail::TestFunction<Dut> function) {
    return detail::TestRegistration<Dut>{name, function};
}

template <typename Dut>
bool run_registered_test(coro::Testbench& scheduler, Dut dut,
                         TestResult& result,
                         coro::ClockRegistrar clocks = {}) {
    const auto& entries = detail::test_registry<Dut>();
    if (entries.size() != 1) {
        ++result.failures;
        if (entries.empty()) {
            std::fprintf(stderr,
                         "cpptb: no test is registered for the selected DUT\n");
        } else {
            std::fprintf(stderr,
                         "cpptb: %zu tests are registered for the selected DUT; "
                         "one test per simulator invocation is supported:",
                         entries.size());
            for (const auto& entry : entries) {
                std::fprintf(stderr, " %s", entry.name);
            }
            std::fputc('\n', stderr);
        }
        return false;
    }

    scheduler.spawn_detached(detail::invoke_registered_test(
        entries.front().function, dut, scheduler, result, clocks));
    return true;
}

}  // namespace cpptb

#define CPPTB_DETAIL_JOIN_IMPL(Left, Right) Left##Right
#define CPPTB_DETAIL_JOIN(Left, Right) CPPTB_DETAIL_JOIN_IMPL(Left, Right)
#define CPPTB_REGISTER_TEST(TestFunction)                                  \
    [[maybe_unused]] const auto CPPTB_DETAIL_JOIN(                         \
        cpptb_test_registration_, __COUNTER__) =                           \
        ::cpptb::register_test(#TestFunction, TestFunction)
