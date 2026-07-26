// A cpptb port of Ibex's CS registers testbench, dv/cs_registers.
//
// Every other port here runs a program and checks what it produced. This one
// does not run a program at all: it drives random CSR transactions at a
// submodule and scores each one against a C++ model of what the registers
// should have done. Constrained stimulus and a scoreboard, rather than
// software.
//
// Upstream's version is layered the way a UVM testbench is, in C++: an
// environment owning a model, a register driver, a reset driver and a
// simulation controller, with each layer reaching the design through its own
// DPI shim -- env_dpi, reg_dpi, rst_dpi -- called from `always_ff` blocks in
// tb_cs_registers.sv. The layering exists because C++ cannot otherwise see
// when a clock edge happened or drive a port when it does.
//
// cpptb can, so the port keeps the parts that decide what a correct answer is
// and drops the parts that exist to cross the language boundary:
//
//   kept       model/base_register, model/register_model, register_transaction,
//              env/simctrl, env/register_types      -- the reference and the stimulus
//   replaced   register_driver, reset_driver, register_environment,
//              env_dpi, reg_dpi, rst_dpi, tb_cs_registers.cc/sv
//
// The behaviour is upstream's: a transaction every 1 to 20 cycles from the same
// uniform distribution, seeded the same way, 10,000 of them, each captured on
// the clock edge and handed to the model.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>

#include "cpptb/cpptb.hpp"
#include "dut.hpp"

// Upstream's, reused unmodified.
#include "register_model.h"
#include "register_transaction.h"
#include "register_types.h"
#include "simctrl.h"

namespace cpptb::ports::csr {
namespace {

using cpptb::Dut;
using coro::FallingEdge;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

constexpr auto kClockPeriod = 10_ns;

// RegisterDriver::OnClock stops after this many, and the delay between
// transactions is uniform on [1, 20]. Both copied from upstream rather than
// chosen here, so the two testbenches exercise the design the same way.
constexpr int kTransactions = 10000;
constexpr int kDelayMin = 1;
constexpr int kDelayMax = 20;

// The parameters the design is elaborated with have to reach the model too, or
// it disagrees about which counters and PMP regions exist. These must match
// cpptb.toml; a mismatch shows up as the model expecting a register the design
// does not implement, which reads as a design bug and is not one.
constexpr uint32_t kPMPEnable = 1;
constexpr uint32_t kPMPGranularity = 0;
constexpr uint32_t kPMPNumRegions = 4;
constexpr uint32_t kMHPMCounterNum = 8;
constexpr uint32_t kMHPMCounterWidth = 40;

std::string env(const char* name, const std::string& fallback = {}) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : fallback;
}

// Upstream takes its seed from +ntb_random_seed and defaults to 0. Same
// default, from the environment for the same reason as the other ports here.
unsigned seed_from_env() {
    const std::string text = env("CSR_SEED");
    return text.empty() ? 0u : static_cast<unsigned>(std::stoul(text, nullptr, 0));
}

Task<void> cs_registers(Dut dut, TestContext& test) {
    CSRParams params{kPMPEnable, kPMPGranularity, kPMPNumRegions,
                     kMHPMCounterNum, kMHPMCounterWidth};
    SimCtrl simctrl;
    RegisterModel model(&simctrl, &params);

    std::default_random_engine generator(seed_from_env());
    std::uniform_int_distribution<int> delay_dist(kDelayMin, kDelayMax);

    dut.clk_i.set(0);
    dut.rst_ni.set(0);
    dut.csr_access_i.set(0);
    dut.csr_op_en_i.set(0);
    dut.csr_addr_i.set(0);
    dut.csr_wdata_i.set(0);
    dut.csr_op_i.set(0);
    test.start_clock(dut.clk_i, kClockPeriod);

    co_await clock_cycles(dut.clk_i, 4);

    // The model has to be told about the reset, or it keeps the values its
    // constructor left rather than the ones the design comes out of reset
    // with. Upstream gets this for free: its monitor runs from the first edge,
    // so it observes rst_ni low and calls RegisterReset several times before
    // any transaction. Here reset is driven before the monitoring loop starts,
    // so the loop never sees it low and the call has to be explicit.
    //
    // Skipping it is quiet. Most registers reset to zero and the model's
    // initial value is zero too, so the first dozen transactions agree; the
    // first mismatch is on mhpmevent9, whose reset value is 0x1 << (9 - 3),
    // and reads as a design bug rather than a testbench one.
    model.RegisterReset();
    dut.rst_ni.set(1);

    RegisterTransaction next{};
    int delay = 1;
    int driven = 0;
    bool driving = false;

    while (driven < kTransactions && !simctrl.StopRequested()) {
        // Sample on the rising edge, drive on the falling one.
        //
        // `co_await RisingEdge` resumes *before* the design evaluates that
        // edge, so every signal still holds the value it had during the cycle
        // just ending -- which is exactly what upstream's monitor sees, since
        // `always_ff @(posedge clk_i)` samples pre-edge values too.
        //
        // The corollary is the trap. A value set here is seen by the very edge
        // being awaited, so driving from this point makes a write commit in
        // the same cycle it is presented, and the read-back is the value just
        // written rather than the previous one:
        //
        //     Operation: CSR Write   Write data: 4b639620
        //     Read data: 4b639620    Expected rdata: 0
        //
        // That is the scheduling hazard tb_cs_registers.sv describes in the
        // comment explaining why it drives through non-blocking assignments.
        // cpptb has no NBA to reach for, so the driving moves to the falling
        // edge instead, where it is stable well before the edge that commits
        // it. Same effect, and nothing to get wrong at the next edge.
        co_await RisingEdge{dut.clk_i};

        // The gate is upstream's, from monitor_tick in reg_dpi.cc:
        //
        //     if ((csr_access && (csr_op_en || illegal_csr)) || !rst_n)
        //
        // Without it every idle cycle is captured as a read of CSR 0 with
        // access deasserted, and the model expects an illegal instruction for
        // an undefined register.
        const bool accessing = dut.csr_access_i.get() != 0
                               && (dut.csr_op_en_i.get() != 0
                                   || dut.illegal_csr_insn_o.get() != 0);
        if (dut.rst_ni.get() == 0) {
            model.RegisterReset();
        } else if (accessing) {
            auto observed = std::make_unique<RegisterTransaction>();
            observed->illegal_csr =
                static_cast<unsigned char>(dut.illegal_csr_insn_o.get());
            observed->csr_op =
                static_cast<CSRegisterOperation>(dut.csr_op_i.get());
            observed->csr_addr = dut.csr_addr_i.get();
            observed->csr_rdata = dut.csr_rdata_o.get();
            observed->csr_wdata = dut.csr_wdata_i.get();
            model.NewTransaction(std::move(observed));
        }

        co_await FallingEdge{dut.clk_i};

        // Decide what to present next, as RegisterDriver::OnClock does.
        if (--delay == 0) {
            next.Randomize(generator);
            delay = delay_dist(generator);
            driving = true;
            ++driven;
        } else {
            driving = false;
        }
        dut.csr_access_i.set(driving);
        dut.csr_op_en_i.set(driving);
        dut.csr_op_i.set(static_cast<uint32_t>(next.csr_op));
        dut.csr_addr_i.set(next.csr_addr);
        dut.csr_wdata_i.set(next.csr_wdata);
    }

    // SimCtrl is upstream's pass/fail sink and the model reports into it, so
    // the result is the model's verdict rather than this file's opinion.
    test.expect("the register model saw no mismatch", simctrl.TestPassed());
    test.expect_eq("drove the full transaction count", driven, kTransactions);
    std::printf("cpptb: drove %d register transactions\n", driven);
}

CPPTB_REGISTER_TEST(cs_registers);

}  // namespace
}  // namespace cpptb::ports::csr
