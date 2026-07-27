// BUG (Verilator 5.050): a class-scope `dist` constraint is applied as an
// equality against a freshly drawn sample on EVERY randomize() of the object,
// including calls where the dist's left-hand side is not in the argument list
// and is therefore a state variable that must be left alone.
//
// The lowering of `x dist {a :/ wa, b :/ wb}` is two hard SMT assertions.
// From the generated C++ (obj_.../V..._Vclpkg__0.cpp), lightly trimmed:
//
//     bucket = 1 + VL_RANDOM_Q() % (wa + wb);              // drawn per solve
//     lhs    = __Vrandmode.at(0) ? "x" : "<current value of x>";
//     constraint.hard( "(bvor (= lhs a) (= lhs b))" );      // membership  OK
//     constraint.hard( bucket <= wa ? "(= lhs a)"
//                                   : "(= lhs b)" );        // sample equality
//
// The `__Vrandmode` gate -- Verilator's rand_mode / argument-list test -- is
// applied to the *operand*, so a state variable is correctly folded to its
// current value. It is never applied to the *decision to emit the second
// assertion*. So for a state variable the second assertion degenerates to
// `<current value> == <freshly drawn sample>`: a coin flip that takes the whole
// solve UNSAT and reports the dist constraint as the culprit.
//
// When constant folding instead leaves the operand symbolic, the same missing
// gate has a quieter effect: the solver assigns x and Verilator writes the
// result back, mutating a variable the caller never asked to randomize
// (IEEE 1800-2017 18.11: everything outside the argument list is a state
// variable for that call). Case D.
//
// This is what breaks Ibex's core_base_new_seq
// (dv/uvm/core_ibex/tests/core_ibex_new_seq_lib.sv). `zero_delays` carries a
// class-scope dist and the next line does `randomize(stimulus_delay_cycles)`:
//
//   %Warning-UNSATCONSTR: core_ibex_new_seq_lib.sv:19:
//     Unsatisfied constraint: 'zero_delays dist {1 :/ zero_delay_pct,'
//   UVM_FATAL core_ibex_new_seq_lib.sv(97): Randomization failed!
//
// `soft` does not rescue it (case E) -- a soft dist is still emitted as a hard
// equality. Nor does moving the dist into an inline `randomize(x) with {}`:
// case F shows that Verilator drops the weights entirely from an inline dist
// and randomizes uniformly over the listed values, which is a second and
// separate defect. The only lowering that is both correct and weighted is to
// draw the value explicitly -- case G, which is what the port's overlay does.
//
// Expected (an LRM-conforming simulator):
//   A 40/40   B 40/40   C 40/40   D 0/40   E 40/40 and 40/40
//   F  10% / 10% / 70-10-10-10
//   G  10% / 70-10-10-10
//
// Observed, Verilator 5.050 + z3 4.8.12 (counts move with the seed, shape does
// not; the deterministic 0/40 rows stay 0/40):
//   A 18/40   B 40/40   C 0/40    D 22/40  E 24/40 and 0/40
//   F  50% / 49% / 25-24-24-25     <-- weights ignored by an inline dist
//   G   9% / 69-10-10-9            <-- the fix
//
// A is the failure itself. B and C are the proof that the failing term is the
// sample equality and not set membership: both have pct=100, so 1 is the only
// value the sampler can draw; with the state variable at 1 it passes 40/40 and
// with it at 0 it fails 40/40, deterministically, even though 0 is a listed
// value of the distribution.
//
// Run with:
//   $ verilator --binary --timing -Wno-fatal verilator_dist_on_state_var.sv \
//       -o vdsv --Mdir obj_vdsv && ./obj_vdsv/vdsv
//
// (z3 must be on PATH or every randomize fails for an unrelated reason and the
// run takes no time at all.)

// The shape core_base_new_seq has: a dist whose weights are class members.
class member_weights;
  rand bit          z;
  rand int unsigned other;
  int unsigned      pct = 50;
  constraint z_c     { z dist {1 :/ pct, 0 :/ 100 - pct}; }
  constraint other_c { other inside {[1000:2000]}; }
endclass

// Literal weights fold differently and expose the write-back face instead.
class literal_weights;
  rand bit          z;
  rand int unsigned other;
  constraint z_c     { z dist {1 :/ 50, 0 :/ 50}; }
  constraint other_c { other inside {[1000:2000]}; }
endclass

class soft_member_weights;
  rand bit          z;
  rand int unsigned other;
  int unsigned      pct = 50;
  constraint z_c     { soft z dist {1 :/ pct, 0 :/ 100 - pct}; }
  constraint other_c { other inside {[1000:2000]}; }
endclass

// No dist at class scope: nothing to trip the other randomize() calls.
class no_class_scope_dist;
  rand bit          z;
  rand int unsigned v;
  int unsigned      pct = 50;
endclass

module top;

  initial begin : main
    automatic member_weights      m  = new();
    automatic literal_weights     l  = new();
    automatic soft_member_weights s  = new();
    automatic no_class_scope_dist n  = new();
    int ok, ones, mutated;
    int h[4];

    // Give every object a legal starting value, as core_base_new_seq::pre_body
    // does: this simulator keeps all constraints active over state variables,
    // so a member outside its own range would fail the call for a real reason.
    void'(m.randomize());
    void'(l.randomize());
    void'(s.randomize());
    void'(n.randomize());

    // A -- the bug. z is a state variable holding a legal, weight-50 value.
    m.pct = 50; m.z = 0; ok = 0;
    for (int i = 0; i < 40; i++) if (m.randomize(other)) ok++;
    $display("A  pct=50  z=0   randomize(other)      : %0d/40   (expect 40)", ok);

    // B -- pct=100: the sampler can only ever draw 1, and z is 1.
    m.pct = 100; m.z = 1; ok = 0;
    for (int i = 0; i < 40; i++) if (m.randomize(other)) ok++;
    $display("B  pct=100 z=1   randomize(other)      : %0d/40   (expect 40)", ok);

    // C -- same, z is 0. Fails every time: 0 == 1 is never satisfiable.
    m.pct = 100; m.z = 0; ok = 0;
    for (int i = 0; i < 40; i++) if (m.randomize(other)) ok++;
    $display("C  pct=100 z=0   randomize(other)      : %0d/40   (expect 40)", ok);

    // D -- the write-back face. randomize(other) must not touch z.
    mutated = 0;
    for (int i = 0; i < 40; i++) begin
      l.z = 0;
      void'(l.randomize(other));
      if (l.z != 0) mutated++;
    end
    $display("D  literal wts   z mutated by the call : %0d/40   (expect 0)", mutated);

    // E -- `soft` does not help: a soft dist is still emitted as a hard
    // equality, so it cannot be dropped when it conflicts.
    s.pct = 50; s.z = 0; ok = 0;
    for (int i = 0; i < 40; i++) if (s.randomize(other)) ok++;
    $display("E  soft dist     pct=50  z=0           : %0d/40   (expect 40)", ok);
    s.pct = 100; s.z = 0; ok = 0;
    for (int i = 0; i < 40; i++) if (s.randomize(other)) ok++;
    $display("E  soft dist     pct=100 z=0           : %0d/40   (expect 40)", ok);

    // F -- inline dist: never UNSAT, but the weights are thrown away.
    n.pct = 10; ones = 0;
    for (int i = 0; i < 4000; i++) begin
      void'(n.randomize(z) with { z dist {1 :/ pct, 0 :/ 100 - pct}; });
      ones += n.z;
    end
    $display("F  inline dist   member weight pct=10  : %0d%%     (expect 10)",
             ones * 100 / 4000);
    ones = 0;
    for (int i = 0; i < 4000; i++) begin
      void'(n.randomize(z) with { z dist {1 :/ 10, 0 :/ 90}; });
      ones += n.z;
    end
    $display("F  inline dist   literal weight 10/90  : %0d%%     (expect 10)",
             ones * 100 / 4000);
    foreach (h[i]) h[i] = 0;
    for (int i = 0; i < 4000; i++) begin
      void'(n.randomize(v) with { v dist {0 :/ 70, 1 :/ 10, 2 :/ 10, 3 :/ 10}; });
      if (n.v < 4) h[n.v]++;
    end
    $display("F  inline dist   4-way 70/10/10/10     : %0d/%0d/%0d/%0d %% (expect 70/10/10/10)",
             h[0] * 100 / 4000, h[1] * 100 / 4000, h[2] * 100 / 4000, h[3] * 100 / 4000);

    // G -- the fix: draw the value, do not ask the solver for it.
    n.pct = 10; ones = 0;
    for (int i = 0; i < 4000; i++) begin
      n.z = ($urandom_range(99, 0) < n.pct);
      ones += n.z;
    end
    $display("G  urandom_range member weight pct=10  : %0d%%     (expect 10)",
             ones * 100 / 4000);
    foreach (h[i]) h[i] = 0;
    for (int i = 0; i < 4000; i++) begin
      int unsigned pick;
      pick = $urandom_range(99, 0);
      if      (pick < 70) n.v = 0;
      else if (pick < 80) n.v = 1;
      else if (pick < 90) n.v = 2;
      else                n.v = 3;
      h[n.v]++;
    end
    $display("G  urandom_range 4-way 70/10/10/10     : %0d/%0d/%0d/%0d %% (expect 70/10/10/10)",
             h[0] * 100 / 4000, h[1] * 100 / 4000, h[2] * 100 / 4000, h[3] * 100 / 4000);

    $finish;
  end : main

endmodule
