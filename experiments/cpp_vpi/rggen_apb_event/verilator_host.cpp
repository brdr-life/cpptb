#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "Vvpi_apb_event_unit.h"
#include "cpptb/coro_runtime.hpp"
#include "experiments/cpp_vpi/rggen_apb_event/apb_event_dut.hpp"
#include "experiments/cpp_vpi/rggen_apb_event/tests/apb_event_test.hpp"
#include "verilated.h"
#include "verilated_vpi.h"
#include "vpi_user.h"

namespace {

using cpptb::coro::EdgeKind;
using cpptb::coro::Signal;
using cpptb::coro::Testbench;
using cpptb::coro::make_vpi_signal;
using cpptb::rggen_apb_event::ApbEventDut;

uint64_t main_time = 0;
Testbench* tb = nullptr;
ApbEventDut dut;

struct SignalWatch {
    uint32_t signal_id = UINT32_MAX;
    vpiHandle callback = nullptr;
    s_vpi_time time{};
    s_vpi_value value{};
    s_cb_data callback_data{};
};

std::array<SignalWatch, cpptb::rggen_apb_event::kSignalCount> watches;

vpiHandle require_handle(const char* path) {
    auto* handle = vpi_handle_by_name(const_cast<PLI_BYTE8*>(path), nullptr);
    if (!handle) {
        std::fprintf(stderr, "cpptb: missing VPI handle for %s\n", path);
        std::exit(1);
    }
    return handle;
}

Signal signal_by_id(uint32_t signal_id) {
    using namespace cpptb::rggen_apb_event;

    switch (signal_id) {
        case kSignalClkI:
            return dut.clk_i;
        case kSignalHclk:
            return dut.HCLK;
        case kSignalHresetn:
            return dut.HRESETn;
        case kSignalPaddr:
            return dut.PADDR;
        case kSignalPwdata:
            return dut.PWDATA;
        case kSignalPwrite:
            return dut.PWRITE;
        case kSignalPsel:
            return dut.PSEL;
        case kSignalPenable:
            return dut.PENABLE;
        case kSignalPrdata:
            return dut.PRDATA;
        case kSignalPready:
            return dut.PREADY;
        case kSignalPslverr:
            return dut.PSLVERR;
        case kSignalIrqI:
            return dut.irq_i;
        case kSignalEventI:
            return dut.event_i;
        case kSignalIrqO:
            return dut.irq_o;
        case kSignalFetchEnableI:
            return dut.fetch_enable_i;
        case kSignalFetchEnableO:
            return dut.fetch_enable_o;
        case kSignalClkGateCoreO:
            return dut.clk_gate_core_o;
        case kSignalCoreBusyI:
            return dut.core_busy_i;
        default:
            std::fprintf(stderr, "cpptb: unknown signal id %u\n", signal_id);
            std::exit(1);
    }
}

PLI_INT32 on_value_change(p_cb_data callback_data) {
    auto* watch = reinterpret_cast<SignalWatch*>(callback_data->user_data);
    if (!watch || !tb) return 0;

    const auto value = signal_by_id(watch->signal_id).get();
    tb->notify_edge(watch->signal_id,
                    value ? EdgeKind::Rising : EdgeKind::Falling);
    return 0;
}

void register_edge_callback(uint32_t signal_id) {
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

void settle(Vvpi_apb_event_unit* top) {
    top->eval();
    VerilatedVpi::callValueCbs();

    for (int i = 0; i < 8 && VerilatedVpi::evalNeeded(); ++i) {
        top->eval();
        VerilatedVpi::clearEvalNeeded();
        VerilatedVpi::callValueCbs();
    }
}

void update_time(uint64_t time) {
    main_time = time;
    if (tb) tb->set_time(main_time);
}

void set_clocks(Vvpi_apb_event_unit* top, uint32_t value) {
    dut.HCLK.set(value);
    dut.clk_i.set(value);
    settle(top);
}

void tick(Vvpi_apb_event_unit* top) {
    set_clocks(top, 0);
    update_time(main_time + 1);
    set_clocks(top, 1);
    update_time(main_time + 1);
    set_clocks(top, 0);
}

Signal make_signal(const char* path, uint32_t id, const char* name) {
    return make_vpi_signal(require_handle(path), id, name);
}

ApbEventDut bind_dut() {
    using namespace cpptb::rggen_apb_event;

    return ApbEventDut{
        make_signal("TOP.vpi_apb_event_unit.clk_i", kSignalClkI, "clk_i"),
        make_signal("TOP.vpi_apb_event_unit.HCLK", kSignalHclk, "HCLK"),
        make_signal("TOP.vpi_apb_event_unit.HRESETn", kSignalHresetn, "HRESETn"),
        make_signal("TOP.vpi_apb_event_unit.PADDR", kSignalPaddr, "PADDR"),
        make_signal("TOP.vpi_apb_event_unit.PWDATA", kSignalPwdata, "PWDATA"),
        make_signal("TOP.vpi_apb_event_unit.PWRITE", kSignalPwrite, "PWRITE"),
        make_signal("TOP.vpi_apb_event_unit.PSEL", kSignalPsel, "PSEL"),
        make_signal("TOP.vpi_apb_event_unit.PENABLE", kSignalPenable, "PENABLE"),
        make_signal("TOP.vpi_apb_event_unit.PRDATA", kSignalPrdata, "PRDATA"),
        make_signal("TOP.vpi_apb_event_unit.PREADY", kSignalPready, "PREADY"),
        make_signal("TOP.vpi_apb_event_unit.PSLVERR", kSignalPslverr, "PSLVERR"),
        make_signal("TOP.vpi_apb_event_unit.irq_i", kSignalIrqI, "irq_i"),
        make_signal("TOP.vpi_apb_event_unit.event_i", kSignalEventI, "event_i"),
        make_signal("TOP.vpi_apb_event_unit.irq_o", kSignalIrqO, "irq_o"),
        make_signal("TOP.vpi_apb_event_unit.fetch_enable_i", kSignalFetchEnableI,
                    "fetch_enable_i"),
        make_signal("TOP.vpi_apb_event_unit.fetch_enable_o", kSignalFetchEnableO,
                    "fetch_enable_o"),
        make_signal("TOP.vpi_apb_event_unit.clk_gate_core_o", kSignalClkGateCoreO,
                    "clk_gate_core_o"),
        make_signal("TOP.vpi_apb_event_unit.core_busy_i", kSignalCoreBusyI,
                    "core_busy_i"),
    };
}

}  // namespace

double sc_time_stamp() { return static_cast<double>(main_time); }

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    const std::unique_ptr<VerilatedContext> context{new VerilatedContext};
    const std::unique_ptr<Vvpi_apb_event_unit> top{
        new Vvpi_apb_event_unit{context.get()}};

    Testbench testbench;
    tb = &testbench;

    settle(top.get());
    dut = bind_dut();
    register_edge_callback(cpptb::rggen_apb_event::kSignalHclk);
    register_edge_callback(cpptb::rggen_apb_event::kSignalClkI);

    update_time(0);
    set_clocks(top.get(), 0);
    cpptb::rggen_apb_event::tests::register_tests(testbench, dut);
    settle(top.get());

    constexpr int kMaxCycles = 220;
    for (int cycle = 0; !testbench.done() && cycle < kMaxCycles; ++cycle) {
        tick(top.get());
    }

    if (!testbench.done()) {
        std::fprintf(stderr, "cpptb: timed out after %d cycles\n", kMaxCycles);
        return 1;
    }

    top->final();
    return cpptb::rggen_apb_event::tests::done(testbench, dut);
}
