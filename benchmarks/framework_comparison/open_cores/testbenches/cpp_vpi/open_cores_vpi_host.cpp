#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>

#include "Vopen_cores_benchmark_top.h"
#include "cpptb/coro_runtime.hpp"
#include "verilated.h"
#include "verilated_vpi.h"
#include "vpi_user.h"

#define OPEN_CORE_PICORV32 0
#define OPEN_CORE_AES128 1
#define OPEN_CORE_FCS64 2
#ifndef OPEN_CORE_WORKLOAD
#define OPEN_CORE_WORKLOAD OPEN_CORE_PICORV32
#endif

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
constexpr uint32_t kSignalCpuDone = 5;
constexpr uint64_t kHalfPeriodTicks = 1'000;
constexpr uint64_t kClockPeriodFs = 2'000'000;
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

struct Packed64Signal {
    vpiHandle handle = nullptr;

    void set(uint64_t data) const {
        s_vpi_vecval words[2] = {
            {static_cast<PLI_UINT32>(data), 0},
            {static_cast<PLI_UINT32>(data >> 32u), 0},
        };
        s_vpi_value value{};
        value.format = vpiVectorVal;
        value.value.vector = words;
        vpi_put_value(handle, &value, nullptr, vpiNoDelay);
    }
};

struct Dut {
    Signal clk;
    Signal rst_n;
    Signal cpu_prog_we;
    Signal cpu_prog_addr;
    Signal cpu_prog_data;
    Signal cpu_done;
    Signal cpu_trap;
    Signal cpu_result;
    Signal cpu_instruction_count;
    Signal aes_cs;
    Signal aes_we;
    Signal aes_address;
    Signal aes_write_data;
    Signal aes_read_data;
    Packed64Signal fcs_tdata;
    Signal fcs_tkeep;
    Signal fcs_tvalid;
    Signal fcs_tready;
    Signal fcs_tlast;
    Signal fcs_result;
    Signal fcs_valid;
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
    uint32_t iterations;
    BenchResult& result;
};

uint64_t main_time_ticks = 0;

constexpr const char* workload_name() {
#if OPEN_CORE_WORKLOAD == OPEN_CORE_PICORV32
    return "picorv32_firmware";
#elif OPEN_CORE_WORKLOAD == OPEN_CORE_AES128
    return "secworks_aes128";
#else
    return "ethernet_fcs64";
#endif
}

uint32_t env_u32(const char* name, uint32_t default_value) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') return default_value;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (!end || *end != '\0' || parsed == 0 ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        std::fprintf(stderr, "open-core VPI: invalid %s='%s'\n", name, value);
        std::exit(2);
    }
    return static_cast<uint32_t>(parsed);
}

vpiHandle require_handle(const char* name) {
    char path[192];
    std::snprintf(path, sizeof(path), "TOP.open_cores_benchmark_top.%s", name);
    auto* handle = vpi_handle_by_name(reinterpret_cast<PLI_BYTE8*>(path), nullptr);
    if (handle) return handle;
    std::fprintf(stderr, "open-core VPI: missing handle for %s\n", path);
    std::exit(1);
}

Signal bind_signal(const char* name, uint32_t id) {
    return make_vpi_signal(require_handle(name), id, name);
}

Dut bind_dut() {
    uint32_t id = 0;
    Dut dut{
        bind_signal("clk", id++),
        bind_signal("rst_n", id++),
        bind_signal("cpu_prog_we", id++),
        bind_signal("cpu_prog_addr", id++),
        bind_signal("cpu_prog_data", id++),
        bind_signal("cpu_done", id++),
        bind_signal("cpu_trap", id++),
        bind_signal("cpu_result", id++),
        bind_signal("cpu_instruction_count", id++),
        bind_signal("aes_cs", id++),
        bind_signal("aes_we", id++),
        bind_signal("aes_address", id++),
        bind_signal("aes_write_data", id++),
        bind_signal("aes_read_data", id++),
        Packed64Signal{require_handle("fcs_tdata")},
        bind_signal("fcs_tkeep", id++),
        bind_signal("fcs_tvalid", id++),
        bind_signal("fcs_tready", id++),
        bind_signal("fcs_tlast", id++),
        bind_signal("fcs_result", id++),
        bind_signal("fcs_valid", id++),
    };
    return dut;
}

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

void check(Context& context, const char* label, uint32_t actual,
           uint32_t expected) {
    ++context.result.checks;
    if (actual == expected) return;
    ++context.result.failures;
    if (context.result.failures <= 8) {
        std::printf(
            "OPEN_CORE_BENCH_MISMATCH mode=cpp_vpi workload=%s label=%s "
            "actual=0x%08x expected=0x%08x\n",
            workload_name(), label, actual, expected);
    }
}

void fold(Context& context, uint32_t value) {
    context.result.checksum =
        (context.result.checksum ^ value) * 0x0100'0193u;
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

Task<void> run(Context context) {
    initialize_inputs(context);
#if OPEN_CORE_WORKLOAD == OPEN_CORE_PICORV32
    co_await run_picorv32(context);
#elif OPEN_CORE_WORKLOAD == OPEN_CORE_AES128
    co_await run_aes(context);
#else
    co_await run_fcs(context);
#endif
}

void settle(Vopen_cores_benchmark_top* top) {
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
                 Vopen_cores_benchmark_top* top, uint64_t ticks) {
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
    const std::unique_ptr<Vopen_cores_benchmark_top> top{
        new Vopen_cores_benchmark_top{verilated_context.get()}};
    Testbench scheduler{1_ps};
    settle(top.get());
    Dut dut = bind_dut();
    const uint32_t iterations = env_u32("OPEN_CORE_BENCH_ITERS", 100);
    BenchResult result;

    update_time(scheduler, *verilated_context, top.get(), 0);
    dut.clk.set(0);
    settle(top.get());
    const auto start = std::chrono::steady_clock::now();
    scheduler.spawn_detached(run(Context{scheduler, dut, iterations, result}));
    settle(top.get());

    bool clock_high = false;
    bool cpu_done_high = dut.cpu_done.get() != 0;
    uint64_t next_clock_edge_ticks = kHalfPeriodTicks;
    const uint64_t max_time_ticks =
        static_cast<uint64_t>(iterations) * 1'024'000u + 40'000'000u;
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
            const bool next_cpu_done_high = dut.cpu_done.get() != 0;
            if (next_cpu_done_high != cpu_done_high) {
                scheduler.notify_edge(
                    kSignalCpuDone,
                    next_cpu_done_high ? EdgeKind::Rising : EdgeKind::Falling);
                settle(top.get());
                cpu_done_high = next_cpu_done_high;
            }
            next_clock_edge_ticks += kHalfPeriodTicks;
        }
    }

    const auto finish = std::chrono::steady_clock::now();
    const auto wall_us =
        std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count();
    if (!scheduler.done()) {
        std::fprintf(stderr, "open-core VPI: timed out at %llu ps for %s\n",
                     static_cast<unsigned long long>(main_time_ticks),
                     workload_name());
        return 1;
    }
    top->final();
    const uint64_t sim_cycles =
        (scheduler.now().in_femtoseconds() + 1'000'000u) / kClockPeriodFs;
    std::printf(
        "OPEN_CORE_BENCH_RESULT mode=cpp_vpi workload=%s iterations=%u "
        "transactions=%llu checks=%llu sim_cycles=%llu checksum=%u "
        "failures=%u wall_ms=%.3f\n",
        workload_name(), iterations,
        static_cast<unsigned long long>(result.transactions),
        static_cast<unsigned long long>(result.checks),
        static_cast<unsigned long long>(sim_cycles), result.checksum,
        result.failures, static_cast<double>(wall_us) / 1000.0);
    return result.failures == 0 ? 0 : 1;
}
