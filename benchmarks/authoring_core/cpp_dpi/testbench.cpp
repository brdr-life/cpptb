#include "benchmarks/authoring_core/cpp_dpi/framework/authoring_core.hpp"

#include <chrono>
#include <cstdio>

namespace cpptb::benchmarks::authoring_core {
namespace {

using coro::Channel;
using coro::Delay;
using coro::Event;
using coro::FallingEdge;
using coro::RisingEdge;
using coro::Task;
using coro::TimeoutOutcome;
using namespace coro;

struct Context {
    coro::Testbench& scheduler;
    AuthoringCoreDut dut;
    uint32_t iterations;
    BenchResult& result;
};

uint32_t stimulus(uint32_t iteration) {
    return ((iteration + 1u) * 0x1f12'3bb5u) ^ 0xc001'd00du;
}

uint32_t expected_response(uint32_t iteration) {
    return (stimulus(iteration) ^ 0xa5a5'5a5au) + iteration;
}

void check(Context& context, const char* label, uint32_t actual,
           uint32_t expected) {
    ++context.result.checks;
    if (actual == expected) return;
    ++context.result.failures;
    if (context.result.failures <= 8) {
        std::printf("AUTHORING_CORE_MISMATCH mode=cpp_dpi kernel=%s "
                    "label=%s actual=0x%08x expected=0x%08x\n",
                    kernel_name(), label, actual, expected);
    }
}

Task<uint32_t> authored_value(uint32_t iteration) {
    co_return stimulus(iteration);
}

Task<uint32_t> delayed_authored_value(uint32_t iteration, SimTime delay) {
    co_await Delay{delay};
    co_return stimulus(iteration);
}

Task<void> wait_ready_raw(Context& context) {
    while (context.dut.req_ready.get() == 0) {
        co_await RisingEdge{context.dut.clk};
    }
}

Task<void> transact(Context& context, uint32_t iteration, uint32_t payload,
                    bool ready_already = false) {
    if (!ready_already) co_await wait_ready_raw(context);

    co_await FallingEdge{context.dut.clk};
    context.dut.req_data.set(payload);
    context.dut.req_valid.set(1);

    co_await RisingEdge{context.dut.clk};
    co_await FallingEdge{context.dut.clk};
    context.dut.req_valid.set(0);

    while (true) {
        co_await RisingEdge{context.dut.clk};
        co_await Delay{1_ps};
        if (context.dut.rsp_valid.get() != 0) break;
    }

    const uint32_t response = context.dut.rsp_data.get();
    check(context, "response", response, expected_response(iteration));
    context.result.checksum =
        (context.result.checksum ^ response) * 0x0100'0193u;
    ++context.result.transactions;
}

Task<bool> timeout_probe(Context& context, uint32_t iteration) {
    ++context.result.features.timeouts;
    const bool expect_timeout = (iteration & 1u) != 0;
    const auto outcome = co_await with_timeout(
        RisingEdge{context.dut.clk}, expect_timeout ? 500_ps : 3_ns);
    const bool timed_out = outcome == TimeoutOutcome::TimedOut;
    if (timed_out) ++context.result.features.timeout_hits;
    check(context, "timeout outcome", timed_out, expect_timeout);
    co_return timed_out;
}

Task<uint32_t> task_timeout_probe(Context& context, uint32_t iteration,
                                  uint32_t fallback) {
    ++context.result.features.task_timeouts;
    const bool expect_timeout = (iteration & 1u) != 0;
    const auto outcome = co_await with_timeout(
        delayed_authored_value(iteration, expect_timeout ? 3_ns : 500_ps),
        expect_timeout ? 500_ps : 3_ns);
    const bool valid_outcome =
        expect_timeout ? outcome.timed_out() : outcome.triggered();
    if (outcome.timed_out()) {
        ++context.result.features.task_timeout_hits;
    }
    check(context, "task timeout outcome", valid_outcome, 1);
    co_return outcome.triggered() ? outcome.value() : fallback;
}

Task<void> event_roundtrip(Context& context, Event& event) {
    ++context.result.features.event_set;
    event.set();
    ++context.result.features.event_wait;
    co_await event;
    check(context, "event sticky state", event.is_set(), 1);
    event.clear();
}

Task<void> wait_for_response_count(Context& context) {
    while (context.dut.response_count.get() != context.iterations) {
        co_await RisingEdge{context.dut.clk};
        co_await Delay{1_ps};
    }
}

uint64_t reported_sim_cycles(Context& context) {
    constexpr uint64_t one_ns_fs = 1'000'000u;
    constexpr uint64_t clock_period_fs = 2'000'000u;
    return (context.scheduler.now().in_femtoseconds() + one_ns_fs) /
           clock_period_fs;
}

void report(Context& context) {
    const auto elapsed = std::chrono::steady_clock::now() - context.result.start;
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    const auto& feature = context.result.features;
    std::printf(
        "AUTHORING_CORE_RESULT mode=cpp_dpi kernel=%s iterations=%u "
        "transactions=%llu checks=%llu sim_cycles=%llu checksum=%u failures=%u "
        "task_value=%llu clock_cycles=%llu timeouts=%llu timeout_hits=%llu "
        "task_timeouts=%llu task_timeout_hits=%llu "
        "wait_until=%llu event_set=%llu event_wait=%llu channel_send=%llu "
        "channel_receive=%llu wall_ms=%.3f\n",
        kernel_name(), context.iterations,
        static_cast<unsigned long long>(context.result.transactions),
        static_cast<unsigned long long>(context.result.checks),
        static_cast<unsigned long long>(reported_sim_cycles(context)),
        context.result.checksum, context.result.failures,
        static_cast<unsigned long long>(feature.task_value),
        static_cast<unsigned long long>(feature.clock_cycles),
        static_cast<unsigned long long>(feature.timeouts),
        static_cast<unsigned long long>(feature.timeout_hits),
        static_cast<unsigned long long>(feature.task_timeouts),
        static_cast<unsigned long long>(feature.task_timeout_hits),
        static_cast<unsigned long long>(feature.wait_until),
        static_cast<unsigned long long>(feature.event_set),
        static_cast<unsigned long long>(feature.event_wait),
        static_cast<unsigned long long>(feature.channel_send),
        static_cast<unsigned long long>(feature.channel_receive),
        static_cast<double>(elapsed_us) / 1000.0);
}

Task<void> run(Context context) {
    context.dut.rst_n.set(0);
    context.dut.req_valid.set(0);
    context.dut.req_data.set(0);
    context.dut.rsp_ready.set(1);
    for (uint32_t cycle = 0; cycle < 4; ++cycle) {
        co_await RisingEdge{context.dut.clk};
    }
    context.dut.rst_n.set(1);

    Event event;
    Channel<uint32_t> channel;

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        uint32_t payload = stimulus(iteration);

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_TASK_VALUE || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        payload = co_await authored_value(iteration);
        ++context.result.features.task_value;
        check(context, "task value", payload, stimulus(iteration));
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_CLOCK_CYCLES || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        ++context.result.features.clock_cycles;
        co_await clock_cycles(context.dut.clk, 1);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_TIMEOUT || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        static_cast<void>(co_await timeout_probe(context, iteration));
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_TASK_TIMEOUT || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        payload = co_await task_timeout_probe(context, iteration, payload);
#endif

        bool ready_already = false;
#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_WAIT_UNTIL || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        ++context.result.features.wait_until;
        co_await wait_until(context.dut.req_ready,
                            [](uint32_t value) { return value != 0; },
                            context.dut.clk);
        check(context, "wait_until ready", context.dut.req_ready.get(), 1);
        ready_already = true;
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_EVENT || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        co_await event_roundtrip(context, event);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_CHANNEL || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        ++context.result.features.channel_send;
        channel.put_nowait(payload);
        ++context.result.features.channel_receive;
        const uint32_t received = co_await channel.get();
        check(context, "channel payload", received, payload);
        payload = received;
#endif

        co_await transact(context, iteration, payload, ready_already);
    }

    co_await wait_for_response_count(context);
    check(context, "request count", context.dut.request_count.get(),
          context.iterations);
    check(context, "response count", context.dut.response_count.get(),
          context.iterations);
    report(context);
}

}  // namespace

void register_benchmark(coro::Testbench& scheduler, AuthoringCoreDut dut,
                        uint32_t iterations, BenchResult& result) {
    result = BenchResult{};
    result.start = std::chrono::steady_clock::now();
    scheduler.spawn_detached(run(Context{scheduler, dut, iterations, result}));
}

}  // namespace cpptb::benchmarks::authoring_core
