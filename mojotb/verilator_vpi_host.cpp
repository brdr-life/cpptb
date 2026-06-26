#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <memory>
#include <vector>

#include "Vvpi_counter.h"
#include "verilated.h"
#include "verilated_vpi.h"
#include "vpi_user.h"

namespace {

uint64_t main_time = 0;
vpiHandle clk_handle = nullptr;
vpiHandle rst_handle = nullptr;
vpiHandle en_handle = nullptr;
vpiHandle count_handle = nullptr;

using SignalHandle = void*;
using GetU32 = uint32_t (*)(SignalHandle);
using PutU32 = void (*)(SignalHandle, uint32_t);
using ResolveSignal = SignalHandle (*)(uint32_t);
using ProcessCount = uint32_t (*)();
using OnSetup = void (*)(GetU32, PutU32, ResolveSignal);
using OnProcessStart = uint64_t (*)(GetU32, PutU32, ResolveSignal, uint32_t);
using OnProcessResume = uint64_t (*)(GetU32, PutU32, ResolveSignal, uint32_t, uint64_t, uint32_t);
using OnDone = int32_t (*)(GetU32, PutU32, ResolveSignal);

constexpr uint32_t kRequestDone = 0;
constexpr uint32_t kRequestRisingEdge = 1;
constexpr uint32_t kNoSignal = UINT32_MAX;

struct WaitRequest {
    uint32_t kind;
    uint32_t signal_id;
    uint32_t next_phase;
};

struct ProcessState {
    uint32_t id;
    WaitRequest wait;
    bool done;
};

struct SignalWatch {
    uint32_t signal_id = kNoSignal;
    vpiHandle callback = nullptr;
    s_vpi_time time{};
    s_vpi_value value{};
    s_cb_data callback_data{};
};

ProcessCount mojo_process_count = nullptr;
OnSetup mojo_on_setup = nullptr;
OnProcessStart mojo_on_process_start = nullptr;
OnProcessResume mojo_on_process_resume = nullptr;
OnDone mojo_on_done = nullptr;
std::vector<ProcessState> processes;
SignalWatch watches[4];

void apply_wait_request(ProcessState& process, uint64_t encoded);

uint32_t get_u32(vpiHandle handle) {
    s_vpi_value value;
    value.format = vpiIntVal;
    vpi_get_value(handle, &value);
    return static_cast<uint32_t>(value.value.integer);
}

void put_u32(vpiHandle handle, uint32_t data) {
    s_vpi_value value;
    value.format = vpiIntVal;
    value.value.integer = static_cast<PLI_INT32>(data);
    vpi_put_value(handle, &value, nullptr, vpiNoDelay);
}

uint32_t api_get_u32(SignalHandle handle) {
    return get_u32(static_cast<vpiHandle>(handle));
}

void api_put_u32(SignalHandle handle, uint32_t data) {
    put_u32(static_cast<vpiHandle>(handle), data);
}

SignalHandle api_resolve_signal(uint32_t signal_id) {
    switch (signal_id) {
        case 0:
            return clk_handle;
        case 1:
            return rst_handle;
        case 2:
            return en_handle;
        case 3:
            return count_handle;
        default:
            std::fprintf(stderr, "mojotb: unknown signal id %u\n", signal_id);
            std::exit(1);
    }
}

vpiHandle signal_handle(uint32_t signal_id) {
    return static_cast<vpiHandle>(api_resolve_signal(signal_id));
}

WaitRequest decode_wait_request(uint64_t encoded) {
    return WaitRequest{
        static_cast<uint32_t>((encoded >> 56) & 0xff),
        static_cast<uint32_t>((encoded >> 32) & 0xffffff),
        static_cast<uint32_t>(encoded & 0xffffffff),
    };
}

vpiHandle require_handle(const char* path) {
    auto* handle = vpi_handle_by_name(const_cast<PLI_BYTE8*>(path), nullptr);
    if (!handle) {
        std::fprintf(stderr, "mojotb: missing VPI handle for %s\n", path);
        std::exit(1);
    }
    return handle;
}

void* require_symbol(void* library, const char* name) {
    auto* symbol = dlsym(library, name);
    if (!symbol) {
        std::fprintf(stderr, "mojotb: missing Mojo symbol %s: %s\n", name, dlerror());
        std::exit(1);
    }
    return symbol;
}

void load_mojo_test(const char* path) {
    auto* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::fprintf(stderr, "mojotb: dlopen failed for %s: %s\n", path, dlerror());
        std::exit(1);
    }

    mojo_process_count
        = reinterpret_cast<ProcessCount>(require_symbol(library, "mojotb_process_count"));
    mojo_on_setup = reinterpret_cast<OnSetup>(require_symbol(library, "mojotb_on_setup"));
    mojo_on_process_start = reinterpret_cast<OnProcessStart>(
        require_symbol(library, "mojotb_on_process_start"));
    mojo_on_process_resume = reinterpret_cast<OnProcessResume>(
        require_symbol(library, "mojotb_on_process_resume"));
    mojo_on_done = reinterpret_cast<OnDone>(require_symbol(library, "mojotb_on_done"));
}

PLI_INT32 on_value_change(p_cb_data callback_data) {
    auto* watch = reinterpret_cast<SignalWatch*>(callback_data->user_data);
    if (!watch) return 0;

    auto* signal = signal_handle(watch->signal_id);
    const auto value = get_u32(signal);
    if (value != 1) return 0;

    for (auto& process : processes) {
        if (process.done) continue;
        if (process.wait.kind != kRequestRisingEdge) continue;
        if (process.wait.signal_id != watch->signal_id) continue;

        apply_wait_request(
            process, mojo_on_process_resume(api_get_u32, api_put_u32, api_resolve_signal,
                                            process.id, main_time, process.wait.next_phase));
    }

    return 0;
}

void register_rising_edge_callback(uint32_t signal_id) {
    if (signal_id >= 4) {
        std::fprintf(stderr, "mojotb: cannot watch unknown signal id %u\n", signal_id);
        std::exit(1);
    }

    auto& watch = watches[signal_id];
    if (watch.callback) return;

    watch.signal_id = signal_id;
    watch.time.type = vpiSimTime;
    watch.value.format = vpiIntVal;
    watch.callback_data.reason = cbValueChange;
    watch.callback_data.cb_rtn = on_value_change;
    watch.callback_data.obj = signal_handle(signal_id);
    watch.callback_data.time = &watch.time;
    watch.callback_data.value = &watch.value;
    watch.callback_data.user_data = reinterpret_cast<PLI_BYTE8*>(&watch);
    watch.callback = vpi_register_cb(&watch.callback_data);
    if (!watch.callback) {
        std::fprintf(stderr, "mojotb: failed to register VPI callback for signal %u\n",
                     signal_id);
        std::exit(1);
    }
}

void apply_wait_request(ProcessState& process, uint64_t encoded) {
    process.wait = decode_wait_request(encoded);
    switch (process.wait.kind) {
        case kRequestDone:
            process.done = true;
            return;
        case kRequestRisingEdge:
            process.done = false;
            register_rising_edge_callback(process.wait.signal_id);
            return;
        default:
            std::fprintf(stderr, "mojotb: process %u returned unknown wait request kind %u\n",
                         process.id, process.wait.kind);
            std::exit(1);
    }
}

bool all_processes_done() {
    for (const auto& process : processes) {
        if (!process.done) return false;
    }
    return true;
}

void start_processes() {
    mojo_on_setup(api_get_u32, api_put_u32, api_resolve_signal);

    const auto count = mojo_process_count();
    processes.clear();
    processes.reserve(count);
    for (uint32_t process_id = 0; process_id < count; ++process_id) {
        processes.push_back(ProcessState{process_id, WaitRequest{kRequestDone, 0, 0}, false});
        apply_wait_request(processes.back(),
                           mojo_on_process_start(api_get_u32, api_put_u32,
                                                 api_resolve_signal, process_id));
    }
}

void settle(Vvpi_counter* top) {
    top->eval();
    VerilatedVpi::callValueCbs();
    if (VerilatedVpi::evalNeeded()) {
        top->eval();
        VerilatedVpi::clearEvalNeeded();
    }
}

void set_clk(Vvpi_counter* top, uint32_t value) {
    put_u32(clk_handle, value);
    settle(top);
}

void tick(Vvpi_counter* top) {
    // Prototype clock driver: Mojo registers for RisingEdge(clk), this adapter toggles clk.
    set_clk(top, 0);
    ++main_time;
    set_clk(top, 1);
    ++main_time;
    set_clk(top, 0);
}

}  // namespace

double sc_time_stamp() { return static_cast<double>(main_time); }

int main(int argc, char** argv) {
    const char* mojo_test_path = argc > 1 ? argv[1] : "build/mojotb/libcounter_test.dylib";
    load_mojo_test(mojo_test_path);

    Verilated::commandArgs(argc, argv);
    const std::unique_ptr<VerilatedContext> context{new VerilatedContext};
    const std::unique_ptr<Vvpi_counter> top{new Vvpi_counter{context.get()}};

    settle(top.get());
    clk_handle = require_handle("TOP.vpi_counter.clk");
    rst_handle = require_handle("TOP.vpi_counter.rst");
    en_handle = require_handle("TOP.vpi_counter.en");
    count_handle = require_handle("TOP.vpi_counter.count");
    start_processes();

    constexpr int kMaxCycles = 64;
    for (int cycle = 0; !all_processes_done() && cycle < kMaxCycles; ++cycle) {
        tick(top.get());
    }

    if (!all_processes_done()) {
        std::fprintf(stderr, "mojotb: timed out after %d cycles\n", kMaxCycles);
        return 1;
    }

    top->final();
    return mojo_on_done(api_get_u32, api_put_u32, api_resolve_signal);
}
