#pragma once

#include "cpptb/coro_runtime.hpp"

namespace cpptb::rggen_apb_event {

constexpr uint32_t kSignalClkI = 0;
constexpr uint32_t kSignalHclk = 1;
constexpr uint32_t kSignalHresetn = 2;
constexpr uint32_t kSignalPaddr = 3;
constexpr uint32_t kSignalPwdata = 4;
constexpr uint32_t kSignalPwrite = 5;
constexpr uint32_t kSignalPsel = 6;
constexpr uint32_t kSignalPenable = 7;
constexpr uint32_t kSignalPrdata = 8;
constexpr uint32_t kSignalPready = 9;
constexpr uint32_t kSignalPslverr = 10;
constexpr uint32_t kSignalIrqI = 11;
constexpr uint32_t kSignalEventI = 12;
constexpr uint32_t kSignalIrqO = 13;
constexpr uint32_t kSignalFetchEnableI = 14;
constexpr uint32_t kSignalFetchEnableO = 15;
constexpr uint32_t kSignalClkGateCoreO = 16;
constexpr uint32_t kSignalCoreBusyI = 17;
constexpr uint32_t kSignalCount = 18;

struct ApbEventDut {
    coro::Signal clk_i;
    coro::Signal HCLK;
    coro::Signal HRESETn;
    coro::Signal PADDR;
    coro::Signal PWDATA;
    coro::Signal PWRITE;
    coro::Signal PSEL;
    coro::Signal PENABLE;
    coro::Signal PRDATA;
    coro::Signal PREADY;
    coro::Signal PSLVERR;
    coro::Signal irq_i;
    coro::Signal event_i;
    coro::Signal irq_o;
    coro::Signal fetch_enable_i;
    coro::Signal fetch_enable_o;
    coro::Signal clk_gate_core_o;
    coro::Signal core_busy_i;
};

inline void drive_apb_idle(ApbEventDut dut) {
    dut.PADDR.set(0);
    dut.PWDATA.set(0);
    dut.PWRITE.set(0);
    dut.PSEL.set(0);
    dut.PENABLE.set(0);
}

}  // namespace cpptb::rggen_apb_event
