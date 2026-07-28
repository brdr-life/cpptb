// Reduced case: a constrained randomize() over an `inside` range is not
// uniform over that range.
//
// Found while comparing ports/ibex_icache_cpptb against ports/ibex_icache_uvm.
// ibex_icache_core_base_seq draws num_insns from a three-bucket dist, and the
// UVM baseline replaces that dist with a direct draw of the bucket plus
//
//     soft num_insns inside {[lo:hi]};
//
// against the item's own hard `num_insns inside {[0:100]}`. The bucket weights
// come out right, and the value inside the bucket does not: about half of every
// [1:20] draw lands in [16:20], where a quarter should. The baseline therefore
// runs about 25% more instruction fetches per transaction than its own
// constraints ask for.
//
// Build it as a standalone module with `--binary --timing -Wno-fatal` and run
// the result. An SMT solver has to be on PATH: constraints are solved by
// piping to `z3 --in`.

module verilator_inside_range_uniformity;

  localparam int unsigned Draws = 4000;

  class hard_only;
    rand int value;
    constraint c_support { value inside {[1:20]}; }
  endclass

  class soft_bucket;
    rand int value;
    constraint c_support { value inside {[0:100]}; }
  endclass

  function automatic void report(string label, int unsigned quartile[4],
                                 int unsigned total, real sum);
    $display("%-28s q1=%0d q2=%0d q3=%0d q4=%0d  mean=%0.2f (uniform 10.50)",
             label, quartile[0], quartile[1], quartile[2], quartile[3],
             sum / total);
  endfunction

  initial begin
    hard_only  a = new();
    soft_bucket b = new();
    int unsigned quartile[4];
    real sum;
    int value;

    // 1. A hard `inside` range on its own.
    quartile = '{0, 0, 0, 0};
    sum = 0.0;
    repeat (Draws) begin
      if (!a.randomize()) $fatal(1, "randomize failed");
      quartile[(a.value - 1) / 5]++;
      sum += a.value;
    end
    report("hard inside [1:20]", quartile, Draws, sum);

    // 2. A soft `inside` bucket inside a wider hard range, which is the shape
    //    ibex_icache_core_base_seq uses.
    quartile = '{0, 0, 0, 0};
    sum = 0.0;
    repeat (Draws) begin
      if (!b.randomize() with { soft value inside {[1:20]}; })
        $fatal(1, "randomize failed");
      if (b.value < 1 || b.value > 20) $fatal(1, "soft bucket not honoured");
      quartile[(b.value - 1) / 5]++;
      sum += b.value;
    end
    report("soft inside [1:20] of [0:100]", quartile, Draws, sum);

    // 3. The control: the same range drawn directly, which is what the cpptb
    //    port does.
    quartile = '{0, 0, 0, 0};
    sum = 0.0;
    repeat (Draws) begin
      value = $urandom_range(20, 1);
      quartile[(value - 1) / 5]++;
      sum += value;
    end
    report("$urandom_range(20, 1)", quartile, Draws, sum);

    $finish;
  end

endmodule
