#!/usr/bin/env python3
"""Build Ibex's UVM testbench (dv/uvm/core_ibex) with Verilator.

Upstream's `dv/uvm/core_ibex/yaml/rtl_simulation.yaml` names six simulators and
Verilator is not one of them, because until recently no open simulator could
elaborate UVM. Verilator 5 can: classes, constrained randomisation, covergroups
and `--timing` are all present. This builds the same testbench, from the same
file lists, with the same defines, using Verilator instead.

    python3 build_tb.py --config small
    python3 build_tb.py --config opentitan --jobs 16

Each configuration is built into its own `build/obj_<config>`, so more than one
can be held at a time and a test can be run against the configuration its
testlist entry asks for. `build/overlay` is shared, because nothing in it
depends on the configuration; the `--debug-mem` and `--pin-delays` variants
rewrite it, so a configuration built before one of those flags was used is not
the same build as one built after.

The sources come from upstream's own `ibex_dv.f`, expanded here rather than
transcribed, so a change upstream is a change in what gets built rather than a
silent divergence. The parameters come from `util/ibex_config.py`, the tool
Ibex's own flow calls, translated from `-pvalue+` to Verilator's `-G`.

Standard library only, matching the other tools here.
"""

from __future__ import annotations

import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
IBEX = ROOT / "deps" / "ibex"
LOWRISC_IP = IBEX / "vendor" / "lowrisc_ip"
CORE_IBEX = IBEX / "dv" / "uvm" / "core_ibex"
UVM = ROOT / "deps" / "uvm_core" / "src"
SPIKE = ROOT / "deps" / "spike_cosim" / "install"
BUILD = HERE / "build"

TOP = "core_ibex_tb_top"


def obj_dir(config: str, variant: str = "") -> Path:
    """Where a configuration's model and binary live.

    One directory per configuration, because the configurations are not
    interchangeable: `small` sets PMPEnable=0 and SecureIbex=0, so the whole
    directed test list and the five integrity classes are meaningless on it.
    A single build/obj meant proving anything about those needed a rebuild
    first, and the rebuild silently invalidated whatever had been measured
    before it.

    `variant` extends that to builds of the same configuration whose testbench
    differs -- currently only --no-irq-agent. Without it a measurement build
    overwrites the binary the recorded results were produced with, which is the
    same mistake one directory per configuration exists to prevent.
    """
    return BUILD / (f"obj_{config}-{variant}" if variant else f"obj_{config}")

# Verilator exposes a signal to VPI only when it is marked public, and accepts a
# write only when it is public_flat_rw. The integrity and glitch tests reach
# into the DUT by name with uvm_hdl_read/uvm_hdl_force/uvm_hdl_release, so every
# path they build is listed in this control file; without it vpi_handle_by_name
# returns null and shims/uvm_dpi_verilator.cc reports "unable to locate hdl
# path". See that file for the list and for which signals also need `forceable`.
HDL_PUBLIC_VLT = HERE / "shims" / "uvm_hdl_public.vlt"

# compile_tb.py hardcodes these alongside the config-derived parameters. The
# debug-module addresses have to agree with Spike, which takes them from
# DEBUG_ROM_ENTRY and DEBUG_ROM_TVEC at its own build time, so they are fixed
# on both sides rather than chosen here.
ADDRESS_DEFINES = {
    "DM_ADDR": "1A11_0000",
    "DM_ADDR_MASK": "0000_0FFF",
    "BOOT_ADDR": "8000_0000",
    "DEBUG_MODE_HALT_ADDR": "8000_0000",
    "DEBUG_MODE_EXCEPTION_ADDR": "8000_0008",
}

# Spike, through pkg-config, exactly as compile_tb.py locates it. riscv-fesvr is
# in upstream's list as a workaround for a CentOS 7 link failure and is left out
# here; nothing in the cosim sources references it.
SPIKE_PACKAGES = ["riscv-riscv", "riscv-disasm", "riscv-fdt"]

# Functional coverage is off by default because Verilator 5.050 does not
# survive compiling it. core_ibex_fcov_if.sv writes its FSM coverpoint with
# transition bins over enum items:
#
#     bins out_of_reset = (RESET => BOOT_SET);
#
# and Verilator stops with an internal error rather than an unsupported
# message: "AstNode is not of expected type, but instead has type
# 'ENUMITEMREF'". An internal error is a Verilator bug, not a missing feature,
# so this is a workaround pending a fix rather than a limitation to design
# around. See README.md.
#
# The covergroup is declared unconditionally; only its instantiation sits
# behind DV_FCOV_DISABLE_CP, which dv_fcov_macros.svh already sets under
# Verilator. Defining DV_FCOV_DISABLE is therefore not enough on its own: the
# files have to leave the compile as well.
FCOV_SOURCES = [
    "core_ibex_fcov_if.sv",
    "core_ibex_fcov_bind.sv",
    "core_ibex_pmp_fcov_if.sv",
]


# lowRISC's shared DV library excludes itself under Verilator: clk_rst_if.sv
# wraps its UVM includes and imports in `ifndef VERILATOR`, so on Verilator the
# interface compiles without `DV_CHECK_FATAL` and fails at the first use of it.
# The guard dates from when no open simulator could elaborate UVM. That is no
# longer true, so this build removes it.
#
# The edit is applied to a copy under build/overlay/ rather than to the fetched
# tree, and it is an exact-text replacement: if upstream changes those lines the
# build stops with a clear message instead of silently building something else.
#
# Two other `VERILATOR guards in the DV library are deliberately left alone:
#   pins_if.sv         picks a strength-free `assign`, which Verilator needs
#   dv_fcov_macros.svh sets DV_FCOV_DISABLE_CP, turning functional coverage off
# The second is the switch to flip when functional coverage is wanted.
#
# The second overlay is the interrupt agent's clocking block. `default output
# negedge` is an edge-based output skew, which Verilator does not implement: a
# write lands at the next negedge of the clocking event's clock. Moving the
# event to `@(negedge clk)` and taking the default `#0` skew lands the write at
# the same negedge, so the interrupt pins still change half a cycle away from
# the edge the core samples them on.
#
# The third overlay follows from a `VERILATOR guard in the RTL rather than the
# DV library: prim_assert.sv includes prim_assert_dummy_macros.svh under
# Verilator and, unlike every other branch, does not define INC_ASSERT. The
# signal `unused_assert_connected` in prim_count.sv only exists inside
# `ifdef INC_ASSERT, but core_ibex_tb_top.sv drives it unconditionally, so
# elaboration fails on a name that is not there. Guarding the assign with the
# same `ifdef is what upstream would have to do anyway; it changes nothing on a
# simulator that defines INC_ASSERT.
#
# The remaining overlays are one genuine LRM violation in lowRISC's shared DV
# library. IEEE 1800 13.2.2 makes it illegal to write an automatic task's
# output argument after a timing control, and `csr_rd_sub` and `mem_rd_sub` in
# csr_utils_pkg.sv do exactly that: they hand their own output formals to
# `uvm_reg::read` inside a `fork ... join_any; disable fork`. Verilator
# enforces the rule; the commercial simulators upstream uses do not.
#
# The fix keeps the outputs as outputs and writes automatic locals inside the
# fork instead, copying them out once it has joined. That is what the code
# already means -- the outer `join` guarantees the fork has finished before the
# task returns -- so it is a restatement rather than a behaviour change.
# Changing the formals to `ref` was tried first and Verilator rejects that too.
#
# csr_utils_pkg.sv is in the compile because push_pull_agent_pkg imports
# dv_lib_pkg which imports it, not because core_ibex uses it: nothing in
# dv/uvm/core_ibex names either task.
#
# What does move is `reset`, which the driver reads through the same clocking
# block: it is now sampled on the falling edge rather than the rising one.
# Reset is held for 100 cycles by core_ibex_tb_top, so half a cycle either way
# does not change which cycle the driver sees it in. The alternative -- two
# clocking blocks, one per edge -- would diverge further from upstream's source
# than this does.
# One line per posedge of clk: every input of the testbench wrapper
# ports/core_ibex_cpptb elaborates, and every output of it that the two
# harnesses can be compared on. ports/core_ibex_cpptb/replay.py drives the
# inputs at its own drive point, half a cycle earlier, and compares the outputs
# at the edge this reads them on -- which is the value the design samples at
# that edge, because an `always @(posedge clk)` executes in the Active region
# before the clocking blocks' non-blocking updates land.
#
# Two details are worth stating.
#
# The integrity bits are recorded as a pair of flags rather than as the seven
# bits themselves. ibex_mem_intf_response_seq computes them with
# prim_secded_inv_39_32_enc(data) and inverts them when it wants a bad-integrity
# response, and core_ibex_cpptb_tb_top computes them the same way from a
# `bad_intg` input, so what a replay needs is that one bit. `intg_known` says
# the recorded rintg really is the encoding or its inverse; a replay that sees
# it clear on a cycle with rvalid asserted reports that rather than driving
# something the wrapper cannot express.
#
# The key and nonce change rarely and are 192 bits between them, so they get a
# `K` line only when they change and a reader holds the last value.
RECORDER = r"""
  // ------------------------------------------------------------------
  // Pin recording for ports/core_ibex_cpptb/replay.py. Enabled by
  // +core_ibex_record=<prefix>, and does nothing without it.
  // ------------------------------------------------------------------
  string       cpptb_record_prefix;
  integer      cpptb_record_fd = 0;
  int unsigned cpptb_record_cycle = 0;
  int unsigned cpptb_record_max = 0;
  logic [ibex_pkg::SCRAMBLE_KEY_W-1:0]   cpptb_last_key;
  logic [ibex_pkg::SCRAMBLE_NONCE_W-1:0] cpptb_last_nonce;
  bit          cpptb_key_written = 1'b0;

  function automatic bit cpptb_bad_intg(logic [31:0] rdata, logic [6:0] rintg);
    logic [38:0] enc;
    enc = prim_secded_pkg::prim_secded_inv_39_32_enc(rdata);
    return (rintg === ~enc[38:32]);
  endfunction

  function automatic bit cpptb_intg_known(logic [31:0] rdata, logic [6:0] rintg);
    logic [38:0] enc;
    enc = prim_secded_pkg::prim_secded_inv_39_32_enc(rdata);
    return (rintg === enc[38:32]) || (rintg === ~enc[38:32]);
  endfunction

  initial begin
    if ($value$plusargs("core_ibex_record=%0s", cpptb_record_prefix)) begin
      void'($value$plusargs("core_ibex_record_max=%0d", cpptb_record_max));
      cpptb_record_fd = $fopen({cpptb_record_prefix, ".pins"}, "w");
      if (cpptb_record_fd == 0) begin
        $fatal(1, "cannot open the cpptb pin recording for writing");
      end
      $fwrite(cpptb_record_fd,
              "# core_ibex pin recording for ports/core_ibex_cpptb/replay.py\n");
      $fwrite(cpptb_record_fd,
              "# in  0 rst_n 1 instr_gnt 2 instr_rvalid 3 instr_err 4 instr_bad_intg\n");
      $fwrite(cpptb_record_fd,
              "#     5 data_gnt 6 data_rvalid 7 data_err 8 data_bad_intg 9 key_valid\n");
      $fwrite(cpptb_record_fd,
              "#     10 instr_intg_known 11 data_intg_known\n");
      $fwrite(cpptb_record_fd,
              "# ctl 8:5 fetch_enable 4:1 mcounteren_writable 0 debug_req\n");
      $fwrite(cpptb_record_fd,
              "# out 0 instr_req 1 data_req 2 data_we 3 rvfi_valid 4 rvfi_trap\n");
      $fwrite(cpptb_record_fd,
              "#     5 double_fault_seen 6 alert_minor 7 alert_major_internal\n");
      $fwrite(cpptb_record_fd,
              "#     8 alert_major_bus 9 core_sleep 10 key_req\n");
      $fwrite(cpptb_record_fd,
              "# K cycle key nonce\n");
      $fwrite(cpptb_record_fd,
              "# C cycle in ctl instr_rdata data_rdata out instr_addr data_addr data_be data_wdata rvfi_order rvfi_pc rvfi_rd rvfi_rd_wdata\n");
    end
  end

  always @(posedge clk) begin
    if (cpptb_record_fd != 0 &&
        (cpptb_record_max == 0 || cpptb_record_cycle < cpptb_record_max)) begin
      if (!cpptb_key_written || (scramble_key !== cpptb_last_key) ||
          (scramble_nonce !== cpptb_last_nonce)) begin
        $fwrite(cpptb_record_fd, "K %0d %032h %016h\n", cpptb_record_cycle,
                scramble_key, scramble_nonce);
        cpptb_last_key    = scramble_key;
        cpptb_last_nonce  = scramble_nonce;
        cpptb_key_written = 1'b1;
      end
      $fwrite(cpptb_record_fd,
              "C %0d %03h %03h %08h %08h %03h %08h %08h %1h %08h %016h %08h %02h %08h\n",
              cpptb_record_cycle,
              {cpptb_intg_known(data_mem_vif.rdata, data_mem_vif.rintg),
               cpptb_intg_known(instr_mem_vif.rdata, instr_mem_vif.rintg),
               scrambling_key_if.ack,
               cpptb_bad_intg(data_mem_vif.rdata, data_mem_vif.rintg),
               data_mem_vif.error, data_mem_vif.rvalid, data_mem_vif.grant,
               cpptb_bad_intg(instr_mem_vif.rdata, instr_mem_vif.rintg),
               instr_mem_vif.error, instr_mem_vif.rvalid, instr_mem_vif.grant,
               rst_n},
              {dut_if.fetch_enable, dut_if.mcounteren_writable,
               dut_if.debug_req},
              instr_mem_vif.rdata, data_mem_vif.rdata,
              {scrambling_key_if.req, dut_if.core_sleep,
               dut_if.alert_major_bus, dut_if.alert_major_internal,
               dut_if.alert_minor, dut_if.double_fault_seen,
               dut.rvfi_trap, dut.rvfi_valid,
               data_mem_vif.we, data_mem_vif.request, instr_mem_vif.request},
              instr_mem_vif.addr, data_mem_vif.addr, data_mem_vif.be,
              data_mem_vif.wdata, dut.rvfi_order, dut.rvfi_pc_rdata,
              dut.rvfi_rd_addr, dut.rvfi_rd_wdata);
      cpptb_record_cycle <= cpptb_record_cycle + 1;
    end
  end
endmodule
"""


OVERLAYS = [
    # A class-scope `dist` poisons every other randomize() on the object:
    # this simulator emits the distribution as a hard equality against a
    # freshly drawn sample and does not gate that on the randomize()
    # argument list, so `randomize(stimulus_delay_cycles)` is handed
    # `0 == 1` and goes UNSAT about half the time. Reduced case and the
    # captured SMT in shims/verilator_dist_on_state_var.sv.
    #
    # Drawing the value explicitly keeps the distribution and removes the
    # constraint. `soft` does not help, and moving the dist inline into
    # `randomize() with {}` is worse: the weights are silently dropped.
    # Both classes need it -- memory_error_seq extends core_base_new_seq,
    # so its own dist breaks the inherited drive_stimulus().
    (
        CORE_IBEX / 'tests/core_ibex_new_seq_lib.sv',
        '  constraint zero_delays_c {\n     zero_delays dist {1 :/ zero_delay_pct,\n                       0 :/ 100 - zero_delay_pct};\n  }\n',
        '  // The dist that was here is drawn explicitly in drive_stimulus().\n  // A class-scope `dist` is applied by this simulator as an equality\n  // against a freshly drawn sample on every randomize() of the object,\n  // even when its variable is not in the argument list, so leaving it\n  // here makes the next randomize(stimulus_delay_cycles) fail about half\n  // the time. See shims/verilator_dist_on_state_var.sv.\n',
    ),
    (
        CORE_IBEX / 'tests/core_ibex_new_seq_lib.sv',
        '    `DV_CHECK_MEMBER_RANDOMIZE_FATAL(zero_delays)\n',
        '    // Same distribution as the zero_delays_c constraint this replaces.\n    zero_delays = ($urandom_range(99, 0) < zero_delay_pct);\n',
    ),
    (
        CORE_IBEX / 'tests/core_ibex_new_seq_lib.sv',
        '  constraint inject_intg_err_c {\n     inject_intg_err dist {1 :/ intg_err_pct,\n                           0 :/ 100 - intg_err_pct};\n  }\n',
        '  // Drawn explicitly in send_req(); see the note on zero_delays_c above.\n  // This one also breaks the inherited drive_stimulus(), which randomizes\n  // zero_delays and stimulus_delay_cycles with inject_intg_err as state.\n',
    ),
    (
        CORE_IBEX / 'tests/core_ibex_new_seq_lib.sv',
        '    `DV_CHECK_MEMBER_RANDOMIZE_FATAL(inject_intg_err)\n',
        '    inject_intg_err = ($urandom_range(99, 0) < intg_err_pct);\n',
    ),
    # The memory agents randomize a delay on every bus transaction, and each
    # constrained solve is a round trip over a pipe to z3. Measured: the
    # process spends about 85% of its wall clock blocked rather than
    # computing, and removing this one solve roughly halved the run time.
    #
    # This keeps the distribution and drops the round trip: same buckets,
    # same weights, drawn with $urandom_range.
    (
        CORE_IBEX / "common/ibex_mem_intf_agent/ibex_mem_intf_response_driver.sv",
        '        if (!std::randomize(gnt_delay) with {\n          gnt_delay dist {\n            cfg.gnt_delay_min                           :/ 10,\n            [cfg.gnt_delay_min+1 : cfg.gnt_delay_max-1] :/ cfg.valid_pick_medium_speed_weight,\n            cfg.gnt_delay_max                           :/ cfg.valid_pick_slow_speed_weight\n          };\n        }) begin\n          `uvm_fatal(`gfn, $sformatf("Cannot randomize grant"))\n        end\n',
        '        // Same three buckets and the same weights as the dist this replaces,\n        // drawn with $urandom_range instead of the constraint solver. `:/`\n        // spreads a weight uniformly across its range, which is the second\n        // branch here. See build_tb.py for why.\n        begin\n          int unsigned w_lo, w_mid, w_hi, pick;\n          w_lo  = 10;\n          w_mid = (cfg.gnt_delay_max >= cfg.gnt_delay_min + 2) ?\n                    cfg.valid_pick_medium_speed_weight : 0;\n          w_hi  = cfg.valid_pick_slow_speed_weight;\n          pick  = $urandom_range(w_lo + w_mid + w_hi - 1, 0);\n          if (pick < w_lo) begin\n            gnt_delay = cfg.gnt_delay_min;\n          end else if (pick < w_lo + w_mid) begin\n            gnt_delay = $urandom_range(cfg.gnt_delay_max - 1, cfg.gnt_delay_min + 1);\n          end else begin\n            gnt_delay = cfg.gnt_delay_max;\n          end\n        end\n',
    ),
    # clk_rst_if declares `logic o_rst_n;` with no initialiser and then
    # apply_reset() drives it low. On a 4-state simulator that is X -> 0, a
    # real falling edge, and every `always_ff @(posedge clk or negedge rst_ni)`
    # in the design takes its reset branch. Verilator zero-initialises it, so
    # the same assignment is 0 -> 0, there is no edge, the asynchronous resets
    # never fire, and the whole design keeps its zero-initialised state while
    # rst_n sits at 0 looking perfectly asserted.
    #
    # The visible symptom is three levels away: priv_lvl_q resets to
    # PRIV_LVL_M (2'b11) but stays 2'b00, so the core boots in User mode,
    # `csrr t0, mhartid` is an illegal CSR access by privilege, and the first
    # instruction of every program traps.
    #
    # Starting it deasserted gives apply_reset the 1 -> 0 edge it assumes.
    # `+verilator+rand+reset+2` also works, by accident, and is not
    # deterministic.
    (
        LOWRISC_IP / "dv/sv/common_ifs/clk_rst_if.sv",
        "  logic o_rst_n;\n",
        "  logic o_rst_n = 1'b1;\n",
    ),
    # A variable named `item` referenced inside a `randomize() with {}`
    # block is silently ignored by Verilator: the constraint is dropped
    # and randomize() still returns success. Any other name works, and
    # local versus class member makes no difference -- see
    # shims/verilator_constraint_item_name.sv. `item` is SystemVerilog's
    # implicit iterator argument for array-manipulation `with` clauses,
    # which is the likely collision.
    #
    # This sequence ties its response to the monitored request with
    # `addr == item.addr`, so every response went to a random address and
    # the core was answered with c.unimp at its reset vector. Aliasing
    # the handle for the constraint block is the whole fix.
    (
        CORE_IBEX / "common/ibex_mem_intf_agent/ibex_mem_intf_response_seq_lib.sv",
        "      bit [INTG_WIDTH-1:0] read_intg;\n      bit                  data_was_uninitialized = 1'b0;\n",
        "      bit [INTG_WIDTH-1:0] read_intg;\n      bit                  data_was_uninitialized = 1'b0;\n      // `item` is the implicit iterator name for `with` clauses, and this\n      // simulator lets that shadow a user variable of the same name inside\n      // randomize() with {}, silently dropping the constraint. Any other\n      // name works, so the handle is aliased for the constraint block.\n      ibex_mem_intf_seq_item l_item;\n",
    ),
    (
        CORE_IBEX / "common/ibex_mem_intf_agent/ibex_mem_intf_response_seq_lib.sv",
        '      if (!req.randomize() with {\n        addr       == item.addr;\n        read_write == item.read_write;\n        data       == item.data;\n        intg       == item.intg;\n        be         == item.be;\n        if (p_sequencer.cfg.zero_delays) {\n          rvalid_delay == 0;\n        } else {\n          rvalid_delay dist {\n            p_sequencer.cfg.valid_delay_min                                                  :/ 5,\n            [p_sequencer.cfg.valid_delay_min + 1 : p_sequencer.cfg.valid_delay_max / 2 - 1]  :/ 3,\n            [p_sequencer.cfg.valid_delay_max / 2 : p_sequencer.cfg.valid_delay_max - 1]\n            :/ p_sequencer.cfg.valid_pick_medium_speed_weight,\n            p_sequencer.cfg.valid_delay_max\n            :/  p_sequencer.cfg.valid_pick_slow_speed_weight\n          };\n        }\n        error == enable_error;\n      }) begin\n',
        '      // Every field here is determined: five by equality to the monitored\n      // request, error by enable_error, and gnt_delay/req_delay are never\n      // read from the response item. Only rvalid_delay ever varied, and its\n      // four dist buckets are drawn below with the same weights. With\n      // nothing left to solve this simulator makes no solver call at all,\n      // which is the point: each one is a pipe round trip.\n      l_item = item;\n      req.addr.rand_mode(0);\n      req.read_write.rand_mode(0);\n      req.data.rand_mode(0);\n      req.intg.rand_mode(0);\n      req.be.rand_mode(0);\n      req.error.rand_mode(0);\n      req.gnt_delay.rand_mode(0);\n      req.req_delay.rand_mode(0);\n      req.rvalid_delay.rand_mode(0);\n      req.addr       = l_item.addr;\n      req.read_write = l_item.read_write;\n      req.data       = l_item.data;\n      req.intg       = l_item.intg;\n      req.be         = l_item.be;\n      req.error      = enable_error;\n      if (p_sequencer.cfg.zero_delays) begin\n        req.rvalid_delay = 0;\n      end else begin\n        int unsigned vmin, vmax, w0, w1, w2, w3, pick;\n        vmin = p_sequencer.cfg.valid_delay_min;\n        vmax = p_sequencer.cfg.valid_delay_max;\n        w0 = 5;\n        w1 = (vmax / 2 - 1 >= vmin + 1) ? 3 : 0;\n        w2 = (vmax - 1 >= vmax / 2) ?\n               p_sequencer.cfg.valid_pick_medium_speed_weight : 0;\n        w3 = p_sequencer.cfg.valid_pick_slow_speed_weight;\n        pick = $urandom_range(w0 + w1 + w2 + w3 - 1, 0);\n        if (pick < w0) req.rvalid_delay = vmin;\n        else if (pick < w0 + w1) req.rvalid_delay = $urandom_range(vmax / 2 - 1, vmin + 1);\n        else if (pick < w0 + w1 + w2) req.rvalid_delay = $urandom_range(vmax - 1, vmax / 2);\n        else req.rvalid_delay = vmax;\n      end\n      if (!req.randomize()) begin\n',
    ),
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
    (
        CORE_IBEX / "common/irq_agent/irq_if.sv",
        "  clocking driver_cb @(posedge clk);\n"
        "    default output negedge;\n",
        "  clocking driver_cb @(negedge clk);\n",
    ),
    (
        CORE_IBEX / "tb/core_ibex_tb_top.sv",
        "  if (SecureIbex && LockstepOffset > 1) begin : gen_disable_count_check\n"
        "    assign dut.u_ibex_top.gen_lockstep.u_ibex_lockstep."
        "gen_reset_counter.u_rst_shadow_cnt.\n"
        "          unused_assert_connected = 1;\n"
        "  end\n",
        "`ifdef INC_ASSERT\n"
        "  if (SecureIbex && LockstepOffset > 1) begin : gen_disable_count_check\n"
        "    assign dut.u_ibex_top.gen_lockstep.u_ibex_lockstep."
        "gen_reset_counter.u_rst_shadow_cnt.\n"
        "          unused_assert_connected = 1;\n"
        "  end\n"
        "`endif\n",
    ),
    # `DV_CREATE_SIGNAL_PROBE_FUNCTION builds a function with force and release
    # arms. core_ibex_dut_probe_if.sv instantiates it twelve times, which makes
    # those twelve interface signals forceable; Verilator represents a
    # forceable signal as VlForceVec and cannot build the virtual-interface
    # triggers for one, so every declaration is an error.
    #
    # Nothing in the Ibex tree ever calls a probe with SignalProbeForce or
    # SignalProbeRelease -- every call site passes SignalProbeSample -- so the
    # two arms are dead code that costs the whole interface. They are replaced
    # by a fatal rather than deleted, so a future caller finds out.
    (
        LOWRISC_IP / "dv/sv/dv_utils/dv_macros.svh",
        "      dv_utils_pkg::SignalProbeForce: force SIGNAL_PATH_ = value;"
        "                             \\\n"
        "      dv_utils_pkg::SignalProbeRelease: release SIGNAL_PATH_;"
        "                                 \\\n",
        "      dv_utils_pkg::SignalProbeForce,"
        "                                                            \\\n"
        "      dv_utils_pkg::SignalProbeRelease:"
        "                                                          \\\n"
        "        `uvm_fatal(`\"FUNC_NAME_`\", \"force and release are not"
        " available on Verilator\")   \\\n",
    ),
    # `event monitor_tick = null;` is a legal declaration initialiser, and the
    # default for an event variable is null anyway, but Verilator's generated
    # C++ has no `operator=(VlNull)` for VlAssignableEvent and fails to
    # compile. Dropping the initialiser leaves the same starting value.
    (
        CORE_IBEX / "common/ibex_mem_intf_agent/ibex_mem_intf_response_sequencer.sv",
        "  event monitor_tick = null;\n",
        "  event monitor_tick;\n",
    ),
    # `uvm_hdl_data_t` is 1024 bits wide, so indexing a queue with one of these
    # addresses hands Verilator's generated C++ a VlWide<32> where it wants an
    # int32_t. Narrowing at the index with a part select is what the code means -- an
    # icache set index is a handful of bits -- and it leaves the values
    # themselves alone.
    (
        CORE_IBEX / "tests/core_ibex_test_lib.sv",
        "          if (data_req[i]) data_valid[i][data_addr] = 1'b1;\n",
        "          if (data_req[i]) data_valid[i][data_addr[31:0]] = 1'b1;\n",
    ),
    (
        CORE_IBEX / "tests/core_ibex_test_lib.sv",
        "          if (tag_req[i]) tag_valid[i][tag_addr] = 1'b1;\n",
        "          if (tag_req[i]) tag_valid[i][tag_addr[31:0]] = 1'b1;\n",
    ),
    (
        CORE_IBEX / "tests/core_ibex_test_lib.sv",
        "          if (data_req[i] && data_valid[i][data_addr])"
        " valid_and_used_ways.push_back(i);\n",
        "          if (data_req[i] && data_valid[i][data_addr[31:0]])"
        " valid_and_used_ways.push_back(i);\n",
    ),
    (
        CORE_IBEX / "tests/core_ibex_test_lib.sv",
        "          if (tag_req[i] && tag_valid[i][tag_addr])"
        " valid_and_used_ways.push_back(i);\n",
        "          if (tag_req[i] && tag_valid[i][tag_addr[31:0]])"
        " valid_and_used_ways.push_back(i);\n",
    ),
    # $readmemh's second argument has to be a variable, and `mem.system_memory`
    # is a property of a class object. Verilator rejects it; the commercial
    # simulators accept it. This does by hand what $readmemh does for a
    # byte-wide associative array, so the loaded image is the same.
    (
        CORE_IBEX / "tests/core_ibex_base_test.sv",
        "  function void load_vmem_to_dut_mem(string vmem_file);\n"
        "    $readmemh(vmem_file, mem.system_memory);\n"
        "  endfunction\n",
        "  function void load_vmem_to_dut_mem(string vmem_file);\n"
        "    int        fd;\n"
        "    string     token;\n"
        "    bit [31:0] addr = 0;\n"
        "    fd = $fopen(vmem_file, \"r\");\n"
        "    if (!fd) begin\n"
        "      `uvm_fatal(get_full_name(),"
        " $sformatf(\"Cannot open file %0s\", vmem_file))\n"
        "    end\n"
        "    while ($fscanf(fd, \"%s\", token) == 1) begin\n"
        "      if (token.substr(0, 0) == \"@\") begin\n"
        "        addr = token.substr(1, token.len() - 1).atohex();\n"
        "      end else begin\n"
        "        mem.write_byte(addr, token.atohex());\n"
        "        addr++;\n"
        "      end\n"
        "    end\n"
        "    $fclose(fd);\n"
        "  endfunction\n",
    ),
    # csr_rd_sub: write locals inside the fork, copy out once it has joined.
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
    # mem_rd_sub: the same shape, one output.
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
    # The pin recording ports/core_ibex_cpptb/replay.py replays. It writes
    # nothing at all without +core_ibex_record=<prefix>, so this build is the
    # build the results above were measured with; see RECORDER below.
    (
        CORE_IBEX / "tb/core_ibex_tb_top.sv",
        "\nendmodule\n",
        RECORDER,
    ),
]


# Opt-in instrumentation, added by --debug-mem. Prints every memory response
# near the reset vector, which is what a run that reads zeros at 0x8000_0080
# needs and what neither the UVM log nor the tracer shows: the address the
# agent was asked for, the data it decided to return, and whether it found the
# address in the memory model at all.
# Bisect of the `with` block that loses `addr == item.addr`: five randomize
# calls on fresh items, each with a different subset of the real constraints,
# all in one run so one rebuild answers which term breaks it.
BISECT_ANCHOR = "      enable_error = 1'b0; // Disable after single inserted error.\n"
BISECT_SNIPPET = '\n      begin : bisect\n        ibex_mem_intf_seq_item b1, b2, b3, b4, b5;\n        bit ok1, ok2, ok3, ok4, ok5;\n        b1 = ibex_mem_intf_seq_item::type_id::create("b1");\n        ok1 = b1.randomize() with { addr == item.addr; };\n        b2 = ibex_mem_intf_seq_item::type_id::create("b2");\n        ok2 = b2.randomize() with { addr == item.addr; read_write == item.read_write;\n                    data == item.data; intg == item.intg; be == item.be; };\n        b3 = ibex_mem_intf_seq_item::type_id::create("b3");\n        ok3 = b3.randomize() with { addr == item.addr; read_write == item.read_write;\n                    data == item.data; intg == item.intg; be == item.be; error == enable_error; };\n        b4 = ibex_mem_intf_seq_item::type_id::create("b4");\n        ok4 = b4.randomize() with { addr == item.addr;\n                    if (p_sequencer.cfg.zero_delays) { rvalid_delay == 0; }\n                    else { rvalid_delay dist {\n                        p_sequencer.cfg.valid_delay_min :/ 5,\n                        [p_sequencer.cfg.valid_delay_min + 1 : p_sequencer.cfg.valid_delay_max / 2 - 1] :/ 3,\n                        [p_sequencer.cfg.valid_delay_max / 2 : p_sequencer.cfg.valid_delay_max - 1]\n                          :/ p_sequencer.cfg.valid_pick_medium_speed_weight,\n                        p_sequencer.cfg.valid_delay_max\n                          :/ p_sequencer.cfg.valid_pick_slow_speed_weight }; } };\n        b5 = ibex_mem_intf_seq_item::type_id::create("b5");\n        ok5 = b5.randomize() with { addr == item.addr; read_write == item.read_write;\n                    data == item.data; intg == item.intg; be == item.be;\n                    if (p_sequencer.cfg.zero_delays) { rvalid_delay == 0; }\n                    else { rvalid_delay dist {\n                        p_sequencer.cfg.valid_delay_min :/ 5,\n                        [p_sequencer.cfg.valid_delay_min + 1 : p_sequencer.cfg.valid_delay_max / 2 - 1] :/ 3,\n                        [p_sequencer.cfg.valid_delay_max / 2 : p_sequencer.cfg.valid_delay_max - 1]\n                          :/ p_sequencer.cfg.valid_pick_medium_speed_weight,\n                        p_sequencer.cfg.valid_delay_max\n                          :/ p_sequencer.cfg.valid_pick_slow_speed_weight }; }\n                    error == enable_error; };\n        $display("[BISECT] item.addr=%08h | eq=%0b/%08h eq5=%0b/%08h +err=%0b/%08h +dist=%0b/%08h full=%0b/%08h",\n                 item.addr, ok1, b1.addr, ok2, b2.addr, ok3, b3.addr,\n                 ok4, b4.addr, ok5, b5.addr);\n      end\n'

DEBUG_OVERLAYS = [
    (
        CORE_IBEX / "common/ibex_mem_intf_agent/ibex_mem_intf_response_seq_lib.sv",
        BISECT_ANCHOR,
        BISECT_ANCHOR + BISECT_SNIPPET,
    ),
    # Print, from inside the monitor, what the clocking block samples next to
    # what the raw interface wires carry. tb_top already shows the DUT and the
    # wires agreeing, so this is the last place the address can go wrong.
    (
        CORE_IBEX / "common/ibex_mem_intf_agent/ibex_mem_intf_monitor.sv",
        "      trans_collected.addr                       = vif.monitor_cb.addr;\n",
        "      $display(\"[MONDBG] t=%0t cb: addr=%08h req=%b gnt=%b |"
        " raw: addr=%08h req=%b gnt=%b\",\n"
        "               $time, vif.monitor_cb.addr, vif.monitor_cb.request,\n"
        "               vif.monitor_cb.grant, vif.addr, vif.request, vif.grant);\n"
        "      trans_collected.addr                       = vif.monitor_cb.addr;\n",
    ),
    # Print the instruction bus from both ends on the same cycle: what the DUT
    # drives on its ports, and what the interface wires the agent watches
    # actually carry. If those disagree, the port connection in tb_top is the
    # problem; if they agree, the agent is.
    (
        CORE_IBEX / "tb/core_ibex_tb_top.sv",
        "    end\n  end\nendmodule\n",
        "    end\n  end\n\n"
        "  int dbg_cycle = 0;\n"
        "  always @(posedge clk) begin\n"
        "    if (dbg_cycle < 60) begin\n"
        "      dbg_cycle <= dbg_cycle + 1;\n"
        "      $display(\"[TBDBG] t=%0t rst_n=%b priv=%h fe=%h | dut req=%b addr=%08h"
        " gnt=%b rvalid=%b rdata=%08h | vif req=%b addr=%08h"
        " gnt=%b rvalid=%b rdata=%08h\",\n"
        "               $time, rst_n,\n"
        "               dut.u_ibex_top.u_ibex_core.cs_registers_i.priv_lvl_q,\n"
        "               dut_if.fetch_enable,\n"
        "               dut.instr_req_o, dut.instr_addr_o, dut.instr_gnt_i,\n"
        "               dut.instr_rvalid_i, dut.instr_rdata_i,\n"
        "               instr_mem_vif.request, instr_mem_vif.addr,\n"
        "               instr_mem_vif.grant, instr_mem_vif.rvalid,\n"
        "               instr_mem_vif.rdata);\n"
        "    end\n"
        "  end\n\n"
  # The retire itself, from both ends: what the RTL puts on RVFI and what
  # the interface the cosim monitor watches carries.
        "  always @(posedge clk) begin\n"
        "    if (dut.rvfi_valid || rvfi_if.valid) begin\n"
        "      $display(\"[RVFIDBG] t=%0t | rtl valid=%b insn=%08h rd=%0d"
        " wdata=%08h trap=%b pc=%08h | vif valid=%b rd=%0d wdata=%08h\",\n"
        "               $time, dut.rvfi_valid, dut.rvfi_insn, dut.rvfi_rd_addr,\n"
        "               dut.rvfi_rd_wdata, dut.rvfi_trap, dut.rvfi_pc_rdata,\n"
        "               rvfi_if.valid, rvfi_if.rd_addr, rvfi_if.rd_wdata);\n"
        "      $display(\"[RVFIDBG]   cause: illegal=%b fetch_err=%b exc_cause=%h"
        " instr_err_pin=%b priv=%h\",\n"
        "               dut_if.illegal_instr,\n"
        "               dut.u_ibex_top.u_ibex_core.id_stage_i.controller_i.instr_fetch_err,\n"
        "               dut_if.exc_cause, instr_mem_vif.error, dut_if.priv_mode);\n"
        "      $display(\"[RVFIDBG]   csr:   priv_lvl_q=%h illegal_csr=%b"
        " csr_access=%b csr_addr=%h\",\n"
        "               dut.u_ibex_top.u_ibex_core.cs_registers_i.priv_lvl_q,\n"
        "               dut.u_ibex_top.u_ibex_core.cs_registers_i.illegal_csr,\n"
        "               dut.u_ibex_top.u_ibex_core.csr_access,\n"
        "               dut.u_ibex_top.u_ibex_core.csr_addr);\n"
        "    end\n"
        "  end\n"
        "endmodule\n",
    ),
    (
        CORE_IBEX / "common/ibex_mem_intf_agent/ibex_mem_intf_response_seq_lib.sv",
        '      `uvm_info(get_full_name(), $sformatf("Response transfer:\\n%0s",'
        ' req.sprint()), UVM_HIGH)\n'
        '      start_item(req);\n',
        '      if (req.addr < 32\'h8000_0100) begin\n'
        '        $display("[MEMDBG %0s] t=%0t req.addr=%08h item.addr=%08h rw=%0d data=%08h uninit=%0b",\n'
        '                 is_dmem_seq ? "D" : "I", $time, req.addr, item.addr, item.read_write,\n'
        '                 req.data, data_was_uninitialized);\n'
        '      end\n'
        '      `uvm_info(get_full_name(), $sformatf("Response transfer:\\n%0s",'
        ' req.sprint()), UVM_HIGH)\n'
        '      start_item(req);\n',
    ),
]


# Every agent in this testbench starts its run_phase with
#
#     wait (vif.<cb>.reset === 1'b0);
#
# meaning "block until we are out of reset". A clocking block input has no
# sampled value before its first clocking event, so on a 4-state simulator it
# reads X, `=== 1'b0` is false, and the wait blocks as intended. Verilator has
# no X: the sampled variable reads 0, the comparison is true at time 0, and
# every driver and monitor is released while reset is still asserted.
#
# Reduced case, `wait` on a clocking block input against `wait` on the raw wire
# behind it, with reset genuinely asserted until t=100:
#
#     0    wait on clocking-block input woke     <-- wrong
#     100  rst deasserted
#     100  wait on raw wire woke                 <-- right
#
# The damage is not a stall. The monitors are released at time 0, immediately
# see reset asserted at the first clocking event, take their mid-test-reset
# branch and kill their collectors -- so no address phase is ever published, no
# response sequence ever responds, and the memory interface signals sit at
# whatever Verilator initialised them to. The core then "executes" from a bus
# nothing is driving, retires 0x0000 at its reset vector, and the cosim
# scoreboard reports a mismatch on instruction one. That is the shared cause
# behind 22 of the 27 test classes.
#
# The fix is to sample the clocking block once before testing it, which is what
# a 4-state simulator effectively gets for free. Applied by regex because the
# same line appears in seven files with three different clocking block names.
RESET_WAIT = re.compile(
    r"^(?P<indent>[ 	]*)wait \((?P<cb>[\w.]+)\.reset === 1'b0\);[ 	]*$",
    re.M)

RESET_WAIT_FILES = [
    CORE_IBEX / "common/ibex_mem_intf_agent/ibex_mem_intf_monitor.sv",
    CORE_IBEX / "common/ibex_mem_intf_agent/ibex_mem_intf_response_driver.sv",
    CORE_IBEX / "common/irq_agent/irq_request_driver.sv",
    CORE_IBEX / "common/irq_agent/irq_monitor.sv",
    CORE_IBEX / "common/ibex_cosim_agent/ibex_rvfi_monitor.sv",
    CORE_IBEX / "common/ibex_cosim_agent/ibex_ifetch_monitor.sv",
    CORE_IBEX / "common/ibex_cosim_agent/ibex_ifetch_pmp_monitor.sv",
]

# What the count should be, so a file gaining or losing one is a build failure
# rather than a silent change in how much of the testbench is fixed.
RESET_WAIT_COUNT = 10


# Measurement only, added by --pin-delays. Two edits that between them take
# every constrained solve off the memory-response path:
#
#   send_grant() randomizes gnt_delay with a dist on every grant, on both
#   agents, and the response sequence randomizes rvalid_delay once per
#   transaction. Forcing the zero-delay branch skips the first; rand_mode(0)
#   plus a fixed value skips the second.
#
# Not a configuration to keep -- it removes the protocol-delay variation the
# agent exists to provide -- but the difference against a normal build is how
# much of the run time the solver costs. Verilator makes no solver call at all
# when a randomize() has nothing left to solve, so with both applied the
# solver log should be empty. Check that it is before reading any timing.
MEASURE_OVERLAYS = [
    (
        CORE_IBEX / "common/ibex_mem_intf_agent/ibex_mem_intf_response_driver.sv",
        "      if(cfg.zero_delays) begin\n",
        "      if(1) begin  // measurement: skip the gnt_delay solve\n",
    ),
]


# Measurement build: take the interrupt agent out of the environment.
#
# `core_ibex_env::build_phase` is unconditional, so every test builds the same
# seven components whether it uses them or not. `core_ibex_cpptb` ports six of
# them; the interrupt agent is the one it does not have, and its monitor runs
#
#     forever begin ... vif.wait_clks(1); end
#
# for the whole of every run. So a UVM directed run pays for a component the
# cpptb run does not, and a throughput comparison between the two charges cpptb
# nothing for it. Removing it here is how that difference gets a number instead
# of a caveat.
#
# Under core_ibex_base_test nothing drives the interrupt lines, so
# irq_monitor::collect_irq compares against its initial `stored_irq_val = '0`
# and never writes an item: the analysis path is dead and only the per-cycle
# sample loop costs anything. base_test's own `irq_collected_port` is a
# uvm_tlm_analysis_fifo it flushes on reset and never gets, so dropping the
# connect changes no behaviour on this test list. It would change behaviour on
# the eight test classes that raise interrupts, which is why this is a
# measurement flag and not a default.
NO_IRQ_OVERLAYS = [
    (
        CORE_IBEX / "env/core_ibex_env.sv",
        "    irq_agent = irq_request_agent::type_id::create(\"irq_agent\", this);\n",
        "    // measurement: interrupt agent not built; see NO_IRQ_OVERLAYS\n",
    ),
    (
        CORE_IBEX / "env/core_ibex_env.sv",
        "    vseqr.irq_seqr = irq_agent.sequencer;\n",
        "    // measurement: no interrupt agent to take a sequencer from\n",
    ),
    (
        CORE_IBEX / "tests/core_ibex_base_test.sv",
        "    env.irq_agent.monitor.irq_port.connect(this.irq_collected_port.analysis_export);\n",
        "    // measurement: no interrupt monitor to subscribe to\n",
    ),
]


class BuildError(RuntimeError):
    pass


def apply_overlays(debug: bool = False, pin_delays: bool = False,
                   no_irq: bool = False) -> dict[str, str]:
    """Write the patched copies and return upstream path -> overlay path."""
    out = BUILD / "overlay"
    if out.is_dir():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    # A file can carry more than one edit, so the text is threaded through
    # every rule that names it and written once at the end.
    patched: dict[Path, str] = {}
    rules = OVERLAYS + (DEBUG_OVERLAYS if debug else [])
    rules += MEASURE_OVERLAYS if pin_delays else []
    rules += NO_IRQ_OVERLAYS if no_irq else []
    for source, old, new in rules:
        if not source.is_file():
            raise BuildError(f"overlay target missing: {source}")
        text = patched.get(source, source.read_text(encoding="utf-8"))
        if text.count(old) != 1:
            raise BuildError(
                f"{source.name}: the text this build patches is no longer "
                f"present exactly once; upstream has changed and the overlay "
                f"in build_tb.py needs revisiting")
        patched[source] = text.replace(old, new)

    total = 0
    for source in RESET_WAIT_FILES:
        if not source.is_file():
            raise BuildError(f"overlay target missing: {source}")
        text = patched.get(source, source.read_text(encoding="utf-8"))
        text, count = RESET_WAIT.subn(
            lambda m: (f"{m['indent']}@({m['cb']});\n"
                       f"{m['indent']}wait ({m['cb']}.reset === 1'b0);"),
            text)
        patched[source] = text
        total += count
    if total != RESET_WAIT_COUNT:
        raise BuildError(
            f"expected {RESET_WAIT_COUNT} reset waits to fix, found {total}; "
            f"upstream has changed and RESET_WAIT needs revisiting")

    mapping = {}
    for source, text in patched.items():
        target = out / source.name
        if target.exists():
            raise BuildError(f"two overlays share the basename {source.name}")
        target.write_text(text, encoding="utf-8")
        mapping[str(source)] = str(target)
    return mapping


def strip_comments(text: str) -> str:
    """Drop the `//` comments upstream's .f files carry.

    Verilator accepts them, but the expanded list is meant to be readable when
    a build goes wrong, and a file list interleaved with licence headers is not.
    """
    return "\n".join(line for line in text.splitlines()
                     if not line.lstrip().startswith("//"))


def expand_filelist(path: Path) -> list[str]:
    """Substitute the two variables upstream's .f files are written against."""
    text = strip_comments(path.read_text(encoding="utf-8"))
    text = text.replace("${PRJ_DIR}", str(IBEX))
    text = text.replace("${LOWRISC_IP_DIR}", str(LOWRISC_IP))
    if "${" in text:
        unresolved = sorted(set(re.findall(r"\$\{(\w+)\}", text)))
        raise BuildError(f"{path.name}: unresolved variables {unresolved}")
    return [token for token in text.split() if token]


def config_parameters(name: str) -> tuple[dict[str, str], dict[str, str]]:
    """Ask Ibex's own tool for the configuration, split into -G and +define+.

    `ibex_config.py <cfg> vcs_opts` is what compile_tb.py calls. It emits
    `-pvalue+core_ibex_tb_top.X=N` for integer parameters and `+define+IBEX_CFG_X`
    for the enum and string ones, because VCS cannot override those on the
    command line. Verilator can override integers with -G; the enums stay as
    defines because the testbench reads them through `IBEX_CFG_*` regardless.
    """
    env = dict(os.environ)
    pylibs = ROOT / ".tools" / "pylibs"
    if pylibs.is_dir():
        env["PYTHONPATH"] = f"{pylibs}:{env.get('PYTHONPATH', '')}"
    result = subprocess.run(
        [sys.executable, "util/ibex_config.py",
         "--config_filename", "ibex_configs.yaml", name, "vcs_opts",
         "--ins_hier_path", TOP, "--string_define_prefix", "IBEX_CFG_"],
        cwd=IBEX, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False)
    if result.returncode != 0:
        raise BuildError(f"ibex_config.py {name} failed:\n{result.stderr.strip()}")

    parameters: dict[str, str] = {}
    defines: dict[str, str] = {}
    for token in shlex.split(result.stdout.strip()):
        if token.startswith("-pvalue+"):
            key, _, value = token[len("-pvalue+"):].partition("=")
            parameters[key.split(".")[-1]] = value
        elif token.startswith("+define+"):
            key, _, value = token[len("+define+"):].partition("=")
            defines[key] = value
        else:
            raise BuildError(f"ibex_config.py emitted an unexpected option: {token}")
    if not parameters and not defines:
        raise BuildError(f"ibex_config.py {name} emitted nothing")
    return parameters, defines


def pkg_config(packages: list[str], *flags: str) -> list[str]:
    env = dict(os.environ)
    env["PKG_CONFIG_PATH"] = str(SPIKE / "lib" / "pkgconfig")
    if not (SPIKE / "lib" / "pkgconfig").is_dir():
        raise BuildError(f"no Spike at {SPIKE}\n"
                         f"run: python3 {ROOT / 'build_spike.py'}")
    result = subprocess.run(["pkg-config", *flags, *packages], env=env,
                            text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
    if result.returncode != 0:
        raise BuildError(f"pkg-config {' '.join(flags)} failed:\n"
                         f"{result.stderr.strip()}")
    return shlex.split(result.stdout.strip())


def verilator_command(config: str, jobs: int, fcov: bool, debug: bool,
                      pin_delays: bool, extra: list[str],
                      no_irq: bool = False) -> list[str]:
    parameters, defines = config_parameters(config)
    if not HDL_PUBLIC_VLT.is_file():
        raise BuildError(f"missing Verilator control file: {HDL_PUBLIC_VLT}")
    overlay = apply_overlays(debug, pin_delays, no_irq)
    sources = [overlay.get(path, path)
               for path in expand_filelist(CORE_IBEX / "ibex_dv.f")]
    if not fcov:
        sources = [path for path in sources
                   if Path(path).name not in FCOV_SOURCES]
        defines["DV_FCOV_DISABLE"] = ""
    # Verilator compiles every source it is given with $(CXX), so the one C
    # file in the list goes through the shim in shims/date_c_linkage.cc that
    # restores its C linkage. See that file.
    sources = [path for path in sources if Path(path).name != "date.c"]
    cosim = expand_filelist(CORE_IBEX / "ibex_dv_cosim_dpi.f")

    cflags = pkg_config(SPIKE_PACKAGES, "--cflags") + [f"-I{IBEX}/dv/cosim"]
    libs = pkg_config(SPIKE_PACKAGES, "--libs")

    command = [
        "verilator", "--binary", "--timing", "--vpi",
        # Verilator makes warnings fatal by default. The compile raises about
        # 3,500, nearly all WIDTHTRUNC and WIDTHEXPAND from UVM's own source
        # and from lowRISC's DV library -- code no other simulator lints this
        # way. They are kept in the log rather than silenced individually.
        "-Wno-fatal",
        # Verilator's skip-identical check hashes the command line and the
        # files it read last time. Neither notices when a source moves from the
        # upstream tree into build/overlay/, because the include path is not on
        # the command line and the upstream file has not changed, so adding an
        # overlay silently has no effect until the object directory is cleared.
        # Cost is a full re-verilation per build; correctness is worth it.
        "--no-skip-identical",
        "-j", str(jobs),
        "--top-module", TOP,
        "--timescale", "1ns/10ps",
        "--Mdir", str(obj_dir(config, "no-irq" if no_irq else "")),
        "-o", "core_ibex_tb",
        # The overlay directory comes first so a patched `include is picked up
        # ahead of the upstream one; only files this build patches are in it.
        f"+incdir+{BUILD / 'overlay'}",
        # UVM's own DPI, and the library itself. incdir first so
        # `include "uvm_macros.svh"` resolves from every file that opens with it.
        f"+incdir+{UVM}",
        f"+incdir+{IBEX}/dv/cosim",
        # The signals UVM's HDL access reaches by name. Nothing is public to VPI
        # in Verilator unless it is named here; see HDL_PUBLIC_VLT above.
        str(HDL_PUBLIC_VLT),
        str(UVM / "uvm_pkg.sv"),
    ]
    command += expand_filelist(CORE_IBEX / "ibex_dv_defines.f")
    command += [f"+define+{k}={v}" for k, v in ADDRESS_DEFINES.items()]
    command += [f"+define+{k}={v}" if v else f"+define+{k}"
                for k, v in defines.items()]
    command += [f"-G{k}={v}" for k, v in parameters.items()]
    command += sources
    command += cosim
    command += [str(HERE / "shims" / "uvm_dpi_verilator.cc"),
                str(HERE / "shims" / "date_c_linkage.cc")]
    command += ["-CFLAGS", shlex.join(
        [f"-I{UVM}/dpi", f"-I{CORE_IBEX}/common", *cflags])]
    command += ["-LDFLAGS", shlex.join(libs)]
    command += extra
    return command


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", default="small",
                        help="Ibex configuration from ibex_configs.yaml")
    # Half the cores, not all of them: compiling UVM together with the design
    # peaks well over a gigabyte per job, and oversubscribing gets the compiler
    # killed part way through with "fatal error: Killed signal terminated
    # program cc1plus", which reads like a compiler bug rather than OOM.
    parser.add_argument("--jobs", type=int,
                        default=max(1, (os.cpu_count() or 4) // 2))
    parser.add_argument("--fcov", action="store_true",
                        help="compile the functional coverage sources; see "
                             "FCOV_SOURCES for why this does not work yet")
    parser.add_argument("--debug-mem", action="store_true",
                        help="print every memory response near the reset "
                             "vector; see DEBUG_OVERLAYS")
    parser.add_argument("--pin-delays", action="store_true",
                        help="measurement build: no constrained solve on the "
                             "memory-response path; see MEASURE_OVERLAYS")
    parser.add_argument("--no-irq-agent", action="store_true",
                        help="measurement build: leave the interrupt agent out "
                             "of the environment, matching what core_ibex_cpptb "
                             "does not port; see NO_IRQ_OVERLAYS. Builds into "
                             "obj_<config>-no-irq, so the default binary and "
                             "whatever was measured with it are untouched")
    parser.add_argument("--show", action="store_true",
                        help="print the command without running it")
    parser.add_argument("extra", nargs="*",
                        help="further options passed to Verilator")
    args = parser.parse_args(argv)

    BUILD.mkdir(parents=True, exist_ok=True)
    try:
        command = verilator_command(args.config, args.jobs, args.fcov,
                                    args.debug_mem, args.pin_delays, args.extra,
                                    args.no_irq_agent)
    except BuildError as error:
        print(f"build_tb: {error}", file=sys.stderr)
        return 1

    if args.show:
        print(shlex.join(command))
        return 0

    variant = "no-irq" if args.no_irq_agent else ""
    tag = f"{args.config}-{variant}" if variant else args.config
    log = BUILD / f"compile_tb_{tag}.log"
    # The command is a page wide with a hundred absolute paths in it. Keeping it
    # beside the log rather than at the top of it leaves the log greppable.
    (BUILD / f"compile_tb_{tag}.cmd").write_text(
        shlex.join(command) + "\n", encoding="utf-8")
    print(f"build_tb: {tag}, logging to {log.relative_to(HERE)}")
    with log.open("w", encoding="utf-8") as handle:
        completed = subprocess.run(command, cwd=BUILD, stdout=handle,
                                   stderr=subprocess.STDOUT, check=False)
    if completed.returncode != 0:
        print(f"build_tb: failed; see {log}", file=sys.stderr)
        return completed.returncode
    print(f"build_tb: built {obj_dir(args.config, variant) / 'core_ibex_tb'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
