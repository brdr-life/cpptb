#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "Vvpi_counter.h"
#include "experiments/cpp_vpi/counter/runtime.hpp"
#include "experiments/cpp_vpi/counter/counter_test.hpp"
#include "verilated.h"
#include "verilated_vpi.h"
#include "vpi_user.h"

namespace {

using cpptb::CounterDut;
using cpptb::Signal;
using cpptb::WaitRequest;

uint64_t main_time = 0;
CounterDut dut;

struct ProcessState {
    uint32_t id;
    WaitRequest wait;
    bool done;
};

struct SignalWatch {
    uint32_t signal_id = UINT32_MAX;
    vpiHandle callback = nullptr;
    s_vpi_time time{};
    s_vpi_value value{};
    s_cb_data callback_data{};
};

std::vector<ProcessState> processes;
std::array<SignalWatch, cpptb::kSignalCountTotal> watches;

vpiHandle require_handle(const char* path) {
    auto* handle = vpi_handle_by_name(const_cast<PLI_BYTE8*>(path), nullptr);
    if (!handle) {
        std::fprintf(stderr, "cpptb: missing VPI handle for %s\n", path);
        std::exit(1);
    }
    return handle;
}

Signal signal_by_id(uint32_t signal_id) {
    switch (signal_id) {
        case cpptb::kSignalClk:
            return dut.clk;
        case cpptb::kSignalRst:
            return dut.rst;
        case cpptb::kSignalEn:
            return dut.en;
        case cpptb::kSignalCount:
            return dut.count;
        default:
            std::fprintf(stderr, "cpptb: unknown signal id %u\n", signal_id);
            std::exit(1);
    }
}

void register_rising_edge_callback(uint32_t signal_id);

void apply_wait_request(ProcessState& process, WaitRequest request) {
    process.wait = request;

    switch (process.wait.kind) {
        case cpptb::kRequestDone:
            process.done = true;
            return;
        case cpptb::kRequestRisingEdge:
            process.done = false;
            register_rising_edge_callback(process.wait.signal_id);
            return;
        default:
            std::fprintf(stderr, "cpptb: process %u returned unknown wait kind %u\n",
                         process.id, process.wait.kind);
            std::exit(1);
    }
}

PLI_INT32 on_value_change(p_cb_data callback_data) {
    auto* watch = reinterpret_cast<SignalWatch*>(callback_data->user_data);
    if (!watch) return 0;

    const auto signal = signal_by_id(watch->signal_id);
    if (signal.get() != 1) return 0;

    for (auto& process : processes) {
        if (process.done) continue;
        if (process.wait.kind != cpptb::kRequestRisingEdge) continue;
        if (process.wait.signal_id != watch->signal_id) continue;

        apply_wait_request(process, cpptb::counter_test::process_resume(
                                        dut, process.id, main_time,
                                        process.wait.next_phase));
    }

    return 0;
}

void register_rising_edge_callback(uint32_t signal_id) {
    if (signal_id >= watches.size()) {
        std::fprintf(stderr, "cpptb: cannot watch unknown signal id %u\n", signal_id);
        std::exit(1);
    }

    auto& watch = watches[signal_id];
    if (watch.callback) return;

    watch.signal_id = signal_id;
    watch.time.type = vpiSimTime;
    watch.value.format = vpiIntVal;
    watch.callback_data.reason = cbValueChange;
    watch.callback_data.cb_rtn = on_value_change;
    watch.callback_data.obj = signal_by_id(signal_id).handle;
    watch.callback_data.time = &watch.time;
    watch.callback_data.value = &watch.value;
    watch.callback_data.user_data = reinterpret_cast<PLI_BYTE8*>(&watch);
    watch.callback = vpi_register_cb(&watch.callback_data);
    if (!watch.callback) {
        std::fprintf(stderr, "cpptb: failed to register VPI callback for signal %u\n",
                     signal_id);
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
    cpptb::counter_test::setup(dut);

    const auto count = cpptb::counter_test::process_count();
    processes.clear();
    processes.reserve(count);
    for (uint32_t process_id = 0; process_id < count; ++process_id) {
        processes.push_back(ProcessState{
            process_id, WaitRequest{cpptb::kRequestDone, 0, 0}, false});
        apply_wait_request(processes.back(),
                           cpptb::counter_test::process_start(dut, process_id));
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
    dut.clk.set(value);
    settle(top);
}

void tick(Vvpi_counter* top) {
    set_clk(top, 0);
    ++main_time;
    set_clk(top, 1);
    ++main_time;
    set_clk(top, 0);
}

}  // namespace

double sc_time_stamp() { return static_cast<double>(main_time); }

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    const std::unique_ptr<VerilatedContext> context{new VerilatedContext};
    const std::unique_ptr<Vvpi_counter> top{new Vvpi_counter{context.get()}};

    settle(top.get());
    dut = CounterDut{
        Signal{require_handle("TOP.vpi_counter.clk"), cpptb::kSignalClk},
        Signal{require_handle("TOP.vpi_counter.rst"), cpptb::kSignalRst},
        Signal{require_handle("TOP.vpi_counter.en"), cpptb::kSignalEn},
        Signal{require_handle("TOP.vpi_counter.count"), cpptb::kSignalCount},
    };
    start_processes();

    constexpr int kMaxCycles = 64;
    for (int cycle = 0; !all_processes_done() && cycle < kMaxCycles; ++cycle) {
        tick(top.get());
    }

    if (!all_processes_done()) {
        std::fprintf(stderr, "cpptb: timed out after %d cycles\n", kMaxCycles);
        return 1;
    }

    top->final();
    return cpptb::counter_test::done(dut);
}
