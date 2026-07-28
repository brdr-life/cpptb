// `std::randomize(x) with { x dist {...} }` ignores the weights on Verilator
// 5.050 and returns a uniform pick over the support. The same distribution
// written as a class constraint, or inline on a class object's randomize(), is
// weighted correctly, so this is specific to the scope randomize form.
//
// Measured output, 5.050:
//
//   class-scope dist        1s: 985/1000 (want ~990)
//   inline dist on an object 1s: 993/1000 (want ~990)
//   std::randomize           1s: 516/1000 (want ~990)
//   dist {0 :/ 1, [1:99] :/ 1}  -> v==0 14/2000 (want ~1000)
//   req_low_cycles shape        -> v>=1000 1512/2000 (want ~71)
//
// The last line is the shape `ibex_icache_core_driver` asks for when it decides
// how long to hold the request line low, and it is the one that matters: the
// weights put about 3.5% of transactions in the 1000..1200 cycle bucket and the
// uniform pick puts 76% of them there.
//
// The support is honoured, including a zero weight, so what is lost is only the
// weighting. lowRISC's DV library reaches this form through
// `DV_CHECK_STD_RANDOMIZE_WITH_FATAL, which is std::randomize by definition.
//
// Build it with --binary --timing -Wno-fatal. The command cannot be written out
// here: a comment whose first word is the name of the tool is read as a
// metacomment and fails the compile.
//
// SPDX-License-Identifier: Apache-2.0

module verilator_dist_std_randomize;

  class weighted;
    rand int unsigned x;
    constraint k { x dist { 0 :/ 1, 1 :/ 99 }; }
  endclass

  int unsigned w_zero = 1, w_one = 99;
  int unsigned v;

  initial begin
    weighted obj = new();
    int unsigned n_class = 0, n_inline = 0, n_std = 0, n_nonconst = 0;
    int unsigned n_zero_weight = 0;
    int unsigned n_first_bucket = 0, n_slow_bucket = 0;

    // 0 and 1 with weights 1 and 99: three ways of writing the same thing.
    for (int i = 0; i < 1000; i++) begin
      void'(obj.randomize());
      n_class += obj.x;
      void'(obj.randomize() with { x dist { 0 :/ 1, 1 :/ 99 }; });
      n_inline += obj.x;
      void'(std::randomize(v) with { v dist { 0 :/ 1, 1 :/ 99 }; });
      n_std += v;
      void'(std::randomize(v) with { v dist { 0 :/ w_zero, 1 :/ w_one }; });
      n_nonconst += v;
      // A zero weight is honoured even here: the support is right, the
      // weighting is not.
      void'(std::randomize(v) with { v dist { 0 :/ 0, 1 :/ 1 }; });
      n_zero_weight += v;
    end

    $display("class-scope dist         1s: %0d/1000 (want ~990)", n_class);
    $display("inline dist on an object 1s: %0d/1000 (want ~990)", n_inline);
    $display("std::randomize           1s: %0d/1000 (want ~990)", n_std);
    $display("non-constant weights     1s: %0d/1000 (want ~990)", n_nonconst);
    $display("zero-weight value gone   1s: %0d/1000 (want 1000)", n_zero_weight);

    // One value against a wide range: uniform over values, not over buckets.
    for (int i = 0; i < 2000; i++) begin
      void'(std::randomize(v) with { v dist { 0 :/ 1, [1:99] :/ 1 }; });
      if (v == 0) n_first_bucket++;
    end
    $display("dist 0 vs [1:99], equal weights: v==0 %0d/2000 (want ~1000)",
             n_first_bucket);

    // The shape ibex_icache_core_driver uses for req_low_cycles.
    for (int i = 0; i < 2000; i++) begin
      void'(std::randomize(v) with { v dist { 0           :/ 20,
                                              [1:33]      :/ 5,
                                              [100:200]   :/ 2,
                                              [1000:1200] :/ 1 }; });
      if (v >= 1000) n_slow_bucket++;
    end
    $display("req_low_cycles shape: v>=1000 %0d/2000 (want ~71)", n_slow_bucket);
    $finish;
  end

endmodule
