// A `solve ... before ...` anywhere in a class stops every `soft` constraint
// in that class from being honoured on Verilator 5.050.
//
// Three classes that differ only in that one line. `soft s == 0` has nothing
// competing with it in any of them, so it should hold every time.
//
//   soft + plain second var : broken 0/200
//   soft + if/else          : broken 0/200
//   soft + solve before     : broken 197/200
//
// lowRISC's push_pull_agent_cfg is written in exactly that shape: four
// `soft x_min == 0` constraints and four `solve zero_delays before x_max`.
// The minima come out as arbitrary 32-bit values, so
// `ack_lo_delay_min = 2048` against `ack_lo_delay_max = 35`, and
// push_pull_base_seq then randomizes `ack_lo_delay inside {[min:max]}` over an
// empty range and reports "Randomization failed" part way into every run.
//
// A second, separate soft defect is in shims/verilator_dist_plus_equality.sv's
// neighbourhood and is worth stating here too: a soft constraint nested inside
// an `if` in a constraint block is honoured when satisfiable and is *not*
// dropped when it conflicts, which is the opposite of what soft means. Only
// top-level soft constraints behave correctly.
//
// Build it with --binary --timing -Wno-fatal. The command cannot be written out
// here: a comment whose first word is the name of the tool is read as a
// metacomment and fails the compile.
//
// SPDX-License-Identifier: Apache-2.0

module verilator_soft_with_solve_before;

  // soft, plus a second random variable with an ordinary constraint.
  class plain;
    rand int unsigned s;
    rand int unsigned x;
    constraint s_c { soft s == 0; }
    constraint x_c { x inside {[1:100]}; }
  endclass

  // soft, plus a conditional constraint on another variable.
  class conditional;
    rand int unsigned s;
    rand int unsigned x;
    rand bit          z;
    constraint s_c { soft s == 0; }
    constraint x_c { if (z) { x == 0; } else { x inside {[1:100]}; } }
  endclass

  // The same, with an ordering constraint added.
  class ordered;
    rand int unsigned s;
    rand int unsigned x;
    rand bit          z;
    constraint s_c { soft s == 0; }
    constraint x_c {
      solve z before x;
      if (z) { x == 0; } else { x inside {[1:100]}; }
    }
  endclass

  initial begin
    plain       o_plain = new();
    conditional o_cond  = new();
    ordered     o_order = new();
    int unsigned broken_plain = 0, broken_cond = 0, broken_order = 0;

    for (int unsigned i = 0; i < 200; i++) begin
      void'(o_plain.randomize());
      if (o_plain.s != 0) broken_plain++;
      void'(o_cond.randomize());
      if (o_cond.s != 0) broken_cond++;
      void'(o_order.randomize());
      if (o_order.s != 0) broken_order++;
    end

    $display("soft + plain second var : broken %0d/200", broken_plain);
    $display("soft + if/else          : broken %0d/200", broken_cond);
    $display("soft + solve before     : broken %0d/200", broken_order);
    $finish;
  end

endmodule
