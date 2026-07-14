#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string_view>

#include "Vheavy_benchmark_vpi_top.h"
#include "cpptb/coro_runtime.hpp"
#include "verilated.h"
#include "verilated_vpi.h"
#include "vpi_user.h"

namespace {

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
constexpr uint64_t kHalfPeriodTicks = 1'000;
constexpr uint64_t kClockPeriodFs = 2'000'000;

enum class Workload { Fir, Crc32, Matrix };

struct Dut {
    Signal clk;
    Signal rst_n;
    Signal fir_in_valid;
    Signal fir_in_ready;
    Signal fir_in_sample;
    Signal fir_out_valid;
    Signal fir_out_ready;
    Signal fir_out_result;
    Signal fir_sample_count;
    Signal crc_in_valid;
    Signal crc_in_ready;
    Signal crc_in_data;
    Signal crc_in_last;
    Signal crc_out_valid;
    Signal crc_out_ready;
    Signal crc_out_result;
    Signal crc_packet_count;
    Signal mat_load_valid;
    Signal mat_load_ready;
    Signal mat_load_select;
    Signal mat_load_index;
    Signal mat_load_data;
    Signal mat_start;
    Signal mat_out_valid;
    Signal mat_out_ready;
    Signal mat_out_index;
    Signal mat_out_data;
    Signal mat_block_count;
};

struct BenchResult {
    uint64_t transactions = 0;
    uint64_t checks = 0;
    uint32_t checksum = 0x811c'9dc5u;
    uint32_t failures = 0;
};

struct Context {
    Testbench& scheduler;
    Dut dut;
    Workload workload;
    uint32_t iterations;
    BenchResult& result;
};

uint64_t main_time_ticks = 0;

const char* workload_name(Workload workload) {
    switch (workload) {
        case Workload::Fir:
            return "streaming_fir";
        case Workload::Crc32:
            return "packet_crc32";
        case Workload::Matrix:
            return "matrix4x4";
    }
    return "unknown";
}

Workload parse_workload(const char* value) {
    const std::string_view name = value ? value : "streaming_fir";
    if (name == "streaming_fir") return Workload::Fir;
    if (name == "packet_crc32") return Workload::Crc32;
    if (name == "matrix4x4") return Workload::Matrix;
    std::fprintf(stderr, "heavy VPI: unsupported workload '%.*s'\n",
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
        std::fprintf(stderr, "heavy VPI: invalid %s='%s'\n", name, value);
        std::exit(2);
    }
    return static_cast<uint32_t>(parsed);
}

vpiHandle require_handle(const char* name) {
    char path[192];
    std::snprintf(path, sizeof(path), "TOP.heavy_benchmark_vpi_top.%s", name);
    auto* handle = vpi_handle_by_name(reinterpret_cast<PLI_BYTE8*>(path), nullptr);
    if (handle) return handle;
    std::fprintf(stderr, "heavy VPI: missing handle for %s\n", path);
    std::exit(1);
}

Signal bind_signal(const char* name, uint32_t id) {
    return make_vpi_signal(require_handle(name), id, name);
}

Dut bind_dut() {
    uint32_t id = 0;
    return Dut{
        bind_signal("clk", id++),
        bind_signal("rst_n", id++),
        bind_signal("fir_in_valid", id++),
        bind_signal("fir_in_ready", id++),
        bind_signal("fir_in_sample", id++),
        bind_signal("fir_out_valid", id++),
        bind_signal("fir_out_ready", id++),
        bind_signal("fir_out_result", id++),
        bind_signal("fir_sample_count", id++),
        bind_signal("crc_in_valid", id++),
        bind_signal("crc_in_ready", id++),
        bind_signal("crc_in_data", id++),
        bind_signal("crc_in_last", id++),
        bind_signal("crc_out_valid", id++),
        bind_signal("crc_out_ready", id++),
        bind_signal("crc_out_result", id++),
        bind_signal("crc_packet_count", id++),
        bind_signal("mat_load_valid", id++),
        bind_signal("mat_load_ready", id++),
        bind_signal("mat_load_select", id++),
        bind_signal("mat_load_index", id++),
        bind_signal("mat_load_data", id++),
        bind_signal("mat_start", id++),
        bind_signal("mat_out_valid", id++),
        bind_signal("mat_out_ready", id++),
        bind_signal("mat_out_index", id++),
        bind_signal("mat_out_data", id++),
        bind_signal("mat_block_count", id++),
    };
}

uint32_t stimulus(uint32_t ordinal) {
    return ((ordinal + 1u) * 0x1f12'3bb5u) ^ 0xc001'd00du;
}

int32_t fir_coefficient(uint32_t tap) {
    return static_cast<int32_t>((tap * 7u) % 19u) - 9;
}

uint32_t crc32_byte(uint32_t current, uint8_t data) {
    uint32_t value = current ^ data;
    for (uint32_t bit = 0; bit < 8; ++bit)
        value = (value & 1u) ? ((value >> 1u) ^ 0xedb8'8320u)
                             : (value >> 1u);
    return value;
}

int16_t matrix_value(uint32_t block, uint32_t matrix, uint32_t index) {
    const uint32_t raw = (stimulus(block * 32u + matrix * 16u + index) >> 8u) &
                         0x7ffu;
    return static_cast<int16_t>(static_cast<int32_t>(raw) - 1024);
}

void check(Context& context, const char* label, uint32_t actual,
           uint32_t expected) {
    ++context.result.checks;
    if (actual == expected) return;
    ++context.result.failures;
    if (context.result.failures <= 8) {
        std::printf(
            "HEAVY_BENCH_MISMATCH mode=cpp_vpi workload=%s label=%s "
            "actual=0x%08x expected=0x%08x\n",
            workload_name(context.workload), label, actual, expected);
    }
}

void fold(Context& context, uint32_t value) {
    context.result.checksum =
        (context.result.checksum ^ value) * 0x0100'0193u;
}

Task<void> reset(Context& context) {
    context.dut.rst_n.set(0);
    context.dut.fir_in_valid.set(0);
    context.dut.fir_in_sample.set(0);
    context.dut.fir_out_ready.set(1);
    context.dut.crc_in_valid.set(0);
    context.dut.crc_in_data.set(0);
    context.dut.crc_in_last.set(0);
    context.dut.crc_out_ready.set(1);
    context.dut.mat_load_valid.set(0);
    context.dut.mat_load_select.set(0);
    context.dut.mat_load_index.set(0);
    context.dut.mat_load_data.set(0);
    context.dut.mat_start.set(0);
    context.dut.mat_out_ready.set(1);
    for (uint32_t cycle = 0; cycle < 4; ++cycle)
        co_await RisingEdge{context.dut.clk};
    context.dut.rst_n.set(1);
}

Task<void> run_fir(Context& context) {
    std::array<int16_t, 32> history{};
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const int16_t sample = static_cast<int16_t>(stimulus(iteration));
        int64_t expected = static_cast<int64_t>(sample) * fir_coefficient(0);
        for (uint32_t tap = 1; tap < history.size(); ++tap)
            expected += static_cast<int64_t>(history[tap - 1]) *
                        fir_coefficient(tap);
        for (uint32_t tap = history.size() - 1; tap > 0; --tap)
            history[tap] = history[tap - 1];
        history[0] = sample;
        co_await FallingEdge{context.dut.clk};
        context.dut.fir_in_sample.set(static_cast<uint16_t>(sample));
        context.dut.fir_in_valid.set(1);
        co_await RisingEdge{context.dut.clk};
        co_await Delay{1_ps};
        const uint32_t result = context.dut.fir_out_result.get();
        check(context, "FIR result", result, static_cast<uint32_t>(expected));
        fold(context, result);
        ++context.result.transactions;
    }
    context.dut.fir_in_valid.set(0);
    check(context, "FIR accepted sample count", context.dut.fir_sample_count.get(),
          context.iterations);
}

Task<void> run_crc(Context& context) {
    for (uint32_t packet = 0; packet < context.iterations; ++packet) {
        uint32_t expected = 0xffff'ffffu;
        const uint32_t length = 32u + (packet & 63u);
        for (uint32_t offset = 0; offset < length; ++offset) {
            const uint8_t data = static_cast<uint8_t>(stimulus(packet * 96u + offset));
            expected = crc32_byte(expected, data);
            co_await FallingEdge{context.dut.clk};
            context.dut.crc_in_data.set(data);
            context.dut.crc_in_last.set(offset + 1u == length);
            context.dut.crc_in_valid.set(1);
            co_await RisingEdge{context.dut.clk};
        }
        co_await Delay{1_ps};
        const uint32_t result = context.dut.crc_out_result.get();
        check(context, "packet CRC32", result, ~expected);
        fold(context, result);
        ++context.result.transactions;
    }
    context.dut.crc_in_valid.set(0);
    context.dut.crc_in_last.set(0);
    check(context, "CRC packet count", context.dut.crc_packet_count.get(),
          context.iterations);
}

Task<void> load_matrix_value(Context& context, uint32_t select, uint32_t index,
                             int16_t value) {
    while (context.dut.mat_load_ready.get() == 0) {
        co_await RisingEdge{context.dut.clk};
        co_await Delay{1_ps};
    }
    co_await FallingEdge{context.dut.clk};
    context.dut.mat_load_select.set(select);
    context.dut.mat_load_index.set(index);
    context.dut.mat_load_data.set(static_cast<uint16_t>(value));
    context.dut.mat_load_valid.set(1);
    co_await RisingEdge{context.dut.clk};
}

Task<void> run_matrix(Context& context) {
    std::array<int16_t, 16> matrix_a{};
    std::array<int16_t, 16> matrix_b{};
    for (uint32_t block = 0; block < context.iterations; ++block) {
        for (uint32_t index = 0; index < 16; ++index) {
            matrix_a[index] = matrix_value(block, 0, index);
            co_await load_matrix_value(context, 0, index, matrix_a[index]);
        }
        for (uint32_t index = 0; index < 16; ++index) {
            matrix_b[index] = matrix_value(block, 1, index);
            co_await load_matrix_value(context, 1, index, matrix_b[index]);
        }
        co_await FallingEdge{context.dut.clk};
        context.dut.mat_load_valid.set(0);
        context.dut.mat_start.set(1);
        co_await RisingEdge{context.dut.clk};
        co_await FallingEdge{context.dut.clk};
        context.dut.mat_start.set(0);
        for (uint32_t output = 0; output < 16; ++output) {
            co_await RisingEdge{context.dut.clk};
            co_await Delay{1_ps};
            const uint32_t row = output / 4u;
            const uint32_t column = output % 4u;
            int64_t expected = 0;
            for (uint32_t element = 0; element < 4; ++element)
                expected += static_cast<int64_t>(matrix_a[row * 4u + element]) *
                            matrix_b[element * 4u + column];
            check(context, "matrix output index", context.dut.mat_out_index.get(),
                  output);
            const uint32_t result = context.dut.mat_out_data.get();
            check(context, "matrix output data", result,
                  static_cast<uint32_t>(expected));
            fold(context, result);
        }
        ++context.result.transactions;
    }
    check(context, "matrix block count", context.dut.mat_block_count.get(),
          context.iterations);
}

Task<void> run(Context context) {
    co_await reset(context);
    if (context.workload == Workload::Fir)
        co_await run_fir(context);
    else if (context.workload == Workload::Crc32)
        co_await run_crc(context);
    else
        co_await run_matrix(context);
}

void settle(Vheavy_benchmark_vpi_top* top) {
    VerilatedVpi::clearEvalNeeded();
    top->eval();
    VerilatedVpi::callValueCbs();
    for (int pass = 0; pass < 8 && VerilatedVpi::evalNeeded(); ++pass) {
        VerilatedVpi::clearEvalNeeded();
        top->eval();
        VerilatedVpi::callValueCbs();
    }
}

void update_time(Testbench& scheduler, VerilatedContext& context,
                 Vheavy_benchmark_vpi_top* top, uint64_t ticks) {
    main_time_ticks = ticks;
    context.time(ticks);
    settle(top);
    scheduler.set_time(ticks);
    settle(top);
}

}  // namespace

double sc_time_stamp() { return static_cast<double>(main_time_ticks); }

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    const std::unique_ptr<VerilatedContext> verilated_context{new VerilatedContext};
    const std::unique_ptr<Vheavy_benchmark_vpi_top> top{
        new Vheavy_benchmark_vpi_top{verilated_context.get()}};
    Testbench scheduler{1_ps};
    settle(top.get());
    Dut dut = bind_dut();
    const Workload workload = parse_workload(std::getenv("HEAVY_BENCH_WORKLOAD"));
    const uint32_t iterations = env_u32("HEAVY_BENCH_ITERS", 1000);
    BenchResult result;

    update_time(scheduler, *verilated_context, top.get(), 0);
    dut.clk.set(0);
    settle(top.get());
    const auto start = std::chrono::steady_clock::now();
    scheduler.spawn_detached(run(Context{scheduler, dut, workload, iterations, result}));
    settle(top.get());

    bool clock_high = false;
    uint64_t next_clock_edge_ticks = kHalfPeriodTicks;
    const uint64_t max_time_ticks =
        static_cast<uint64_t>(iterations) * 256'000u + 2'000'000u;
    while (!scheduler.done() && main_time_ticks <= max_time_ticks) {
        const uint64_t next_timer_ticks = scheduler.next_timer_deadline();
        const uint64_t next_event_ticks =
            std::min(next_clock_edge_ticks, next_timer_ticks);
        if (next_event_ticks == std::numeric_limits<uint64_t>::max()) break;
        update_time(scheduler, *verilated_context, top.get(), next_event_ticks);
        if (next_event_ticks == next_clock_edge_ticks) {
            clock_high = !clock_high;
            dut.clk.set(clock_high ? 1u : 0u);
            scheduler.notify_edge(kSignalClk,
                                  clock_high ? EdgeKind::Rising : EdgeKind::Falling);
            settle(top.get());
            next_clock_edge_ticks += kHalfPeriodTicks;
        }
    }

    const auto finish = std::chrono::steady_clock::now();
    const auto wall_us =
        std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count();
    if (!scheduler.done()) {
        std::fprintf(stderr, "heavy VPI: timed out at %llu ps for %s\n",
                     static_cast<unsigned long long>(main_time_ticks),
                     workload_name(workload));
        return 1;
    }
    top->final();
    const uint64_t sim_cycles =
        (scheduler.now().in_femtoseconds() + 1'000'000u) / kClockPeriodFs;
    std::printf(
        "HEAVY_BENCH_RESULT mode=cpp_vpi workload=%s iterations=%u "
        "transactions=%llu checks=%llu sim_cycles=%llu checksum=%u "
        "failures=%u wall_ms=%.3f\n",
        workload_name(workload), iterations,
        static_cast<unsigned long long>(result.transactions),
        static_cast<unsigned long long>(result.checks),
        static_cast<unsigned long long>(sim_cycles), result.checksum,
        result.failures, static_cast<double>(wall_us) / 1000.0);
    return result.failures == 0 ? 0 : 1;
}
