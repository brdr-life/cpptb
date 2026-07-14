#include "benchmarks/framework_comparison/heavy_suite/testbenches/cpp_dpi/framework/heavy_benchmark.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace cpptb::benchmarks::heavy {
namespace {

using coro::Delay;
using coro::FallingEdge;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

struct Context {
    coro::Testbench& scheduler;
    Dut dut;
    uint32_t iterations;
    BenchResult& result;
};

uint32_t stimulus(uint32_t ordinal) {
    return ((ordinal + 1u) * 0x1f12'3bb5u) ^ 0xc001'd00du;
}

int32_t fir_coefficient(uint32_t tap) {
    return static_cast<int32_t>((tap * 7u) % 19u) - 9;
}

int16_t fir_sample(uint32_t iteration) {
    return static_cast<int16_t>(stimulus(iteration));
}

uint8_t packet_byte(uint32_t packet, uint32_t offset) {
    return static_cast<uint8_t>(stimulus(packet * 96u + offset));
}

uint32_t packet_length(uint32_t packet) { return 32u + (packet & 63u); }

uint32_t crc32_byte(uint32_t current, uint8_t data) {
    uint32_t value = current ^ data;
    for (uint32_t bit = 0; bit < 8; ++bit) {
        value = (value & 1u) ? ((value >> 1u) ^ 0xedb8'8320u)
                             : (value >> 1u);
    }
    return value;
}

int16_t matrix_value(uint32_t block, uint32_t matrix, uint32_t index) {
    const uint32_t raw = (stimulus(block * 32u + matrix * 16u + index) >> 8u) &
                         0x7ffu;
    return static_cast<int16_t>(static_cast<int32_t>(raw) - 1024);
}

void fold(Context& context, uint32_t value) {
    context.result.checksum =
        (context.result.checksum ^ value) * 0x0100'0193u;
}

void check(Context& context, const char* label, uint32_t actual,
           uint32_t expected) {
    ++context.result.checks;
    if (actual == expected) return;
    ++context.result.failures;
    if (context.result.failures <= 8) {
        std::printf(
            "HEAVY_BENCH_MISMATCH mode=cpp_dpi workload=%s label=%s "
            "actual=0x%08x expected=0x%08x\n",
            workload_name(), label, actual, expected);
    }
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
    for (uint32_t cycle = 0; cycle < 4; ++cycle) {
        co_await RisingEdge{context.dut.clk};
    }
    context.dut.rst_n.set(1);
}

Task<void> run_fir(Context& context) {
    std::array<int16_t, 32> history{};
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const int16_t sample = fir_sample(iteration);
        int64_t expected = static_cast<int64_t>(sample) * fir_coefficient(0);
        for (uint32_t tap = 1; tap < history.size(); ++tap) {
            expected += static_cast<int64_t>(history[tap - 1]) *
                        fir_coefficient(tap);
        }
        for (uint32_t tap = history.size() - 1; tap > 0; --tap) {
            history[tap] = history[tap - 1];
        }
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
    check(context, "FIR accepted sample count",
          context.dut.fir_sample_count.get(), context.iterations);
}

Task<void> run_crc(Context& context) {
    for (uint32_t packet = 0; packet < context.iterations; ++packet) {
        uint32_t expected = 0xffff'ffffu;
        const uint32_t length = packet_length(packet);
        for (uint32_t offset = 0; offset < length; ++offset) {
            const uint8_t data = packet_byte(packet, offset);
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
            for (uint32_t element = 0; element < 4; ++element) {
                expected += static_cast<int64_t>(matrix_a[row * 4u + element]) *
                            matrix_b[element * 4u + column];
            }
            check(context, "matrix output index",
                  context.dut.mat_out_index.get(), output);
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

uint64_t reported_sim_cycles(Context& context) {
    return (context.scheduler.now().in_femtoseconds() + 1'000'000u) /
           2'000'000u;
}

void report(Context& context) {
    const auto elapsed = std::chrono::steady_clock::now() - context.result.start;
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    std::printf(
        "HEAVY_BENCH_RESULT mode=cpp_dpi workload=%s iterations=%u "
        "transactions=%llu checks=%llu sim_cycles=%llu checksum=%u "
        "failures=%u wall_ms=%.3f\n",
        workload_name(), context.iterations,
        static_cast<unsigned long long>(context.result.transactions),
        static_cast<unsigned long long>(context.result.checks),
        static_cast<unsigned long long>(reported_sim_cycles(context)),
        context.result.checksum, context.result.failures,
        static_cast<double>(elapsed_us) / 1000.0);
}

Task<void> run(Context context) {
    co_await reset(context);
#if HEAVY_WORKLOAD == HEAVY_WORKLOAD_FIR
    co_await run_fir(context);
#elif HEAVY_WORKLOAD == HEAVY_WORKLOAD_CRC32
    co_await run_crc(context);
#elif HEAVY_WORKLOAD == HEAVY_WORKLOAD_MATRIX
    co_await run_matrix(context);
#endif
    report(context);
}

}  // namespace

void register_benchmark(coro::Testbench& scheduler, Dut dut,
                        uint32_t iterations, BenchResult& result,
                        coro::ClockRegistrar clocks) {
    result = BenchResult{};
    result.start = std::chrono::steady_clock::now();
    dut.clk.set(0);
    clocks.start(dut.clk, 2_ns);
    scheduler.spawn_detached(run(Context{scheduler, dut, iterations, result}));
}

}  // namespace cpptb::benchmarks::heavy
