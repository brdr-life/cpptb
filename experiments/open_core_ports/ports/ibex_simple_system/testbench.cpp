// A cpptb port of Ibex Simple System's Verilator harness.
//
// Upstream drives this design from ibex_simple_system.cc and
// ibex_simple_system_main.cc, backed by verilator_sim_ctrl and
// verilator_memutil in vendor/lowrisc_ip. This does the same job: release
// reset, run the core until its software signals completion, and report what
// the run did.
//
// The design exposes only IO_CLK and IO_RST_N, so everything else is observed
// through generated hierarchy access rather than driven.
//
// Firmware is loaded during elaboration. ibex_simple_system takes an
// SRAMInitFile parameter reaching prim_util_memload's $readmemh, so the VMEM is
// a build parameter and no ELF reader is needed at run time. Upstream instead
// links libelf and loads through the simutil_memload DPI export.

#include <cstdint>
#include <cstdio>

#include "cpptb/cpptb.hpp"
#include "dut.hpp"

namespace cpptb::ports::ibex {
namespace {

using cpptb::Dut;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

// Upstream's SimCtrl drives this design at 10ns, so cycle counts line up with
// the baseline measurement.
constexpr auto kClockPeriod = 10_ns;

// CoreMark retires about 2.75M instructions across roughly 4.14M cycles. This
// cap turns a hang into a failure rather than a CI timeout and sits well clear
// of a healthy run.
constexpr uint64_t kCycleLimit = 20'000'000;

// ibex_pkg::PRIV_LVL_M
constexpr uint32_t kPrivLevelMachine = 3;

// Software ends the run by writing bit 0 of SIM_CTRL_ADDR, after which
// simulator_ctrl raises sim_finish and counts down to $finish. Watching that
// register lets this report its own result instead of waiting to be killed.
Task<void> coremark(Dut dut, TestContext& test) {
    dut.IO_CLK.set(0);
    // Start with reset released, as upstream's SimCtrl does, then assert it.
    // The core's flops reset on `negedge rst_ni`, so holding reset low from
    // time zero never produces that edge and the core boots in user mode,
    // where every machine-mode CSR access traps.
    dut.IO_RST_N.set(1);
    test.start_clock(dut.IO_CLK, kClockPeriod);

    co_await clock_cycles(dut.IO_CLK, 2);
    dut.IO_RST_N.set(0);
    co_await clock_cycles(dut.IO_CLK, 4);
    dut.IO_RST_N.set(1);

    // The run ends when simulator_ctrl issues $finish, which stops the
    // simulator from inside the design. Probing sim_finish through generated
    // hierarchy access would be tidier, but pulls the whole hierarchy catalog
    // into type registration, and ibex_multdiv_fast.sv declares two different
    // mult_fsm_e enums that both want the C++ name MultFsmE. See README.md.
    // The core must come out of reset in machine mode. Booting in user mode
    // traps every machine-mode CSR access, which shows up as an illegal
    // instruction in the firmware rather than as anything obviously wrong here.
    test.expect_eq("core boots in machine mode",
                   dut.u_top.u_ibex_top.u_ibex_core.cs_registers_i
                       .priv_lvl_q.get().signal_value(),
                   kPrivLevelMachine);

    // Software ends the run by writing bit 0 of the simulator_ctrl register,
    // which raises sim_finish and then issues $finish from RTL. Watching that
    // register lets the test finish on its own terms and report a result,
    // rather than being cut off mid-await when the simulator stops.
    uint64_t cycles = 0;
    bool finished = false;
    while (cycles < kCycleLimit) {
        co_await RisingEdge{dut.IO_CLK};
        ++cycles;
        if (dut.u_simulator_ctrl.sim_finish.get() != 0) {
            finished = true;
            break;
        }
    }

    test.expect("firmware signalled completion", finished);
    test.expect("completed within the cycle limit", cycles < kCycleLimit);
    std::printf("cpptb: ibex ran %llu cycles\n",
                static_cast<unsigned long long>(cycles));
}

CPPTB_REGISTER_TEST(coremark);

}  // namespace
}  // namespace cpptb::ports::ibex
