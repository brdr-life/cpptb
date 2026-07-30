// Ibex's functional coverage, in cpptb.
//
// `dv/uvm/core_ibex/fcov/core_ibex_fcov_if.sv` declares `uarch_cg`, sampled on
// every posedge of the core clock. This is that covergroup, coverpoint for
// coverpoint, expressed with cpptb's coverage API.
//
// Why it is not simply run in the UVM baseline instead: Verilator 5.050 cannot
// compile it. It stops with
//
//     %Error: Internal Error: core_ibex_fcov_if.sv:589:28: ../V3Ast.h:1061:
//     AstNode is not of expected type, but instead has type 'ENUMITEMREF'
//
// on the transition bins below, and an internal error is a Verilator defect
// rather than a missing feature. Fixing that would not be enough: the same
// compile discards 456 coverage constructs with COVERIGN warnings, among them
// every one of the 208 `intersect` clauses and all 71 explicit cross bins, so
// what it produced would be a much larger bin set than the source describes.
// `fcov_model.py` reads the SystemVerilog and diffs it against what this
// declares, so the correspondence is checked rather than asserted.
//
// Scope. `uarch_cg` has 39 coverpoints and 27 crosses, most of them over
// signals the fcov interface derives in 318 lines of logic that this port does
// not yet carry. What is here is the controller FSM and privilege coverage --
// chosen because the FSM coverpoints are the ones Verilator hard-errors on, so
// they are the clearest case of coverage that exists here and nowhere else.
// Run `python3 fcov_model.py --diff <model.json>` for exactly what is missing;
// that number is meant to be read, not assumed to be zero.

#pragma once

#include <cstdint>
#include <string>

#include "cpptb/coverage.hpp"

namespace cpptb::ports::core_ibex {

// ibex_pkg::ctrl_fsm_e
enum class CtrlFsm : uint8_t {
    Reset = 0,
    BootSet = 1,
    WaitSleep = 2,
    Sleep = 3,
    FirstFetch = 4,
    Decode = 5,
    Flush = 6,
    IrqTaken = 7,
    DbgTakenIf = 8,
    DbgTakenId = 9,
};

// ibex_pkg::priv_lvl_e
enum class PrivLvl : uint8_t {
    User = 0b00,
    Supervisor = 0b01,
    Hypervisor = 0b10,
    Machine = 0b11,
};

struct FcovSample {
    CtrlFsm fsm = CtrlFsm::Reset;
    PrivLvl priv = PrivLvl::Machine;
    PrivLvl priv_lsu = PrivLvl::Machine;
    uint8_t mprv = 0;
    uint8_t debug_mode = 0;
    uint8_t debug_req = 0;
    uint8_t data_ind_timing = 0;
    uint8_t dummy_instr_en = 0;
    uint8_t ls_error_exception = 0;
    uint8_t ls_pmp_exception = 0;
    uint8_t branch_taken = 0;
    uint8_t branch_not_taken = 0;
    uint8_t irq_pending = 0;
    uint8_t wb_reg_no_load_hz = 0;
    uint8_t dummy_instr_if = 0;
    uint8_t dummy_instr_id = 0;
    uint8_t dummy_instr_wb = 0;
    uint8_t fetch_enable = 0;
};

// ibex_pkg::IbexMuBiOn / IbexMuBiOff, the two values cp_fetch_enable names.
inline constexpr uint8_t kMuBiOn = 0x5;
inline constexpr uint8_t kMuBiOff = 0xA;

// uarch_cg, as far as it is ported.
class Uarch {
   public:
    Uarch() : group_("uarch_cg") {
        // cp_controller_fsm: coverpoint id_stage_i.controller_i.ctrl_fsm_cs
        auto& fsm = group_.coverpoint(
            "cp_controller_fsm", [](const FcovSample& s) { return s.fsm; });
        fsm.transition_bin("out_of_reset", CtrlFsm::Reset, CtrlFsm::BootSet)
            .transition_bin("out_of_boot_set", CtrlFsm::BootSet,
                            CtrlFsm::FirstFetch)
            .transition_bin("out_of_first_fetch0", CtrlFsm::FirstFetch,
                            CtrlFsm::Decode)
            .transition_bin("out_of_first_fetch1", CtrlFsm::FirstFetch,
                            CtrlFsm::IrqTaken)
            .transition_bin("out_of_first_fetch2", CtrlFsm::FirstFetch,
                            CtrlFsm::DbgTakenIf)
            .transition_bin("out_of_decode0", CtrlFsm::Decode, CtrlFsm::Flush)
            .transition_bin("out_of_decode1", CtrlFsm::Decode,
                            CtrlFsm::DbgTakenIf)
            .transition_bin("out_of_decode2", CtrlFsm::Decode,
                            CtrlFsm::IrqTaken)
            .transition_bin("out_of_irq_taken", CtrlFsm::IrqTaken,
                            CtrlFsm::Decode)
            .transition_bin("out_of_debug_taken_if", CtrlFsm::DbgTakenIf,
                            CtrlFsm::Decode)
            .transition_bin("out_of_debug_taken_id", CtrlFsm::DbgTakenId,
                            CtrlFsm::Decode)
            .transition_bin("out_of_flush0", CtrlFsm::Flush, CtrlFsm::Decode)
            .transition_bin("out_of_flush1", CtrlFsm::Flush,
                            CtrlFsm::DbgTakenId)
            .transition_bin("out_of_flush2", CtrlFsm::Flush,
                            CtrlFsm::WaitSleep)
            .transition_bin("out_of_flush3", CtrlFsm::Flush,
                            CtrlFsm::DbgTakenIf)
            .transition_bin("out_of_wait_sleep", CtrlFsm::WaitSleep,
                            CtrlFsm::Sleep)
            .transition_bin("out_of_sleep", CtrlFsm::Sleep,
                            CtrlFsm::FirstFetch)
            // "TODO: VCS does not implement default sequence so illegal_bins
            // will be empty" -- upstream's note. Here it is implemented, so an
            // FSM edge the list above does not name is caught rather than
            // ignored, which is a check the baseline does not have on any
            // simulator it runs on.
            .illegal_default_sequence("illegal_transitions");

        // cp_controller_fsm_sleep: the same signal, two transitions.
        auto& sleep = group_.coverpoint(
            "cp_controller_fsm_sleep",
            [](const FcovSample& s) { return s.fsm; });
        sleep.transition_bin("out_of_sleep", CtrlFsm::Sleep,
                             CtrlFsm::FirstFetch)
            .transition_bin("enter_sleep", CtrlFsm::WaitSleep, CtrlFsm::Sleep)
            .illegal_default_sequence("illegal_transitions");

        // cp_priv_mode_id: coverpoint priv_mode_id { illegal_bins illegal =
        // {PRIV_LVL_H, PRIV_LVL_S}; }
        //
        // No bins body beyond the illegal ones, so SystemVerilog auto-bins the
        // rest: one bin per value of priv_lvl_e. Two of the four are illegal,
        // leaving M and U, which are declared here because cpptb has no type
        // reflection to derive them from.
        auto& priv = group_.coverpoint(
            "cp_priv_mode_id", [](const FcovSample& s) { return s.priv; });
        priv.bin("PRIV_LVL_M", PrivLvl::Machine)
            .bin("PRIV_LVL_U", PrivLvl::User)
            .illegal_bin("illegal", {typename Coverpoint<PrivLvl>::Range{
                                         PrivLvl::Supervisor},
                                     typename Coverpoint<PrivLvl>::Range{
                                         PrivLvl::Hypervisor}});

        // cp_priv_mode_lsu, the same shape as cp_priv_mode_id.
        auto& priv_lsu = group_.coverpoint(
            "cp_priv_mode_lsu", [](const FcovSample& s) { return s.priv_lsu; });
        priv_lsu.bin("PRIV_LVL_M", PrivLvl::Machine)
            .bin("PRIV_LVL_U", PrivLvl::User)
            .illegal_bin("illegal", {typename Coverpoint<PrivLvl>::Range{
                                         PrivLvl::Supervisor},
                                     typename Coverpoint<PrivLvl>::Range{
                                         PrivLvl::Hypervisor}});

        // The single-bit coverpoints declared with no bins body. SystemVerilog
        // auto-bins those: one bin per value of the sampled type, which for a
        // one-bit signal is two, named `auto[0]` and `auto[1]`. cpptb has no
        // type reflection to derive them, so they are declared, and the names
        // follow what a SystemVerilog report would show.
        auto_bit("cp_mprv", [](const FcovSample& s) { return s.mprv; });
        auto_bit("cp_debug_mode",
                 [](const FcovSample& s) { return s.debug_mode; });
        auto_bit("cp_debug_req",
                 [](const FcovSample& s) { return s.debug_req; });
        auto_bit("cp_data_ind_timing",
                 [](const FcovSample& s) { return s.data_ind_timing; });
        auto_bit("cp_dummy_instr_en",
                 [](const FcovSample& s) { return s.dummy_instr_en; });
        auto& ls_error = auto_bit(
            "cp_ls_error_exception",
            [](const FcovSample& s) { return s.ls_error_exception; });
        auto& ls_pmp = auto_bit(
            "cp_ls_pmp_exception",
            [](const FcovSample& s) { return s.ls_pmp_exception; });
        auto_bit("cp_branch_taken",
                 [](const FcovSample& s) { return s.branch_taken; });
        auto_bit("cp_branch_not_taken",
                 [](const FcovSample& s) { return s.branch_not_taken; });
        auto_bit("cp_irq_pending",
                 [](const FcovSample& s) { return s.irq_pending; });
        auto_bit("cp_wb_reg_no_load_hz",
                 [](const FcovSample& s) { return s.wb_reg_no_load_hz; });
        auto_bit("cp_dummy_instr_if_stage",
                 [](const FcovSample& s) { return s.dummy_instr_if; });
        auto_bit("cp_dummy_instr_id_stage",
                 [](const FcovSample& s) { return s.dummy_instr_id; });
        auto_bit("cp_dummy_instr_wb_stage",
                 [](const FcovSample& s) { return s.dummy_instr_wb; });

        // cp_fetch_enable, whose third bin is `default`: everything the first
        // two do not name. cpptb has no default bin, and the multi-bit encoding
        // means "not on and not off" is every other value of the field, so it
        // is declared as the complement of the two named values.
        auto& fetch = group_.coverpoint(
            "cp_fetch_enable",
            [](const FcovSample& s) { return s.fetch_enable; });
        fetch.bin("fetch_on", kMuBiOn).bin("fetch_off", kMuBiOff);
        for (uint16_t value = 0; value < 16; ++value) {
            if (value == kMuBiOn || value == kMuBiOff) continue;
            fetch.bin("fetch_inval[" + std::to_string(value) + "]",
                      static_cast<uint8_t>(value));
        }

        // priv_mode_exception_cross: cross cp_priv_mode_id,
        //   cp_ls_pmp_exception, cp_ls_error_exception {
        //     illegal_bins pmp_and_error_exeption_both =
        //       (binsof(cp_ls_pmp_exception) intersect {1'b1} &&
        //        binsof(cp_ls_error_exception) intersect {1'b1});
        //   }
        //
        // Three coverpoints and a select expression -- the two things Verilator
        // drops. Without the filter this cross would have 2*2*2 bins and the
        // combination the design must never produce would be counted as
        // coverable rather than caught.
        group_.cross("priv_mode_exception_cross", priv, ls_pmp, ls_error)
            .illegal("pmp_and_error_exeption_both",
                     binsof(ls_pmp, {uint8_t{1}}) &&
                         binsof(ls_error, {uint8_t{1}}));
    }

    CoverageSampleResult sample(const FcovSample& value) {
        return group_.sample(value);
    }

    CoverageSnapshot snapshot() const { return group_.snapshot(); }

   private:
    // A coverpoint over a one-bit signal with no bins body, which
    // SystemVerilog auto-bins into one bin per value.
    template <typename Extractor>
    Coverpoint<uint8_t>& auto_bit(std::string name, Extractor extractor) {
        auto& point = group_.coverpoint(std::move(name), extractor);
        point.bin("auto[0]", uint8_t{0}).bin("auto[1]", uint8_t{1});
        return point;
    }

    Covergroup<FcovSample> group_;
};

}  // namespace cpptb::ports::core_ibex
