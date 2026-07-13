#pragma once

#include <cstdint>

#include "vpi_user.h"

namespace cpptb {

constexpr uint32_t kRequestDone = 0;
constexpr uint32_t kRequestRisingEdge = 1;

constexpr uint32_t kSignalClk = 0;
constexpr uint32_t kSignalRst = 1;
constexpr uint32_t kSignalEn = 2;
constexpr uint32_t kSignalCount = 3;
constexpr uint32_t kSignalCountTotal = 4;

struct Signal {
    vpiHandle handle = nullptr;
    uint32_t id = 0;

    uint32_t get() const {
        s_vpi_value value;
        value.format = vpiIntVal;
        vpi_get_value(handle, &value);
        return static_cast<uint32_t>(value.value.integer);
    }

    void set(uint32_t data) const {
        s_vpi_value value;
        value.format = vpiIntVal;
        value.value.integer = static_cast<PLI_INT32>(data);
        vpi_put_value(handle, &value, nullptr, vpiNoDelay);
    }
};

struct RisingEdge {
    Signal signal;
};

struct WaitRequest {
    uint32_t kind = kRequestDone;
    uint32_t signal_id = 0;
    uint32_t next_phase = 0;

    static WaitRequest done(uint32_t phase) { return {kRequestDone, 0, phase}; }

    static WaitRequest rising_edge(Signal signal, uint32_t next_phase) {
        return {kRequestRisingEdge, signal.id, next_phase};
    }
};

struct Scheduler {
    uint32_t current_phase = 0;

    bool at(uint32_t phase) const { return current_phase == phase; }

    WaitRequest wait(RisingEdge trigger, uint32_t next_phase) const {
        return WaitRequest::rising_edge(trigger.signal, next_phase);
    }

    WaitRequest finish() const { return WaitRequest::done(current_phase); }
};

struct CounterDut {
    Signal clk;
    Signal rst;
    Signal en;
    Signal count;
};

}  // namespace cpptb
