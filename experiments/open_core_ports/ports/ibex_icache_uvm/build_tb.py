#!/usr/bin/env python3
"""Build Ibex's icache UVM testbench (dv/uvm/icache) with Verilator.

This is Ibex's second UVM testbench, and a block-level one: the DUT is
`ibex_icache` alone, driven by a core agent on the fetch side and a memory
agent on the bus side, with a scoreboard that models the cache. Upstream signs
it off with VCS and runs it through OpenTitan's dvsim; the tool list in
`dv/ibex_icache_sim_cfg.hjson` does not include Verilator.

    python3 build_tb.py
    python3 build_tb.py --jobs 4

The build description is a FuseSoC CAPI=2 graph rather than the `.f` file lists
`dv/uvm/core_ibex` uses. `fusesoc_setup.py` asks fusesoc for it; see the docstring
there for why that rather than driving fusesoc.

Standard library only, matching the other tools here.
"""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

import fusesoc_setup

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
IBEX = ROOT / "deps" / "ibex"
LOWRISC_IP = IBEX / "vendor" / "lowrisc_ip"
ICACHE = IBEX / "dv" / "uvm" / "icache"
UVM = ROOT / "deps" / "uvm_core" / "src"
BUILD = HERE / "build"
OBJ = BUILD / "obj"

# The core and target `ibex_icache_sim_cfg.hjson` names in `fusesoc_core`.
TOP_CORE = "lowrisc:dv:ibex_icache_sim:0.1"
TOP_TARGET = "sim"

# The build defines `common_sim_cfg.hjson` applies to every OpenTitan-style DV
# build, reproduced here because dvsim is not what drives this. UVM_REGEX_NO_DPI
# is deliberately not among them: it is in upstream's list, and UVM's own
# uvm_regex.cc is compiled by shims/uvm_dpi_verilator.cc, so the DPI regex is
# available and is the faster path.
COMMON_DEFINES = {
    "UVM": "",
    "UVM_NO_DEPRECATED": "",
    "UVM_REG_ADDR_WIDTH": "32",
    "UVM_REG_DATA_WIDTH": "32",
    "UVM_REG_BYTENABLE_WIDTH": "4",
    "SIMULATION": "",
    "DUT_HIER": "tb.dut",
}


class BuildError(RuntimeError):
    pass


# ---------------------------------------------------------------------------
# Overlays
#
# Exact-text patches applied to copies under build/overlay/, never to deps/.
# A replacement that stops matching exactly once fails the build with the file
# named, so an upstream edit is something to look at rather than something that
# silently changes what is built.
# ---------------------------------------------------------------------------

OVERLAYS: list[tuple[Path, str, str]] = [
    (
        ICACHE / "dv/tests/ibex_icache_base_test.sv",
        '  virtual function void build_phase(uvm_phase phase);\n    super.build_phase(phase);\n  endfunction\n',
        '  virtual function void build_phase(uvm_phase phase);\n    super.build_phase(phase);\n  endfunction\n\n  // "Dropping response for sequence N, sequence not found" comes from\n  // uvm_sequencer_param_base::put_response when a response arrives for a\n  // sequence that has gone. UVM 1.2, which upstream runs, reports it with\n  // uvm_report_info. Accellera 1800.2, used here because no UVM ships with\n  // this simulator, raised it to uvm_report_warning, and lowRISC counts any\n  // UVM_WARNING as a failure: dv_report_server says so and\n  // common_sim_cfg.hjson lists ^UVM_WARNING as a run-fail pattern. So\n  // ibex_icache_stress_all_with_reset fails here on a message upstream never\n  // sees.\n  //\n  // Restoring the 1.2 severity keeps the verdict comparable with upstream\n  // rather than hiding a real failure. The race is real either way:\n  // random_reset kills sequences while the driver has responses in flight and\n  // those responses are dropped. Upstream does not call that a failure.\n  //\n  // The override is per report object and has no hierarchical form, so it is\n  // applied to every component once elaboration has built the tree.\n  virtual function void end_of_elaboration_phase(uvm_phase phase);\n    uvm_component components[$];\n    super.end_of_elaboration_phase(phase);\n    uvm_top.find_all("*", components);\n    foreach (components[i]) begin\n      components[i].set_report_severity_id_override(UVM_WARNING, "Sequencer",\n                                                    UVM_INFO);\n    end\n  endfunction\n',
    ),
    # Both agent interfaces open their driver clocking block with
    #
    #     clocking driver_cb @(posedge clk);
    #       default output negedge;
    #
    # which is an edge-based output skew. Verilator does not implement it:
    # "Unsupported: clocking event edge override".
    #
    # Dropping the line and taking the default #0 output skew drives the same
    # value into the same posedge of the DUT. A clocking block output written
    # with a skew of 0 is driven in the NBA region of the clocking event, so a
    # design sampling on that same posedge still reads the old value and picks
    # the new one up on the next posedge -- exactly where the negedge drive
    # would have put it. Upstream says as much in the comment above the line:
    # the negedge is there to make dumped waves easier to read, not because
    # the design needs it.
    #
    # The alternative, moving the clocking event to @(negedge clk), is what
    # ports/core_ibex_uvm does for irq_if.sv. It is worse here, because
    # driver_cb in the core interface has inputs as well: valid and err would
    # then be sampled after the posedge rather than before it, which advances
    # every input the driver reads by a cycle.
    (
        ICACHE / "dv/ibex_icache_core_agent/ibex_icache_core_if.sv",
        "\n"
        "    // Drive signals on the following negedge: this isn't needed by"
        " the design, but makes it\n"
        "    // slightly easier to read dumped waves.\n"
        "    default output negedge;\n",
        "\n"
        "    // The output skew that was here drove signals on the following\n"
        "    // negedge, for wave readability rather than for the design. The\n"
        "    // default #0 skew lands the same value at the same posedge of\n"
        "    // the DUT; see build_tb.py.\n",
    ),
    (
        ICACHE / "dv/ibex_icache_mem_agent/ibex_icache_mem_if.sv",
        "  default clocking driver_cb @(posedge clk);\n"
        "    default output negedge;\n",
        "  default clocking driver_cb @(posedge clk);\n"
        "    // The output skew that was here is dropped; see build_tb.py and\n"
        "    // the note on the core interface.\n",
    ),
    # A one-cycle pulse driven through a clocking block output disappears
    # unless the first write happens on a clocking event.
    #
    #     cb.pulse <= 1'b1;   // should be driven at the next clocking event
    #     @(cb);              // wakes at that event
    #     cb.pulse <= 1'b0;   // should be driven at the event after it
    #
    # This simulator applies a write made after `@(cb)` to the same clocking
    # event as the earlier one, so the two collapse and the signal never goes
    # high. Reduced case in shims/verilator_clocking_pulse.sv.
    #
    # Upstream never sees it because `default output negedge` puts the drive
    # half a cycle after the event, which separates the two. Verilator does not
    # implement that skew, and a numeric skew does not help either -- the
    # reduced case shows `default output #2ns` losing the pulse in the same way.
    #
    # Waiting for a clocking event before the first write fixes it and gives
    # back exactly upstream's timing. Off a clocking event, upstream's negedge
    # drive lands after the *next* posedge, so the pulse is seen one edge
    # later; waiting first produces the same edge. On a clocking event both
    # produce a pulse at the following edge.
    #
    # This is what breaks ibex_icache_stress_all. dv_base_vseq::dut_init ends
    # with `#1ps`, so the first item of every child sequence after a mid-test
    # reset is driven one picosecond off the clock grid; branch_to() then
    # produces no branch at all, the cache prefetches from prefetch_addr_q
    # (which ResetAll=0 leaves unreset), and the scoreboard sees a fetch it
    # was not told to expect.
    (
        ICACHE / "dv/ibex_icache_core_agent/ibex_icache_core_if.sv",
        "  // SVA module\n"
        "  ibex_icache_core_protocol_checker checker_i (.*);\n",
        "  // The time of the last clocking event, and a task that waits for\n"
        "  // one if we are not on it. Needed before driving a single-cycle\n"
        "  // pulse through driver_cb; see build_tb.py.\n"
        "  realtime driver_cb_edge       = 0.0;\n"
        "  bit      driver_cb_edge_valid = 1'b0;\n"
        "  always @(posedge clk) begin\n"
        "    driver_cb_edge       = $realtime;\n"
        "    driver_cb_edge_valid = 1'b1;\n"
        "  end\n"
        "  task automatic align_to_driver_cb();\n"
        "    if (!driver_cb_edge_valid || ($realtime != driver_cb_edge))"
        " @(driver_cb);\n"
        "  endtask\n"
        "\n"
        "  // SVA module\n"
        "  ibex_icache_core_protocol_checker checker_i (.*);\n",
    ),
    (
        ICACHE / "dv/ibex_icache_core_agent/ibex_icache_core_if.sv",
        "  task automatic branch_to(logic [31:0] addr);\n"
        "    driver_cb.branch      <= 1'b1;\n",
        "  task automatic branch_to(logic [31:0] addr);\n"
        "    align_to_driver_cb();\n"
        "    driver_cb.branch      <= 1'b1;\n",
    ),
    (
        ICACHE / "dv/ibex_icache_core_agent/ibex_icache_core_if.sv",
        "  task automatic invalidate_pulse(int unsigned num_cycles);\n"
        "    driver_cb.invalidate <= 1'b1;\n",
        "  task automatic invalidate_pulse(int unsigned num_cycles);\n"
        "    align_to_driver_cb();\n"
        "    driver_cb.invalidate <= 1'b1;\n",
    ),
    # The memory response is the same shape. drive_responses() calls this
    # straight out of `rdata_queue.get()` when the item's delay is zero, which
    # the distribution picks about five times in eleven, and the rvalid pulse
    # is lost the same way.
    (
        ICACHE / "dv/ibex_icache_mem_agent/ibex_icache_mem_if.sv",
        "  // Interface with SVA assertions\n"
        "  ibex_icache_mem_protocol_checker checker_i (.*);\n",
        "  // See the note on the same tracker in ibex_icache_core_if.sv and in\n"
        "  // build_tb.py: a pulse through a clocking block output is lost\n"
        "  // unless the first write is made on a clocking event.\n"
        "  realtime driver_cb_edge       = 0.0;\n"
        "  bit      driver_cb_edge_valid = 1'b0;\n"
        "  always @(posedge clk) begin\n"
        "    driver_cb_edge       = $realtime;\n"
        "    driver_cb_edge_valid = 1'b1;\n"
        "  end\n"
        "  task automatic align_to_driver_cb();\n"
        "    if (!driver_cb_edge_valid || ($realtime != driver_cb_edge))"
        " @(driver_cb);\n"
        "  endtask\n"
        "\n"
        "  // Interface with SVA assertions\n"
        "  ibex_icache_mem_protocol_checker checker_i (.*);\n",
    ),
    (
        ICACHE / "dv/ibex_icache_mem_agent/ibex_icache_mem_if.sv",
        "  task automatic send_response(logic rsp_err, logic [31:0] rsp_rdata);\n"
        "    driver_cb.rvalid <= 1'b1;\n",
        "  task automatic send_response(logic rsp_err, logic [31:0] rsp_rdata);\n"
        "    align_to_driver_cb();\n"
        "    driver_cb.rvalid <= 1'b1;\n",
    ),
    # tb.sv overrides a parameter the DUT does not have. `ibex_icache` calls
    # it `TweakInfection`; `ICacheTweakInfection` is the name the wrapper
    # parameter carries in ibex_if_stage.sv, ibex_core.sv and ibex_top.sv,
    # which is presumably where it was copied from. Verilator reports
    # "Parameter not found: 'ICacheTweakInfection'" and stops.
    #
    # This is an upstream defect rather than anything about Verilator: no
    # simulator can elaborate tb.sv as vendored. See README.md.
    (
        ICACHE / "dv/tb/tb.sv",
        "      .ICacheTweakInfection (ICacheTweakInfection),\n",
        "      .TweakInfection       (ICacheTweakInfection),\n",
    ),
    # `DV_COMMON_CLK_CONSTRAINT is a weighted `dist` over 5..100 MHz, and
    # ibex_icache_env_cfg derives from the class that carries it and adds
    #
    #     constraint clk_freq_50_c { clk_freq_mhz == 50; }
    #
    # 50 is inside the distribution's support, so the pair is satisfiable.
    # This simulator draws a sample from the distribution first and then
    # asserts equality against that sample, so the whole solve is unsatisfiable
    # unless the sample happened to be 50. Measured over 25 seeds of this
    # testbench, one got past `DV_CHECK_RANDOMIZE_FATAL in dv_base_test; the
    # other 24 died at time 0 with "Randomization failed!" and no diagnostic.
    # Reduced case, with the rates, in shims/verilator_dist_plus_equality.sv.
    #
    # Replacing the distribution with its own support as an `inside` keeps
    # every frequency the constraint allows and drops the weights. In this
    # testbench that costs nothing at all: clk_freq_50_c determines the value,
    # and the `foreach` arm of the same constraint iterates clk_freqs_mhz,
    # which is empty here because the icache has no RAL model.
    (
        LOWRISC_IP / "dv/sv/dv_utils/dv_macros.svh",
        "`define DV_COMMON_CLK_CONSTRAINT(FREQ_) \\\n"
        "  FREQ_ dist { \\\n"
        "    [5:23]  :/ 2, \\\n"
        "    [24:25] :/ 2, \\\n"
        "    [26:47] :/ 1, \\\n"
        "    [48:50] :/ 2, \\\n"
        "    [51:95] :/ 1, \\\n"
        "    96      :/ 1, \\\n"
        "    [97:99] :/ 1, \\\n"
        "    100     :/ 1  \\\n"
        "  };\n",
        "`define DV_COMMON_CLK_CONSTRAINT(FREQ_) \\\n"
        "  FREQ_ inside {[5:100]};\n",
    ),
    # `uvm_phase::phase_done` is null here. In UVM 1.2 the phase constructor
    # creates the objection for every task-phase node; in Accellera's
    # 1800.2-2020.3.1 it is created on demand by `get_objection()`, and nothing
    # has called that by the time a component's run_phase starts. dv_base_test
    # reaches for the member directly on its first statement and the run stops
    # with "Null pointer dereferenced".
    #
    # `get_objection()` is the 1800.2 accessor and returns the same object,
    # creating it if it is not there yet. This is the only direct use of
    # phase_done in lowRISC's DV library.
    #
    # Not a Verilator defect: it is a UVM version difference, visible here
    # because Verilator ships no UVM and this build supplies Accellera's.
    (
        LOWRISC_IP / "dv/sv/dv_lib/dv_base_test.sv",
        "    phase.phase_done.set_drain_time(this, (drain_time_ns * 1ns));\n",
        "    // phase_done is created on demand in UVM 1800.2; get_objection()\n"
        "    // is the accessor that creates it. See build_tb.py.\n"
        "    phase.get_objection().set_drain_time(this, (drain_time_ns * 1ns));\n",
    ),
    # tb.sv calls clk_rst_if.set_active() as the first statement of its initial
    # block, and clk_rst_if's own initial block blocks on `@set_active_called`.
    # Both are at time 0 with nothing between them, so which runs first is
    # arbitrary. On this simulator tb's runs first, the event fires with nobody
    # waiting on it, and the clock generator never starts: time steps straight
    # from the reset assertion to the UVM timeout with no clock edge at all.
    #
    # Upstream already knows about this race and works around it one testbench
    # over: core_ibex_tb_top.sv has `#0; // needed for dsim` immediately before
    # its own set_active() call. The same `#0` here yields to the interface's
    # initial block and the clock starts.
    (
        ICACHE / "dv/tb/tb.sv",
        "  initial begin\n"
        "    // drive clk and rst_n from clk_if\n"
        "    clk_rst_if.set_active();\n",
        "  initial begin\n"
        "    // drive clk and rst_n from clk_if\n"
        "    // The #0 lets clk_rst_if's initial block reach its wait on\n"
        "    // set_active_called before the event is triggered. Upstream does\n"
        "    // the same thing in core_ibex_tb_top.sv. See build_tb.py.\n"
        "    #0;\n"
        "    clk_rst_if.set_active();\n",
    ),
    # ------------------------------------------------------------------
    # The distributions.
    #
    # Three separate defects in this simulator's `dist` handling, all measured
    # in shims/verilator_dist_plus_equality.sv and shims/verilator_dist_std_
    # randomize.sv:
    #
    #   1. A `dist` is applied as an equality against a sample drawn from the
    #      distribution before the solve. Any other constraint on the same
    #      variable then makes the whole call unsatisfiable unless the sample
    #      happens to satisfy it. randomize() returns 0.
    #   2. `std::randomize(x) with { x dist {...} }` ignores the weights
    #      entirely and returns a uniform pick over the support. The support
    #      itself, including a zero weight, is honoured.
    #   3. A `soft` constraint nested inside an `if` in a constraint block is
    #      honoured when it is satisfiable and *not* dropped when it is not,
    #      which is the opposite of what soft means. Only top-level soft
    #      constraints behave correctly, so every soft constraint added below
    #      is at the top level.
    #
    # The fix throughout is the one ports/core_ibex_uvm arrived at: keep the
    # buckets and the weights, draw the value with $urandom_range instead of
    # asking the solver for it. Where the value has to stay in the solve
    # because other constraints narrow it, the drawn bucket is applied as a
    # top-level `soft` instead of an equality.
    # ------------------------------------------------------------------
    #
    # base_addr carries a three-bucket dist and, separately, an alignment
    # constraint. Together they fail about half the time, which is
    # `DV_CHECK_RANDOMIZE_FATAL(core_seq) in ibex_icache_base_vseq at the start
    # of every test. base_addr is read in body() and in the inline constraint
    # on branch_addr, both after this point, so drawing it at the top of body()
    # is the same value in the same places.
    (
        ICACHE / "dv/ibex_icache_core_agent/seq_lib/ibex_icache_core_base_seq.sv",
        "  // Distribution for base_addr which adds extra weight to each end of"
        " the address space\n"
        "  constraint c_base_addr { base_addr dist { [0:15]                    "
        "  :/ 1,\n"
        "                                            [16:32'hfffffff0]         "
        "  :/ 2,\n"
        "                                            [32'hfffffff0:32'hffffffff]"
        " :/ 1 }; }\n"
        "\n"
        "  // Make sure that base_addr is even (otherwise you can fail to"
        " generate items if you pick a base\n"
        "  // addr of 32'hffffffff with constrained addresses enabled: the only"
        " address large enough is\n"
        "  // 32'hffffffff, but it doesn't have a zero bottom bit).\n"
        "  constraint c_base_addr_alignment {\n"
        "    // The branch address is always required to be half-word aligned."
        " We don't bother conditioning\n"
        "    // this on the type of transaction because branch_addr is forced to"
        " be zero in anything but a\n"
        "    // branch transaction.\n"
        "    !base_addr[0];\n"
        "  }\n",
        "  // The dist and the alignment constraint that were here are drawn in\n"
        "  // draw_base_addr() below and applied at the top of body(). Same\n"
        "  // three buckets, same weights, same alignment. See build_tb.py.\n"
        "  protected function bit [31:0] draw_base_addr();\n"
        "    bit [31:0]   value;\n"
        "    int unsigned pick = $urandom_range(3, 0);\n"
        "    if (pick == 0)      value = $urandom_range(15, 0);\n"
        "    else if (pick == 3) value = $urandom_range(32'hffffffff,"
        " 32'hfffffff0);\n"
        "    else                value = $urandom_range(32'hfffffff0, 16);\n"
        "    value[0] = 1'b0;\n"
        "    return value;\n"
        "  endfunction\n",
    ),
    (
        ICACHE / "dv/ibex_icache_core_agent/seq_lib/ibex_icache_core_base_seq.sv",
        "  virtual task body();\n"
        "    // Set cache_enabled from initial_enable here",
        "  virtual task body();\n"
        "    base_addr = draw_base_addr();\n"
        "\n"
        "    // Set cache_enabled from initial_enable here",
    ),
    # run_req is where the stimulus is actually made, and every field in it
    # except num_insns is either pinned by an implication or drawn from a dist
    # that an implication also touches. Each one is drawn here with the same
    # weights and the implications applied in the order upstream writes them,
    # so the solver is left with num_insns and, when constrain_branches is set,
    # branch_addr.
    #
    # new_seed is the one worth reading twice. Its weight expression is
    #
    #     new_seed dist { 0 :/ 1, [1:32'hffffffff] :/ (invalidate ? 1000 :
    #                                                  enable ? 0 : 1) }
    #
    # and the zero in it is load-bearing: it is the only thing stopping a new
    # memory seed reaching an enabled cache, which the scoreboard would see as
    # a multi-way hit. The weights are not constant and this simulator gets
    # non-constant weights wrong, so that rule cannot be left to it.
    (
        ICACHE / "dv/ibex_icache_core_agent/seq_lib/ibex_icache_core_base_seq.sv",
        """  protected virtual task run_req(ibex_icache_core_req_item req, ibex_icache_core_rsp_item rsp);
    start_item(req);

    if (constrain_branches && insns_since_branch >= 100)
      force_branch = 1'b1;

    `DV_CHECK_RANDOMIZE_WITH_FATAL(
       req,

       // Force a branch if necessary
       force_branch -> req.trans_type == ICacheCoreTransTypeBranch;

       // If this is a branch and constrain_branches is true then constrain any branch target.
       (constrain_branches && (req.trans_type == ICacheCoreTransTypeBranch)) ->
         req.branch_addr inside {[base_addr:top_restricted_addr]};

       // If this is a branch and constrain_branches is false, we don't constrain the branch target,
       // but we do weight the bottom and top of the address space a bit higher. This is a weaker
       // version of the weighting that we place on base_addr.
       (!constrain_branches && (req.trans_type == ICacheCoreTransTypeBranch)) ->
         req.branch_addr dist { [0:63]                      :/ 1,
                                [64:32'hffffffbf]           :/ 20,
                                [32'hffffffc0:32'hffffffff] :/ 1 };

       // If this is a branch and constrain_branches is true, we can ask for up to 100 instructions
       // (independent of insns_since_branch)
       (constrain_branches && (req.trans_type == ICacheCoreTransTypeBranch)) ->
         num_insns <= 100;

       // If this isn't a branch and constrain_branches is true, we can ask for up to 100 -
       // insns_since_branch instructions. This will be positive because of the check above.
       (constrain_branches && (req.trans_type != ICacheCoreTransTypeBranch)) ->
         num_insns <= 100 - insns_since_branch;

       const_enable -> enable == cache_enabled;

       // Toggle the cache enable line one time in 50. This should allow us a reasonable amount of
       // time in each mode (note that each transaction here results in multiple instruction
       // fetches)
       enable dist { cache_enabled :/ gap_between_toggle_enable, ~cache_enabled :/ 1 };

       // If must_invalidate is set, we have to invalidate with this item.
       must_invalidate -> invalidate == 1'b1;

       // If avoid_invalidation is set, we won't touch the invalidate line, unless we have to.
       (avoid_invalidation && !(must_invalidate || (stale_seed && enable))) -> invalidate == 1'b0;

       // Start an invalidation every 1+gap_between_invalidations items.
       invalidate dist { 1'b0 :/ gap_between_invalidations, 1'b1 :/ 1 };

       // If we have seen a new seed since the last invalidation (which must have happened while the
       // cache was disabled) and this item is enabled, force the cache to invalidate.
       stale_seed && enable -> invalidate == 1'b1;
    )
""",
        """  protected virtual task run_req(ibex_icache_core_req_item req, ibex_icache_core_rsp_item rsp);
    // Every field that upstream draws from a dist is drawn here instead, with
    // the same buckets and the same weights. See build_tb.py for why.
    ibex_icache_core_trans_type_e d_trans_type;
    bit [31:0]                    d_branch_addr;
    bit                           d_enable;
    bit                           d_invalidate;
    bit [31:0]                    d_new_seed;
    int unsigned                  d_insn_lo, d_insn_hi;
    int unsigned                  pick;
    int unsigned                  seed_weight;

    start_item(req);

    if (constrain_branches && insns_since_branch >= 100)
      force_branch = 1'b1;

    // trans_type carries no dist and no constraint but force_branch, so an
    // unconstrained solve is a uniform pick over the two enum values.
    d_trans_type = force_branch ? ICacheCoreTransTypeBranch :
                   ibex_icache_core_trans_type_e'($urandom_range(1, 0));

    // The branch target for the unconstrained case: the same three buckets and
    // weights, and even, which is what c_branch_addr_alignment asks for.
    pick = $urandom_range(21, 0);
    if (pick == 0)       d_branch_addr = $urandom_range(63, 0);
    else if (pick == 21) d_branch_addr = $urandom_range(32'hffffffff, 32'hffffffc0);
    else                 d_branch_addr = $urandom_range(32'hffffffbf, 64);
    d_branch_addr[0] = 1'b0;

    // Toggle the cache enable line one time in gap_between_toggle_enable + 1,
    // unless const_enable says never.
    d_enable = const_enable ? cache_enabled :
               (($urandom_range(gap_between_toggle_enable, 0) == 0) ?
                  ~cache_enabled : cache_enabled);

    // invalidate, with upstream's three implications applied ahead of the
    // distribution behind them.
    if (must_invalidate)             d_invalidate = 1'b1;
    else if (stale_seed && d_enable) d_invalidate = 1'b1;
    else if (avoid_invalidation)     d_invalidate = 1'b0;
    else d_invalidate = ($urandom_range(gap_between_invalidations, 0) == 0);

    // new_seed: dist { 0 :/ 1, nonzero :/ W }. W is 1000 when we are starting
    // an invalidation, 0 when the cache is enabled and 1 otherwise, and the
    // zero is what keeps a new seed away from an enabled cache.
    seed_weight = d_invalidate ? 1000 : (d_enable ? 0 : 1);
    if (seed_weight == 0 || $urandom_range(seed_weight, 0) == 0) begin
      d_new_seed = 32'd0;
    end else begin
      do d_new_seed = $urandom(); while (d_new_seed == 32'd0);
    end

    // num_insns keeps its solve, because the caps below narrow it. The bucket
    // is drawn with the dist's weights and applied as a top-level soft
    // constraint, so a bucket the caps rule out is dropped rather than
    // failing the call.
    if (d_trans_type == ICacheCoreTransTypeBranch) begin
      pick = $urandom_range(25, 0);
      if (pick < 5)       begin d_insn_lo = 0;  d_insn_hi = 0;   end
      else if (pick < 25) begin d_insn_lo = 1;  d_insn_hi = 20;  end
      else                begin d_insn_lo = 21; d_insn_hi = 100; end
    end else begin
      pick = $urandom_range(20, 0);
      if (pick < 1) begin d_insn_lo = 0; d_insn_hi = 0;  end
      else          begin d_insn_lo = 1; d_insn_hi = 20; end
    end

    `DV_CHECK_RANDOMIZE_WITH_FATAL(
       req,

       req.trans_type == d_trans_type;

       // If this is a branch and constrain_branches is true then constrain any branch target.
       (constrain_branches && (req.trans_type == ICacheCoreTransTypeBranch)) ->
         req.branch_addr inside {[base_addr:top_restricted_addr]};

       // The unconstrained branch target, drawn above.
       (!constrain_branches && (req.trans_type == ICacheCoreTransTypeBranch)) ->
         req.branch_addr == d_branch_addr;

       // If this is a branch and constrain_branches is true, we can ask for up to 100 instructions
       // (independent of insns_since_branch)
       (constrain_branches && (req.trans_type == ICacheCoreTransTypeBranch)) ->
         num_insns <= 100;

       // If this isn't a branch and constrain_branches is true, we can ask for up to 100 -
       // insns_since_branch instructions. This will be positive because of the check above.
       (constrain_branches && (req.trans_type != ICacheCoreTransTypeBranch)) ->
         num_insns <= 100 - insns_since_branch;

       soft num_insns inside {[d_insn_lo:d_insn_hi]};

       enable     == d_enable;
       invalidate == d_invalidate;
       new_seed   == d_new_seed;
    )
""",
    ),
    # The item's own two dists go with them: c_num_insns_dist becomes the
    # support the dist implied, which the bucket above narrows, and
    # c_new_seed_dist goes away because run_req now pins new_seed.
    (
        ICACHE / "dv/ibex_icache_core_agent/ibex_icache_core_req_item.sv",
        """  constraint c_num_insns_dist {
    // For branch transactions, we want to read zero instructions reasonably frequently. For req
    // transactions, much less so. Also, we don't bother with long sequences for req transactions:
    // they won't look any different from the tail end of branch transactions from the cache's point
    // of view.
    if (trans_type == ICacheCoreTransTypeBranch)
      num_insns dist { 0 :/ 5, [1:20] :/ 20, [21:100] :/ 1 };
    else
      num_insns dist { 0 :/ 1, [1:20] :/ 20 };
  }

  constraint c_new_seed_dist {
    // We always want to pick a new seed when we start an invalidation (for maximum test
    // sensitivity). If we aren't starting an invalidation, we mustn't pick a new seed if the cache
    // is enabled (because that could cause multi-way hits). If the cache isn't enabled, pick a new
    // seed sometimes.
    new_seed dist { 0 :/ 1, [1:32'hffffffff] :/ (invalidate ? 1000 : enable ? 0 : 1) };
  }
""",
        """  // What is left of c_num_insns_dist: the support the distribution
  // implied. The weighting is a soft bucket added by run_req in
  // ibex_icache_core_base_seq.sv, and c_new_seed_dist is drawn there too.
  // See build_tb.py.
  constraint c_num_insns_support {
    if (trans_type == ICacheCoreTransTypeBranch) num_insns inside {[0:100]};
    else                                         num_insns inside {[0:20]};
  }
""",
    ),
    # ibex_icache_core_back_line_seq overrides run_req rather than extending it,
    # so the two draws moved out of the item above have to be made there as
    # well. Without this, removing c_new_seed_dist leaves new_seed with no
    # constraint at all in ibex_icache_back_line: measured on seed 123, all 918
    # items drew a nonzero seed where the distribution's weight for an enabled,
    # non-invalidating item is zero, so the scoreboard ended the run holding 919
    # seeds and a fetch counted as correct if it matched any of them. num_insns
    # went the same way, uniform over [0:5] rather than half of it zero.
    #
    # The weights here are the item's own dist restricted to `num_insns <= 5`:
    # weight 5 on zero and weight 1 on each of 1 to 5, because `[1:20] :/ 20`
    # spreads its weight over twenty values.
    (
        ICACHE / "dv/ibex_icache_core_agent/seq_lib/ibex_icache_core_back_line_seq.sv",
        """  protected virtual task run_req(ibex_icache_core_req_item req, ibex_icache_core_rsp_item rsp);
    bit [31:0] min_addr, max_addr;

    start_item(req);
""",
        """  protected virtual task run_req(ibex_icache_core_req_item req, ibex_icache_core_rsp_item rsp);
    bit [31:0] min_addr, max_addr;

    // c_num_insns_dist and c_new_seed_dist, which this sequence would
    // otherwise lose along with the base sequence's run_req. See build_tb.py.
    int unsigned d_num_insns;
    bit [31:0]   d_new_seed;
    int unsigned pick;

    pick = $urandom_range(9, 0);
    d_num_insns = (pick < 5) ? 0 : (pick - 4);

    // enable is pinned to 1 below and invalidate to must_invalidate, so the
    // weight on a nonzero seed is 1000 with an invalidation and 0 without one.
    if (!must_invalidate || $urandom_range(1000, 0) == 0) begin
      d_new_seed = 32'd0;
    end else begin
      do d_new_seed = $urandom(); while (d_new_seed == 32'd0);
    end

    start_item(req);
""",
    ),
    (
        ICACHE / "dv/ibex_icache_core_agent/seq_lib/ibex_icache_core_back_line_seq.sv",
        """       // Ask for at most 5 insns in either phase (in the first phase, this means we have a chance
       // of jumping back when the cache isn't ready yet).
       num_insns <= 5;
""",
        """       // Ask for at most 5 insns in either phase (in the first phase, this means we have a chance
       // of jumping back when the cache isn't ready yet). Drawn above rather
       // than solved; see build_tb.py.
       num_insns == d_num_insns;

       new_seed == d_new_seed;
""",
    ),
    # A `solve ... before ...` anywhere in a class stops every `soft`
    # constraint in that class from being honoured. Reduced case, three classes
    # differing only in that one line:
    #
    #     soft + plain second var : broken 0/200
    #     soft + if/else          : broken 0/200
    #     soft + solve before     : broken 197/200
    #
    # push_pull_agent_cfg is written in exactly that shape: four `soft
    # x_min == 0` constraints and four `solve zero_delays before x_max`. The
    # minima come out as arbitrary 32-bit values, and the sequence then asks
    # for `delay inside {[min:max]}` on an empty range and dies with
    # "Randomization failed" part way into every run.
    #
    # Nothing in this testbench overrides those four defaults, so making them
    # hard is what upstream gets on a simulator that honours soft. Removing the
    # `solve` instead would change which values the maxima take.
    (
        LOWRISC_IP / "dv/sv/push_pull_agent/push_pull_agent_cfg.sv",
        "    soft host_delay_min == 0;\n",
        "    host_delay_min == 0;  // hard: see build_tb.py on soft and solve\n",
    ),
    (
        LOWRISC_IP / "dv/sv/push_pull_agent/push_pull_agent_cfg.sv",
        "    soft device_delay_min == 0;\n",
        "    device_delay_min == 0;  // hard: see build_tb.py\n",
    ),
    (
        LOWRISC_IP / "dv/sv/push_pull_agent/push_pull_agent_cfg.sv",
        "    soft req_lo_delay_min == 0;\n",
        "    req_lo_delay_min == 0;  // hard: see build_tb.py\n",
    ),
    (
        LOWRISC_IP / "dv/sv/push_pull_agent/push_pull_agent_cfg.sv",
        "    soft ack_lo_delay_min == 0;\n",
        "    ack_lo_delay_min == 0;  // hard: see build_tb.py\n",
    ),
    # The same interaction reaches the core request item, where run_req now
    # adds a top-level soft for the num_insns bucket. The `solve` exists to
    # stop branch transactions being weighted 2^32 times higher than the
    # others, and that cannot happen any more: run_req draws trans_type itself
    # and pins it, so the solver never chooses between the two.
    (
        ICACHE / "dv/ibex_icache_core_agent/ibex_icache_core_req_item.sv",
        "    // Pick trans_type before the other values. We need to do this"
        " because constraining branch_addr\n"
        "    // to 0 for non-branch transactions would otherwise mean branch"
        " transactions got weighted 2^32\n"
        "    // times higher.\n"
        "    solve trans_type before branch_addr;\n",
        "    // The `solve trans_type before branch_addr` that was here is\n"
        "    // dropped. It stopped branch transactions being weighted 2^32\n"
        "    // times higher, which cannot happen now that run_req draws\n"
        "    // trans_type itself, and a solve in the class would stop the\n"
        "    // soft num_insns bucket being honoured. See build_tb.py.\n",
    ),
    # std::randomize ignores dist weights, so these four sites get the same
    # buckets drawn directly. The first is the worst of them: the shape upstream
    # asks for puts about 3.5% of transactions in the 1000..1200 cycle bucket,
    # and a uniform pick over the support puts 76% of them there. Measured in
    # shims/verilator_dist_std_randomize.sv.
    (
        ICACHE / "dv/ibex_icache_core_agent/ibex_icache_core_driver.sv",
        "    `DV_CHECK_STD_RANDOMIZE_WITH_FATAL(req_low_cycles,\n"
        "                                       req_low_cycles dist {\n"
        "                                           0           :/"
        " (allow_no_low_cycles ? 20 : 0),\n"
        "                                           [1:33]      :/ 5,\n"
        "                                           [100:200]   :/ 2,\n"
        "                                           [1000:1200] :/ 1 };)\n",
        "    // Same four buckets and weights as the dist this replaces, drawn\n"
        "    // directly. See build_tb.py.\n"
        "    begin\n"
        "      int unsigned w_zero, total, pick;\n"
        "      w_zero = allow_no_low_cycles ? 20 : 0;\n"
        "      total  = w_zero + 5 + 2 + 1;\n"
        "      pick   = $urandom_range(total - 1, 0);\n"
        "      if (pick < w_zero)          req_low_cycles = 0;\n"
        "      else if (pick < w_zero + 5) req_low_cycles ="
        " $urandom_range(33, 1);\n"
        "      else if (pick < w_zero + 7) req_low_cycles ="
        " $urandom_range(200, 100);\n"
        "      else                        req_low_cycles ="
        " $urandom_range(1200, 1000);\n"
        "    end\n",
    ),
    (
        ICACHE / "dv/ibex_icache_core_agent/ibex_icache_core_driver.sv",
        "    `DV_CHECK_STD_RANDOMIZE_WITH_FATAL(num_cycles,\n"
        "                                       num_cycles dist { 1 :/ 10,"
        " [2:20] :/ 1 };)\n",
        "    // dist { 1 :/ 10, [2:20] :/ 1 }, drawn directly.\n"
        "    num_cycles = ($urandom_range(10, 0) < 10) ? 1 :"
        " $urandom_range(20, 2);\n",
    ),
    (
        ICACHE / "dv/ibex_icache_mem_agent/ibex_icache_mem_driver.sv",
        "      `DV_CHECK_STD_RANDOMIZE_WITH_FATAL(gnt_delay,\n"
        "                                         gnt_delay dist {\n"
        "                                           min_grant_delay          "
        "               :/ 3,\n"
        "                                           [min_grant_delay+1 :"
        " max_grant_delay-1] :/ 1,\n"
        "                                           max_grant_delay          "
        "               :/ 1\n"
        "                                         };)\n",
        "      // Same three buckets and weights, drawn directly. The middle\n"
        "      // bucket is empty when the two bounds are adjacent, and its\n"
        "      // weight goes with it. See build_tb.py.\n"
        "      begin\n"
        "        int unsigned w_mid, total, pick;\n"
        "        w_mid = (max_grant_delay >= min_grant_delay + 2) ? 1 : 0;\n"
        "        total = 3 + w_mid + 1;\n"
        "        pick  = $urandom_range(total - 1, 0);\n"
        "        if (pick < 3)             gnt_delay = min_grant_delay;\n"
        "        else if (pick < 3 + w_mid) gnt_delay ="
        " $urandom_range(max_grant_delay - 1, min_grant_delay + 1);\n"
        "        else                      gnt_delay = max_grant_delay;\n"
        "      end\n",
    ),
    (
        ICACHE / "dv/env/seq_lib/ibex_icache_combo_vseq.sv",
        "      `DV_CHECK_STD_RANDOMIZE_WITH_FATAL(cycles_till_reset,\n"
        "                                         cycles_till_reset dist {\n"
        "                                           [100:500]  :/ 1,\n"
        "                                           [501:1000] :/ 4\n"
        "                                         };)\n",
        "      // dist { [100:500] :/ 1, [501:1000] :/ 4 }, drawn directly.\n"
        "      cycles_till_reset = ($urandom_range(4, 0) == 0) ?\n"
        "                            $urandom_range(500, 100) :"
        " $urandom_range(1000, 501);\n",
    ),
    # The ECC error rate is a std::randomize dist with a non-constant weight,
    # so it comes out 50/50 rather than dis_err_pct. dis_err_pct defaults to 99,
    # meaning one line in a hundred should get an error; the uniform pick makes
    # it one in two, which is the difference between ibex_icache_ecc testing
    # error handling and testing nothing else.
    (
        ICACHE / "dv/env/ibex_icache_ram_if.sv",
        "  `DV_CHECK_STD_RANDOMIZE_WITH_FATAL(tag_sel_line, tag_sel_line dist"
        " {'b0 :/ dis_err_pct,\n"
        "                                      [1:$] :/ 100 - dis_err_pct};, ,"
        "\"ibex_icache_ram_if\")\n",
        "  // dist { 0 :/ dis_err_pct, [1:$] :/ 100 - dis_err_pct }, drawn\n"
        "  // directly: this simulator ignores dist weights under\n"
        "  // std::randomize. See build_tb.py.\n"
        "  tag_sel_line = ($urandom_range(99, 0) < dis_err_pct) ? '0 :\n"
        "                   $urandom_range({IC_NUM_WAYS{1'b1}}, 1);\n",
    ),
    (
        ICACHE / "dv/env/ibex_icache_ram_if.sv",
        "  `DV_CHECK_STD_RANDOMIZE_WITH_FATAL(data_sel_line, data_sel_line dist"
        " {'b0 :/ dis_err_pct,\n"
        "                                     [1:$] :/ 100 - dis_err_pct};, ,"
        "\"ibex_icache_ram_if\")\n",
        "  // Same as gen_tag_err above.\n"
        "  data_sel_line = ($urandom_range(99, 0) < dis_err_pct) ? '0 :\n"
        "                    $urandom_range({IC_NUM_WAYS{1'b1}}, 1);\n",
    ),
    # The memory response delay is drawn once per bus transaction, and
    # Verilator solves a constrained randomize() by piping to z3, so it is a
    # round trip per response. It is also the only rand field on the item, so
    # dropping `rand` takes the solver off the response path completely --
    # Verilator makes no solver call at all for a randomize() with nothing to
    # solve. Same three buckets and the same weights, and the `!is_grant`
    # branch of c_no_delay_for_req is folded into the draw.
    #
    # This is the lesson ports/core_ibex_uvm ends on: a constrained solve per
    # transaction is affordable on a commercial simulator and is a pipe round
    # trip here.
    (
        ICACHE / "dv/ibex_icache_mem_agent/ibex_icache_mem_resp_item.sv",
        "  rand int unsigned delay;\n",
        "  // Not rand: drawn in pre_randomize below. See build_tb.py.\n"
        "  int unsigned      delay;\n",
    ),
    (
        ICACHE / "dv/ibex_icache_mem_agent/ibex_icache_mem_resp_item.sv",
        """  constraint c_delay_dist {
    delay dist {
      min_response_delay                        :/ 5,
      [min_response_delay+1:mid_response_delay] :/ 5,
      [mid_response_delay+1:max_response_delay] :/ 1
    };
  }

  // The delay field has no effect for requests (i.e. if is_grant is false). Force it to zero rather
  // than leave mysterious numbers in the logs.
  constraint c_no_delay_for_req {
    (!is_grant) -> delay == 0;
  }
""",
        """  // c_delay_dist and c_no_delay_for_req, drawn rather than solved. The
  // delay field has no effect for requests (i.e. if is_grant is false), so it
  // is forced to zero rather than left as a mysterious number in the logs.
  function void pre_randomize();
    super.pre_randomize();
    delay = is_grant ? draw_delay() : 0;
  endfunction

  function int unsigned draw_delay();
    int unsigned pick = $urandom_range(10, 0);
    if (pick < 5) return min_response_delay;
    if (pick < 10) begin
      if (mid_response_delay < min_response_delay + 1) return min_response_delay;
      return $urandom_range(mid_response_delay, min_response_delay + 1);
    end
    if (max_response_delay < mid_response_delay + 1) return mid_response_delay;
    return $urandom_range(max_response_delay, mid_response_delay + 1);
  endfunction
""",
    ),
    # The ECC error masks are the one place in this testbench where the solver
    # is the run time. gen_tag_err and gen_data_err are called on every negedge
    # on which every way's rvalid is set, and each one asks the solver for a
    # `$countones(mask) inside {[1:2]}` over a 34-bit tag or a 78-bit line,
    # once per way. Verilator solves by piping to z3, so that is four round
    # trips per cycle. Measured: ibex_icache_ecc spent 18 minutes at 1 second
    # of CPU and 41,000 voluntary context switches, blocked on the pipe, and
    # had not finished. ibex_icache_caching, the sequence it derives from,
    # takes 48 seconds.
    #
    # Drawing the mask directly gives the same distribution. The constraint
    # admits N one-bit masks and N*(N-1)/2 two-bit masks, all equally likely,
    # so picking uniformly between those two populations and then uniformly
    # within one reproduces it exactly.
    (
        ICACHE / "dv/env/ibex_icache_ram_if.sv",
        "    `DV_CHECK_STD_RANDOMIZE_WITH_FATAL(tag_mask, $countones(tag_mask)"
        " inside {[1:2]};, ,\n"
        "                                       \"ibex_icache_ram_if\")\n",
        "    // One or two bits set, uniformly over every such mask, drawn\n"
        "    // directly rather than through the solver. See build_tb.py.\n"
        "    tag_mask = draw_sparse_mask_tag();\n",
    ),
    (
        ICACHE / "dv/env/ibex_icache_ram_if.sv",
        "    `DV_CHECK_STD_RANDOMIZE_WITH_FATAL(data_mask, $countones(data_mask)"
        " inside {[1:2]};, ,\n"
        "                                       \"ibex_icache_ram_if\")\n",
        "    // As gen_tag_err above.\n"
        "    data_mask = draw_sparse_mask_data();\n",
    ),
    (
        ICACHE / "dv/env/ibex_icache_ram_if.sv",
        "function automatic gen_tag_err();\n",
        "// A mask with one or two bits set, uniform over all such masks. This is\n"
        "// what `$countones(mask) inside {[1:2]}` asks for; see build_tb.py for\n"
        "// why it is not asked of the solver.\n"
        "function automatic bit [TagSizeECC-1:0] draw_sparse_mask_tag();\n"
        "  bit [TagSizeECC-1:0] mask = '0;\n"
        "  int unsigned n = TagSizeECC;\n"
        "  int unsigned singles = n;\n"
        "  int unsigned pairs   = n * (n - 1) / 2;\n"
        "  int unsigned first, second;\n"
        "  first = $urandom_range(n - 1, 0);\n"
        "  mask[first] = 1'b1;\n"
        "  if ($urandom_range(singles + pairs - 1, 0) >= singles) begin\n"
        "    do second = $urandom_range(n - 1, 0); while (second == first);\n"
        "    mask[second] = 1'b1;\n"
        "  end\n"
        "  return mask;\n"
        "endfunction\n"
        "\n"
        "function automatic bit [LineSizeECC-1:0] draw_sparse_mask_data();\n"
        "  bit [LineSizeECC-1:0] mask = '0;\n"
        "  int unsigned n = LineSizeECC;\n"
        "  int unsigned singles = n;\n"
        "  int unsigned pairs   = n * (n - 1) / 2;\n"
        "  int unsigned first, second;\n"
        "  first = $urandom_range(n - 1, 0);\n"
        "  mask[first] = 1'b1;\n"
        "  if ($urandom_range(singles + pairs - 1, 0) >= singles) begin\n"
        "    do second = $urandom_range(n - 1, 0); while (second == first);\n"
        "    mask[second] = 1'b1;\n"
        "  end\n"
        "  return mask;\n"
        "endfunction\n"
        "\n"
        "function automatic gen_tag_err();\n",
    ),
    # lowRISC's shared DV library excludes itself under Verilator:
    # clk_rst_if.sv wraps its UVM includes and imports in `ifndef VERILATOR, so
    # the interface compiles without `DV_CHECK_FATAL and fails at the first use
    # of it. The guard dates from when no open simulator could elaborate UVM.
    (
        LOWRISC_IP / "dv/sv/common_ifs/clk_rst_if.sv",
        "`ifndef VERILATOR\n"
        "  // include macros and import pkgs\n"
        "  `include \"dv_macros.svh\"\n"
        "  `include \"uvm_macros.svh\"\n"
        "  import uvm_pkg::*;\n"
        "  import common_ifs_pkg::*;\n"
        "`endif\n",
        "  // include macros and import pkgs\n"
        "  `include \"dv_macros.svh\"\n"
        "  `include \"uvm_macros.svh\"\n"
        "  import uvm_pkg::*;\n"
        "  import common_ifs_pkg::*;\n",
    ),
    # clk_rst_if declares `logic o_rst_n;` with no initialiser and then
    # apply_reset() drives it low. On a 4-state simulator that is X -> 0, a
    # real falling edge, so every `always_ff @(posedge clk or negedge rst_ni)`
    # takes its reset branch. Verilator zero-initialises it, the same
    # assignment is 0 -> 0, there is no edge, and the asynchronous resets never
    # fire while rst_n sits at 0 looking perfectly asserted.
    #
    # Found and diagnosed in ports/core_ibex_uvm; the same file is in this
    # build and the same fix applies. Starting it deasserted gives apply_reset
    # the 1 -> 0 edge it assumes.
    (
        LOWRISC_IP / "dv/sv/common_ifs/clk_rst_if.sv",
        "  logic o_rst_n;\n",
        "  logic o_rst_n = 1'b1;\n",
    ),
    # IEEE 1800 13.2.2 makes it illegal to write an automatic task's output
    # argument after a timing control, and csr_rd_sub and mem_rd_sub in
    # csr_utils_pkg.sv do exactly that: they hand their own output formals to
    # uvm_reg::read inside a `fork ... join_any; disable fork`. Verilator
    # enforces the rule and the commercial simulators do not.
    #
    # The fix writes automatic locals inside the fork and copies them out once
    # it has joined, which is what the code already means: the outer `join`
    # guarantees the fork has finished before the task returns.
    #
    # csr_utils_pkg.sv is in this compile only because dv_lib_pkg imports it.
    # Nothing in dv/uvm/icache names either task.
    (
        LOWRISC_IP / "dv/sv/csr_utils/csr_utils_pkg.sv",
        "    if (backdoor) begin\n"
        "      value = csr_peek(ptr, check);\n"
        "      status = UVM_IS_OK;\n"
        "      return;\n"
        "    end\n",
        "    uvm_reg_data_t value_l;\n"
        "    uvm_status_e   status_l;\n"
        "    if (backdoor) begin\n"
        "      value = csr_peek(ptr, check);\n"
        "      status = UVM_IS_OK;\n"
        "      return;\n"
        "    end\n",
    ),
    (
        LOWRISC_IP / "dv/sv/csr_utils/csr_utils_pkg.sv",
        "              csr_or_fld.field.read(.status(status), .value(value),"
        " .path(path), .map(map),\n"
        "                                    .prior(100));\n"
        "            end else begin\n"
        "              csr_or_fld.csr.read(.status(status), .value(value),"
        " .path(path), .map(map),\n"
        "                                  .prior(100));\n",
        "              csr_or_fld.field.read(.status(status_l), .value(value_l),"
        " .path(path), .map(map),\n"
        "                                    .prior(100));\n"
        "            end else begin\n"
        "              csr_or_fld.csr.read(.status(status_l), .value(value_l),"
        " .path(path), .map(map),\n"
        "                                  .prior(100));\n",
    ),
    (
        LOWRISC_IP / "dv/sv/csr_utils/csr_utils_pkg.sv",
        "              `DV_CHECK_EQ(status, UVM_IS_OK,\n"
        "                           $sformatf(\"trying to read csr/field %0s\","
        " ptr.get_full_name()),\n",
        "              `DV_CHECK_EQ(status_l, UVM_IS_OK,\n"
        "                           $sformatf(\"trying to read csr/field %0s\","
        " ptr.get_full_name()),\n",
    ),
    (
        LOWRISC_IP / "dv/sv/csr_utils/csr_utils_pkg.sv",
        "      end : isolation_fork\n"
        "    join\n"
        "  endtask\n"
        "\n"
        "  // backdoor read csr\n",
        "      end : isolation_fork\n"
        "    join\n"
        "    value  = value_l;\n"
        "    status = status_l;\n"
        "  endtask\n"
        "\n"
        "  // backdoor read csr\n",
    ),
    (
        LOWRISC_IP / "dv/sv/csr_utils/csr_utils_pkg.sv",
        "                            input  uvm_reg_frontdoor  user_ftdr ="
        " default_user_frontdoor);\n"
        "    fork\n"
        "      begin : isolating_fork\n"
        "        uvm_status_e status;\n",
        "                            input  uvm_reg_frontdoor  user_ftdr ="
        " default_user_frontdoor);\n"
        "    bit [31:0] data_l;\n"
        "    fork\n"
        "      begin : isolating_fork\n"
        "        uvm_status_e status;\n",
    ),
    (
        LOWRISC_IP / "dv/sv/csr_utils/csr_utils_pkg.sv",
        "            ptr.read(.status(status), .offset(offset), .value(data),"
        " .map(map), .prior(100));\n",
        "            ptr.read(.status(status), .offset(offset), .value(data_l),"
        " .map(map), .prior(100));\n",
    ),
    (
        LOWRISC_IP / "dv/sv/csr_utils/csr_utils_pkg.sv",
        "      end : isolating_fork\n"
        "    join\n"
        "  endtask : mem_rd_sub\n",
        "      end : isolating_fork\n"
        "    join\n"
        "    data = data_l;\n"
        "  endtask : mem_rd_sub\n",
    ),
]


def apply_overlays() -> dict[str, str]:
    """Write the patched copies and return upstream path -> overlay path."""
    out = BUILD / "overlay"
    if out.is_dir():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    patched: dict[Path, str] = {}
    for source, old, new in OVERLAYS:
        if not source.is_file():
            raise BuildError(f"overlay target missing: {source}")
        text = patched.get(source, source.read_text(encoding="utf-8"))
        if text.count(old) != 1:
            raise BuildError(
                f"{source.name}: the text this build patches is no longer "
                f"present exactly once; upstream has changed and the overlay "
                f"in build_tb.py needs revisiting")
        patched[source] = text.replace(old, new)

    mapping: dict[str, str] = {}
    for source, text in patched.items():
        target = out / source.name
        if target.exists():
            raise BuildError(f"two overlays share the basename {source.name}")
        target.write_text(text, encoding="utf-8")
        mapping[str(source)] = str(target)
    return mapping


# ---------------------------------------------------------------------------
# The Verilator command
# ---------------------------------------------------------------------------

def verilator_command(jobs: int, extra: list[str]) -> tuple[list[str], str]:
    if not UVM.is_dir():
        raise BuildError(f"no UVM at {UVM}\n"
                         f"run: python3 {ROOT / 'fetch.py'} uvm_core")

    resolved = fusesoc_setup.resolve(BUILD / "fusesoc")
    sources = resolved["sources"]
    incdirs = resolved["incdirs"]
    control = resolved["control"]
    top = resolved["toplevel"]

    overlay = apply_overlays()
    sources = [Path(overlay.get(str(path), str(path))) for path in sources]

    command = [
        "verilator", "--binary", "--timing",
        # UVM's plusarg list comes from vpi_get_vlog_info, so without --vpi
        # uvm_cmdline_processor sees no arguments and +UVM_TESTNAME never
        # reaches run_test(). See shims/uvm_dpi_verilator.cc.
        "--vpi",
        # Verilator makes warnings fatal by default. This compile raises a few
        # thousand, nearly all WIDTHTRUNC and WIDTHEXPAND out of UVM's own
        # source and lowRISC's DV library. They stay in the log.
        "-Wno-fatal",
        # Verilator's skip-identical check hashes the command line and the
        # files it read last time, and notices neither when a source moves into
        # build/overlay/ -- an include path is not on the command line and the
        # upstream file has not changed. An overlay then silently has no
        # effect. Diagnosed in ports/core_ibex_uvm; the cost is a full
        # re-verilation per build.
        "--no-skip-identical",
        "-j", str(jobs),
        "--top-module", top,
        # common_sim_cfg.hjson says 1ns/1ps. Verilator's minimum time precision
        # here is set the same.
        "--timescale", "1ns/1ps",
        "--Mdir", str(OBJ),
        "-o", "ibex_icache_tb",
        # The overlay directory first, so a patched `include wins over the
        # upstream copy. Only files this build patches are in it.
        f"+incdir+{BUILD / 'overlay'}",
        f"+incdir+{UVM}",
    ]
    command += [f"+incdir+{path}" for path in incdirs]
    command += [f"+define+{key}={value}" if value else f"+define+{key}"
                for key, value in COMMON_DEFINES.items()]
    command += [str(path) for path in control]
    command += [str(UVM / "uvm_pkg.sv")]
    command += [str(path) for path in sources]
    command += [str(HERE / "shims" / "uvm_dpi_verilator.cc")]
    command += ["-CFLAGS", shlex.join([f"-I{UVM}/dpi"])]
    command += extra
    return command, top


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    # Half the cores, not all of them: compiling UVM together with the design
    # peaks well over a gigabyte per job, and oversubscribing gets the compiler
    # killed part way through with "fatal error: Killed signal terminated
    # program cc1plus", which reads like a compiler bug rather than OOM.
    parser.add_argument("--jobs", type=int,
                        default=max(1, (os.cpu_count() or 4) // 2))
    parser.add_argument("--show", action="store_true",
                        help="print the command without running it")
    parser.add_argument("--list-sources", action="store_true",
                        help="print the resolved file list and stop")
    parser.add_argument("extra", nargs="*",
                        help="further options passed to Verilator")
    args = parser.parse_args(argv)

    if args.list_sources:
        try:
            resolved = fusesoc_setup.resolve(BUILD / "fusesoc")
        except fusesoc_setup.SetupError as error:
            print(f"build_tb: {error}", file=sys.stderr)
            return 1
        print(f"toplevel {resolved['toplevel']}")
        for kind in ("sources", "incdirs", "control"):
            for path in resolved[kind]:
                print(f"{kind:8s} {path.relative_to(IBEX)}")
        return 0

    BUILD.mkdir(parents=True, exist_ok=True)
    try:
        command, top = verilator_command(args.jobs, args.extra)
    except (BuildError, fusesoc_setup.SetupError) as error:
        print(f"build_tb: {error}", file=sys.stderr)
        return 1

    if args.show:
        print(shlex.join(command))
        return 0

    log = BUILD / "compile_tb.log"
    # The command is a page wide with a hundred absolute paths in it. Keeping
    # it beside the log rather than at the top of it leaves the log greppable.
    (BUILD / "compile_tb.cmd").write_text(shlex.join(command) + "\n",
                                          encoding="utf-8")
    print(f"build_tb: top module {top}, logging to {log.relative_to(HERE)}")
    with log.open("w", encoding="utf-8") as handle:
        completed = subprocess.run(command, cwd=BUILD, stdout=handle,
                                   stderr=subprocess.STDOUT, check=False)
    if completed.returncode != 0:
        print(f"build_tb: failed; see {log}", file=sys.stderr)
        return completed.returncode
    print(f"build_tb: built {OBJ / 'ibex_icache_tb'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
