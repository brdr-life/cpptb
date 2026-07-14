#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>

#include "Vauthoring_core_vpi_top.h"
#include "cpptb/coro_runtime.hpp"
#include "verilated.h"
#include "verilated_vpi.h"
#include "vpi_user.h"

namespace {

using cpptb::Bits;
using cpptb::coro::Delay;
using cpptb::coro::EdgeKind;
using cpptb::coro::FallingEdge;
using cpptb::coro::RisingEdge;
using cpptb::coro::Signal;
using cpptb::coro::Task;
using cpptb::coro::Testbench;
using cpptb::coro::make_vpi_signal;
using namespace cpptb::coro;

constexpr uint32_t kSignalClk = 0;
constexpr uint32_t kSignalRspValid = 1;
constexpr uint64_t kHalfPeriodTicks = 1'000;
constexpr uint64_t kClockPeriodFs = 2'000'000;

enum class Workload {
    Control,
    WideEcho137,
    SignalEdge,
};

struct Wide137Signal {
    vpiHandle handle = nullptr;
    const char* name = "";

    Bits<137> get() const {
        s_vpi_value value{};
        value.format = vpiVectorVal;
        vpi_get_value(handle, &value);

        Bits<137> result;
        for (uint32_t word = 0; word < Bits<137>::word_count; ++word) {
            result.set_word(word,
                            static_cast<uint32_t>(value.value.vector[word].aval));
        }
        return result;
    }

    void set(const Bits<137>& data) const {
        std::array<s_vpi_vecval, Bits<137>::word_count> words{};
        for (uint32_t word = 0; word < words.size(); ++word) {
            words[word].aval = static_cast<PLI_UINT32>(data.word(word));
            words[word].bval = 0;
        }
        s_vpi_value value{};
        value.format = vpiVectorVal;
        value.value.vector = words.data();
        vpi_put_value(handle, &value, nullptr, vpiNoDelay);
    }
};

struct Dut {
    Signal clk;
    Signal rst_n;
    Signal req_valid;
    Signal req_data;
    Signal req_ready;
    Signal rsp_valid;
    Signal rsp_data;
    Signal rsp_ready;
    Signal request_count;
    Signal response_count;
    Wide137Signal wide137_i;
    Wide137Signal wide137_o;
};

struct BenchResult {
    uint64_t transactions = 0;
    uint64_t checks = 0;
    uint32_t checksum = 0x811c'9dc5u;
    uint32_t failures = 0;
    uint64_t wide_echo_137 = 0;
    uint64_t signal_edges = 0;
};

struct Context {
    Testbench& scheduler;
    Dut dut;
    Workload workload;
    uint32_t iterations;
    BenchResult& result;
};

struct SignalWatch {
    Signal signal;
    vpiHandle callback = nullptr;
    s_vpi_time time{};
    s_vpi_value value{};
    s_cb_data callback_data{};
};

uint64_t main_time_ticks = 0;
Testbench* active_testbench = nullptr;
std::array<SignalWatch, 1> watches;

uint32_t stimulus(uint32_t iteration) {
    return ((iteration + 1u) * 0x1f12'3bb5u) ^ 0xc001'd00du;
}

uint32_t expected_response(uint32_t iteration) {
    return (stimulus(iteration) ^ 0xa5a5'5a5au) + iteration;
}

Bits<137> wide137_stimulus(uint32_t iteration) {
    Bits<137> value;
    for (uint32_t word = 0; word < Bits<137>::word_count; ++word) {
        value.set_word(word, stimulus(iteration * 5u + word));
    }
    return value;
}

Bits<137> wide137_response(Bits<137> value) {
    constexpr uint32_t masks[] = {
        0xdead'beefu,
        0x89ab'cdefu,
        0x0123'4567u,
        0x5aa5'5aa5u,
        0x0000'01a5u,
    };
    for (uint32_t word = 0; word < Bits<137>::word_count; ++word) {
        value.set_word(word, value.word(word) ^ masks[word]);
    }
    return value;
}

const char* workload_name(Workload workload) {
    switch (workload) {
        case Workload::Control:
            return "control";
        case Workload::WideEcho137:
            return "wide_echo_137";
        case Workload::SignalEdge:
            return "signal_edge";
    }
    return "unknown";
}

Workload parse_workload(const char* value) {
    const std::string_view name = value ? value : "control";
    if (name == "control") return Workload::Control;
    if (name == "wide_echo_137") return Workload::WideEcho137;
    if (name == "signal_edge") return Workload::SignalEdge;
    std::fprintf(stderr, "framework-comparison: unsupported workload '%.*s'\n",
                 static_cast<int>(name.size()), name.data());
    std::exit(2);
}

uint32_t env_u32(const char* name, uint32_t default_value) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') return default_value;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (!end || *end != '\0' || parsed == 0 ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        std::fprintf(stderr, "framework-comparison: invalid %s='%s'\n", name,
                     value);
        std::exit(2);
    }
    return static_cast<uint32_t>(parsed);
}

vpiHandle require_handle(const char* path) {
    auto* handle = vpi_handle_by_name(const_cast<PLI_BYTE8*>(path), nullptr);
    if (handle) return handle;
    std::fprintf(stderr, "framework-comparison: missing VPI handle for %s\n",
                 path);
    std::exit(1);
}

Signal bind_signal(const char* name, uint32_t id) {
    char path[192];
    std::snprintf(path, sizeof(path), "TOP.authoring_core_vpi_top.%s", name);
    return make_vpi_signal(require_handle(path), id, name);
}

Wide137Signal bind_wide(const char* name) {
    char path[192];
    std::snprintf(path, sizeof(path), "TOP.authoring_core_vpi_top.%s", name);
    return Wide137Signal{require_handle(path), name};
}

Dut bind_dut() {
    return Dut{
        bind_signal("clk", kSignalClk),
        bind_signal("rst_n", 2),
        bind_signal("req_valid", 3),
        bind_signal("req_data", 4),
        bind_signal("req_ready", 5),
        bind_signal("rsp_valid", kSignalRspValid),
        bind_signal("rsp_data", 6),
        bind_signal("rsp_ready", 7),
        bind_signal("request_count", 8),
        bind_signal("response_count", 9),
        bind_wide("wide137_i"),
        bind_wide("wide137_o"),
    };
}

PLI_INT32 on_value_change(p_cb_data callback_data) {
    auto* watch = reinterpret_cast<SignalWatch*>(callback_data->user_data);
    if (!watch || !active_testbench) return 0;
    active_testbench->notify_edge(
        watch->signal.id,
        watch->signal.get() ? EdgeKind::Rising : EdgeKind::Falling);
    return 0;
}

void register_edge_callback(size_t watch_index, Signal signal) {
    auto& watch = watches.at(watch_index);
    watch.signal = signal;
    watch.time.type = vpiSimTime;
    watch.value.format = vpiIntVal;
    watch.callback_data.reason = cbValueChange;
    watch.callback_data.cb_rtn = on_value_change;
    watch.callback_data.obj = signal.handle;
    watch.callback_data.time = &watch.time;
    watch.callback_data.value = &watch.value;
    watch.callback_data.user_data = reinterpret_cast<PLI_BYTE8*>(&watch);
    watch.callback = vpi_register_cb(&watch.callback_data);
    if (watch.callback) return;
    std::fprintf(stderr,
                 "framework-comparison: failed to register callback for %s\n",
                 signal.name);
    std::exit(1);
}

void settle(Vauthoring_core_vpi_top* top) {
    VerilatedVpi::clearEvalNeeded();
    top->eval();
    VerilatedVpi::callValueCbs();
    for (int pass = 0; pass < 8 && VerilatedVpi::evalNeeded(); ++pass) {
        VerilatedVpi::clearEvalNeeded();
        top->eval();
        VerilatedVpi::callValueCbs();
    }
}

void update_time(Testbench& scheduler, VerilatedContext& verilated_context,
                 Vauthoring_core_vpi_top* top, uint64_t time_ticks) {
    main_time_ticks = time_ticks;
    verilated_context.time(time_ticks);
    settle(top);
    scheduler.set_time(time_ticks);
    settle(top);
}

void check(Context& context, const char* label, uint32_t actual,
           uint32_t expected) {
    ++context.result.checks;
    if (actual == expected) return;
    ++context.result.failures;
    if (context.result.failures <= 8) {
        std::printf(
            "FRAMEWORK_COMPARISON_MISMATCH mode=cpp_vpi workload=%s "
            "label=%s actual=0x%08x expected=0x%08x\n",
            workload_name(context.workload), label, actual, expected);
    }
}

void check137(Context& context, const char* label, const Bits<137>& actual,
              const Bits<137>& expected) {
    ++context.result.checks;
    if (actual == expected) return;
    ++context.result.failures;
    if (context.result.failures <= 8) {
        std::printf(
            "FRAMEWORK_COMPARISON_MISMATCH mode=cpp_vpi workload=%s "
            "label=%s actual_word0=0x%08x expected_word0=0x%08x\n",
            workload_name(context.workload), label, actual.word(0),
            expected.word(0));
    }
}

Task<void> wait_ready(Context& context) {
    while (context.dut.req_ready.get() == 0) {
        co_await RisingEdge{context.dut.clk};
    }
}

Task<void> transact(Context& context, uint32_t iteration, uint32_t payload) {
    co_await wait_ready(context);
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

Task<void> transact_signal_edge(Context& context, uint32_t iteration,
                                uint32_t payload) {
    co_await wait_ready(context);
    co_await FallingEdge{context.dut.clk};
    context.dut.req_data.set(payload);
    context.dut.req_valid.set(1);

    co_await RisingEdge{context.dut.clk};
    co_await FallingEdge{context.dut.clk};
    context.dut.req_valid.set(0);

    co_await RisingEdge{context.dut.rsp_valid};
    ++context.result.signal_edges;
    const uint32_t response = context.dut.rsp_data.get();
    check(context, "response", response, expected_response(iteration));
    context.result.checksum =
        (context.result.checksum ^ response) * 0x0100'0193u;
    ++context.result.transactions;
}

Task<void> check_wide137(Context& context, uint32_t iteration) {
    const Bits<137> value = wide137_stimulus(iteration);
    ++context.result.wide_echo_137;
    co_await FallingEdge{context.dut.clk};
    context.dut.wide137_i.set(value);
    co_await RisingEdge{context.dut.clk};
    co_await Delay{1_ps};
    check137(context, "wide137", context.dut.wide137_o.get(),
             wide137_response(value));
}

Task<void> run(Context context) {
    context.dut.rst_n.set(0);
    context.dut.req_valid.set(0);
    context.dut.req_data.set(0);
    context.dut.rsp_ready.set(1);
    context.dut.wide137_i.set(Bits<137>{});

    for (uint32_t cycle = 0; cycle < 4; ++cycle) {
        co_await RisingEdge{context.dut.clk};
    }
    context.dut.rst_n.set(1);

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        if (context.workload == Workload::WideEcho137) {
            co_await check_wide137(context, iteration);
        }

        const uint32_t payload = stimulus(iteration);
        if (context.workload == Workload::SignalEdge) {
            co_await transact_signal_edge(context, iteration, payload);
        } else {
            co_await transact(context, iteration, payload);
        }
    }

    while (context.dut.response_count.get() != context.iterations) {
        co_await RisingEdge{context.dut.clk};
        co_await Delay{1_ps};
    }
    check(context, "request count", context.dut.request_count.get(),
          context.iterations);
    check(context, "response count", context.dut.response_count.get(),
          context.iterations);
}

}  // namespace

double sc_time_stamp() { return static_cast<double>(main_time_ticks); }

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    const std::unique_ptr<VerilatedContext> verilated_context{
        new VerilatedContext};
    const std::unique_ptr<Vauthoring_core_vpi_top> top{
        new Vauthoring_core_vpi_top{verilated_context.get()}};

    Testbench scheduler{1_ps};
    active_testbench = &scheduler;
    settle(top.get());
    Dut dut = bind_dut();
    register_edge_callback(0, dut.rsp_valid);

    const Workload workload =
        parse_workload(std::getenv("FRAMEWORK_COMPARISON_WORKLOAD"));
    const uint32_t iterations =
        env_u32("FRAMEWORK_COMPARISON_ITERS", 1000);
    BenchResult result;

    update_time(scheduler, *verilated_context, top.get(), 0);
    dut.clk.set(0);
    settle(top.get());

    const auto start = std::chrono::steady_clock::now();
    scheduler.spawn_detached(
        run(Context{scheduler, dut, workload, iterations, result}));
    settle(top.get());

    bool clock_high = false;
    uint64_t next_clock_edge_ticks = kHalfPeriodTicks;
    const uint64_t max_time_ticks =
        static_cast<uint64_t>(iterations) * 20'000u + 20'000u;

    while (!scheduler.done() && main_time_ticks <= max_time_ticks) {
        const uint64_t next_timer_ticks = scheduler.next_timer_deadline();
        const uint64_t next_event_ticks =
            std::min(next_clock_edge_ticks, next_timer_ticks);
        if (next_event_ticks == std::numeric_limits<uint64_t>::max()) break;

        update_time(scheduler, *verilated_context, top.get(), next_event_ticks);

        if (next_event_ticks == next_clock_edge_ticks) {
            clock_high = !clock_high;
            dut.clk.set(clock_high ? 1u : 0u);
            scheduler.notify_edge(
                kSignalClk,
                clock_high ? EdgeKind::Rising : EdgeKind::Falling);
            settle(top.get());
            next_clock_edge_ticks += kHalfPeriodTicks;
        }
    }

    const auto finish = std::chrono::steady_clock::now();
    const auto wall_us =
        std::chrono::duration_cast<std::chrono::microseconds>(finish - start)
            .count();

    if (!scheduler.done()) {
        std::fprintf(stderr,
                     "framework-comparison: timed out at %llu fs for %s\n",
                     static_cast<unsigned long long>(main_time_ticks * 1'000u),
                     workload_name(workload));
        return 1;
    }

    top->final();
    const uint64_t sim_cycles =
        (scheduler.now().in_femtoseconds() + 1'000'000u) / kClockPeriodFs;
    std::printf(
        "FRAMEWORK_COMPARISON_RESULT mode=cpp_vpi workload=%s "
        "iterations=%u transactions=%llu checks=%llu sim_cycles=%llu "
        "checksum=%u failures=%u wide_echo_137=%llu signal_edges=%llu "
        "wall_ms=%.3f\n",
        workload_name(workload), iterations,
        static_cast<unsigned long long>(result.transactions),
        static_cast<unsigned long long>(result.checks),
        static_cast<unsigned long long>(sim_cycles), result.checksum,
        result.failures,
        static_cast<unsigned long long>(result.wide_echo_137),
        static_cast<unsigned long long>(result.signal_edges),
        static_cast<double>(wall_us) / 1000.0);
    return result.failures == 0 ? 0 : 1;
}
