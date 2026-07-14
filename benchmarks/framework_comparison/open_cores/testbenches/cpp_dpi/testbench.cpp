#include "benchmarks/framework_comparison/open_cores/testbenches/cpp_dpi/framework/open_cores_benchmark.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace cpptb::benchmarks::open_cores {
namespace {

using coro::Delay;
using coro::FallingEdge;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

constexpr std::array<uint32_t, 14> kFirmware = {
    0x1000'2083u, 0x1234'5137u, 0x6781'0113u, 0x00d1'1193u,
    0x0031'4133u, 0x0111'5193u, 0x0031'4133u, 0x0051'1193u,
    0x0031'4133u, 0xfff0'8093u, 0xfe00'92e3u, 0x1000'0237u,
    0x0022'2023u, 0x0000'006fu,
};
constexpr std::array<uint32_t, 8> kAesKey = {
    0x2b7e'1516u, 0x28ae'd2a6u, 0xabf7'1588u, 0x09cf'4f3cu,
    0, 0, 0, 0,
};
constexpr std::array<std::array<uint32_t, 4>, 4> kAesPlaintext = {{
    {0x6bc1'bee2u, 0x2e40'9f96u, 0xe93d'7e11u, 0x7393'172au},
    {0xae2d'8a57u, 0x1e03'ac9cu, 0x9eb7'6facu, 0x45af'8e51u},
    {0x30c8'1c46u, 0xa35c'e411u, 0xe5fb'c119u, 0x1a0a'52efu},
    {0xf69f'2445u, 0xdf4f'9b17u, 0xad2b'417bu, 0xe66c'3710u},
}};
constexpr std::array<std::array<uint32_t, 4>, 4> kAesCiphertext = {{
    {0x3ad7'7bb4u, 0x0d7a'3660u, 0xa89e'caf3u, 0x2466'ef97u},
    {0xf5d3'd585u, 0x03b9'699du, 0xe785'895au, 0x96fd'baafu},
    {0x43b1'cd7fu, 0x598e'ce23u, 0x881b'00e3u, 0xed03'0688u},
    {0x7b0c'785eu, 0x27e8'ad3fu, 0x8223'2071u, 0x0472'5dd4u},
}};

struct Context {
    coro::Testbench& scheduler;
    Dut dut;
    uint32_t iterations;
    BenchResult& result;
};

uint32_t stimulus(uint32_t ordinal) {
    return ((ordinal + 1u) * 0x1f12'3bb5u) ^ 0xc001'd00du;
}

uint8_t frame_byte(uint32_t packet, uint32_t offset) {
    return static_cast<uint8_t>(stimulus(packet * 2048u + offset));
}

uint32_t frame_length(uint32_t packet) {
    return 64u + ((packet * 37u) % 1455u);
}

uint32_t crc32_byte(uint32_t current, uint8_t data) {
    uint32_t value = current ^ data;
    for (uint32_t bit = 0; bit < 8; ++bit)
        value = (value & 1u) ? ((value >> 1u) ^ 0xedb8'8320u)
                             : (value >> 1u);
    return value;
}

uint32_t xorshift32(uint32_t value) {
    value ^= value << 13u;
    value ^= value >> 17u;
    value ^= value << 5u;
    return value;
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
            "OPEN_CORE_BENCH_MISMATCH mode=cpp_dpi workload=%s label=%s "
            "actual=0x%08x expected=0x%08x\n",
            workload_name(), label, actual, expected);
    }
}

void initialize_inputs(Context& context) {
    context.dut.rst_n.set(1);
    context.dut.cpu_prog_we.set(0);
    context.dut.cpu_prog_addr.set(0);
    context.dut.cpu_prog_data.set(0);
    context.dut.aes_cs.set(0);
    context.dut.aes_we.set(0);
    context.dut.aes_address.set(0);
    context.dut.aes_write_data.set(0);
    context.dut.fcs_tdata.set(0);
    context.dut.fcs_tkeep.set(0);
    context.dut.fcs_tvalid.set(0);
    context.dut.fcs_tlast.set(0);
}

Task<void> reset(Context& context) {
    co_await FallingEdge{context.dut.clk};
    context.dut.rst_n.set(0);
    for (uint32_t cycle = 0; cycle < 4; ++cycle)
        co_await RisingEdge{context.dut.clk};
    co_await FallingEdge{context.dut.clk};
    context.dut.rst_n.set(1);
}

Task<void> program_word(Context& context, uint32_t address, uint32_t data) {
    co_await FallingEdge{context.dut.clk};
    context.dut.cpu_prog_addr.set(address);
    context.dut.cpu_prog_data.set(data);
    context.dut.cpu_prog_we.set(1);
    co_await RisingEdge{context.dut.clk};
}

Task<void> run_picorv32(Context& context) {
    co_await FallingEdge{context.dut.clk};
    context.dut.rst_n.set(0);
    for (uint32_t index = 0; index < kFirmware.size(); ++index)
        co_await program_word(context, index * 4u, kFirmware[index]);
    co_await program_word(context, 0x100u, context.iterations);
    co_await FallingEdge{context.dut.clk};
    context.dut.cpu_prog_we.set(0);
    context.dut.rst_n.set(1);

    co_await RisingEdge{context.dut.cpu_done};
    co_await Delay{1_ps};

    uint32_t expected = 0x1234'5678u;
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration)
        expected = xorshift32(expected);
    check(context, "firmware result", context.dut.cpu_result.get(), expected);
    check(context, "CPU trap", context.dut.cpu_trap.get(), 0);
    fold(context, context.dut.cpu_result.get());
    context.result.transactions = context.iterations;
}

Task<void> aes_write(Context& context, uint32_t address, uint32_t data) {
    co_await FallingEdge{context.dut.clk};
    context.dut.aes_cs.set(1);
    context.dut.aes_we.set(1);
    context.dut.aes_address.set(address);
    context.dut.aes_write_data.set(data);
    co_await RisingEdge{context.dut.clk};
}

Task<void> aes_select_read(Context& context, uint32_t address) {
    co_await FallingEdge{context.dut.clk};
    context.dut.aes_cs.set(1);
    context.dut.aes_we.set(0);
    context.dut.aes_address.set(address);
}

Task<void> aes_wait_status(Context& context, uint32_t mask) {
    bool saw_clear = false;
    co_await aes_select_read(context, 0x09u);
    while (true) {
        co_await RisingEdge{context.dut.clk};
        co_await Delay{1_ps};
        if ((context.dut.aes_read_data.get() & mask) == 0)
            saw_clear = true;
        else if (saw_clear)
            co_return;
    }
}

Task<uint32_t> aes_read(Context& context, uint32_t address) {
    co_await aes_select_read(context, address);
    co_await Delay{1_ps};
    co_return context.dut.aes_read_data.get();
}

Task<void> run_aes(Context& context) {
    co_await reset(context);
    co_await aes_write(context, 0x0au, 1u);
    for (uint32_t index = 0; index < kAesKey.size(); ++index)
        co_await aes_write(context, 0x10u + index, kAesKey[index]);
    co_await aes_write(context, 0x08u, 1u);
    co_await aes_wait_status(context, 1u);

    for (uint32_t block = 0; block < context.iterations; ++block) {
        const uint32_t vector = block & 3u;
        for (uint32_t word = 0; word < 4; ++word)
            co_await aes_write(context, 0x20u + word,
                               kAesPlaintext[vector][word]);
        co_await aes_write(context, 0x08u, 2u);
        co_await aes_wait_status(context, 2u);
        for (uint32_t word = 0; word < 4; ++word) {
            const uint32_t actual = co_await aes_read(context, 0x30u + word);
            check(context, "AES ciphertext word", actual,
                  kAesCiphertext[vector][word]);
            fold(context, actual);
        }
        ++context.result.transactions;
    }
    const uint32_t status = co_await aes_read(context, 0x09u);
    check(context, "AES ready", status & 1u, 1u);
}

Task<void> run_fcs(Context& context) {
    co_await reset(context);
    for (uint32_t packet = 0; packet < context.iterations; ++packet) {
        const uint32_t length = frame_length(packet);
        uint32_t expected = 0xffff'ffffu;
        uint32_t beat = 0;
        for (uint32_t offset = 0; offset < length; offset += 8u, ++beat) {
            const uint32_t bytes =
                (length - offset < 8u) ? length - offset : 8u;
            uint64_t data = 0;
            for (uint32_t lane = 0; lane < bytes; ++lane) {
                const uint8_t value = frame_byte(packet, offset + lane);
                data |= static_cast<uint64_t>(value) << (lane * 8u);
                expected = crc32_byte(expected, value);
            }

            if (((packet + beat) % 17u) == 0u) {
                co_await FallingEdge{context.dut.clk};
                context.dut.fcs_tvalid.set(0);
                co_await RisingEdge{context.dut.clk};
            }
            co_await FallingEdge{context.dut.clk};
            context.dut.fcs_tdata.set(data);
            context.dut.fcs_tkeep.set((1u << bytes) - 1u);
            context.dut.fcs_tlast.set(offset + bytes == length);
            context.dut.fcs_tvalid.set(1);
            co_await RisingEdge{context.dut.clk};
        }
        co_await Delay{1_ps};
        check(context, "Ethernet FCS", context.dut.fcs_result.get(),
              ~expected);
        fold(context, context.dut.fcs_result.get());
        ++context.result.transactions;
    }
    context.dut.fcs_tvalid.set(0);
    context.dut.fcs_tlast.set(0);
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
        "OPEN_CORE_BENCH_RESULT mode=cpp_dpi workload=%s iterations=%u "
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
#if OPEN_CORE_WORKLOAD == OPEN_CORE_PICORV32
    co_await run_picorv32(context);
#elif OPEN_CORE_WORKLOAD == OPEN_CORE_AES128
    co_await run_aes(context);
#elif OPEN_CORE_WORKLOAD == OPEN_CORE_FCS64
    co_await run_fcs(context);
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
    Context context{scheduler, dut, iterations, result};
    initialize_inputs(context);
    clocks.start(dut.clk, 2_ns);
    scheduler.spawn_detached(run(context));
}

}  // namespace cpptb::benchmarks::open_cores
