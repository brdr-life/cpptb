// A one-cycle pulse driven through a clocking block output is lost on this
// simulator (5.050) unless the first write happens on a clocking event.
//
//     cb.pulse <= 1'b1;   // due at the next clocking event
//     @(cb);              // wakes at that event
//     cb.pulse <= 1'b0;   // due at the event after it
//
// The second write is applied to the same clocking event as the first, so the
// two collapse and the signal never goes high. Measured output:
//
//   posedge t=10 pulse=0 level=0
//   posedge t=30 pulse=0 level=1    <-- pulse should be 1 here
//   posedge t=50 pulse=0 level=1
//
// `level`, written once, appears exactly where it should. `pulse` never
// appears at all. A numeric output skew does not help: `default output #2ns`
// loses it the same way. `default output negedge`, which is what lowRISC's
// interfaces use and which would separate the two drives, is not implemented
// at all.
//
// Waiting for a clocking event before the first write restores the pulse and
// gives back the timing the negedge skew produces: one cycle, at the edge
// after the write when the caller was already on an edge, and one edge later
// when it was not.
//
// This is why ibex_icache_stress_all fails. dv_base_vseq::dut_init ends with
// `#1ps`, so the first item of a child sequence after a mid-test reset is
// driven a picosecond off the clock grid, ibex_icache_core_if::branch_to()
// drives no branch at all, and the cache carries on prefetching from the
// address it held before the reset.
//
// Build it with --binary --timing -Wno-fatal. The command cannot be written out
// here: a comment whose first word is the name of the tool is read as a
// metacomment and fails the compile.
//
// SPDX-License-Identifier: Apache-2.0

`timescale 1ns/1ps

module verilator_clocking_pulse;

  bit clk = 0;
  always #10 clk = ~clk;

  logic pulse;        // written 1, then 0 one clocking event later
  logic level;        // written once, as a control
  logic pulse_skew;   // the same pulse through a numeric output skew
  logic pulse_align;  // the same pulse, started on a clocking event

  clocking cb @(posedge clk);
    output pulse;
    output level;
    output pulse_align;
  endclocking

  clocking cb_skew @(posedge clk);
    default output #2ns;
    output pulse_skew;
  endclocking

  // The workaround: the time of the last clocking event, and a wait for one if
  // we are not already on it.
  // $realtime, not $time: $time is rounded to the module's time unit, so a
  // caller a picosecond off a clocking event in a 1ns/1ps module compares
  // equal to the event and the wait is skipped. That is exactly the case this
  // exists for.
  realtime cb_edge       = 0.0;
  bit      cb_edge_valid = 1'b0;
  always @(posedge clk) begin
    cb_edge       = $realtime;
    cb_edge_valid = 1'b1;
  end

  task automatic align_to_cb();
    if (!cb_edge_valid || ($realtime != cb_edge)) @(cb);
  endtask

  initial begin
    #5;                     // between edges, as a UVM driver taking an item is
    cb.level       <= 1'b1;
    cb.pulse       <= 1'b1;
    cb_skew.pulse_skew <= 1'b1;
    @(cb);
    cb.pulse       <= 1'b0;
    cb_skew.pulse_skew <= 1'b0;
  end

  initial begin
    #5;
    align_to_cb();
    cb.pulse_align <= 1'b1;
    @(cb);
    cb.pulse_align <= 1'b0;
  end

  always @(posedge clk) begin
    $display("posedge t=%0t level=%b pulse=%b pulse_skew=%b pulse_align=%b",
             $time, level, pulse, pulse_skew, pulse_align);
  end

  initial begin
    #120;
    $finish;
  end

endmodule
