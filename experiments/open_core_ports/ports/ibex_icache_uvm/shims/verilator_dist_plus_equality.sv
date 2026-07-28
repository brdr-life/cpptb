// A class-scope `dist` and an equality on the same variable are together
// unsatisfiable on Verilator 5.050, unless the value the equality names is the
// one the distribution happened to draw.
//
// This is the shape lowRISC's DV library is written in. dv_base_env_cfg has
//
//     constraint clk_freq_mhz_c { `DV_COMMON_CLK_CONSTRAINT(clk_freq_mhz) }
//
// which expands to a weighted `dist` over 5..100 MHz, and ibex_icache_env_cfg
// derives from it and adds
//
//     constraint clk_freq_50_c { clk_freq_mhz == 50; }
//
// 50 is inside the distribution's support, so the pair is satisfiable and
// every commercial simulator solves it. Verilator draws a sample from the
// distribution first and then asserts equality against that sample, so the
// call only succeeds when the sample is 50. Measured over 25 seeds of the real
// testbench: 1 succeeded.
//
// randomize() returns 0 with no `%Warning-UNSATCONSTR`, so the only symptom is
// `Randomization failed!` from `DV_CHECK_RANDOMIZE_FATAL at time 0.
//
// Build it with --binary --timing -Wno-fatal. The command cannot be written
// out here: a comment whose first word is the name of the tool is read as a
// metacomment and fails the compile.
//
// SPDX-License-Identifier: Apache-2.0

module verilator_dist_plus_equality;

  // The distribution as lowRISC writes it, in dv_macros.svh.
  class freq_base;
    rand int unsigned freq;
    constraint freq_c {
      freq dist {
        [5:23]  :/ 2,
        [24:25] :/ 2,
        [26:47] :/ 1,
        [48:50] :/ 2,
        [51:95] :/ 1,
        96      :/ 1,
        [97:99] :/ 1,
        100     :/ 1
      };
    }
  endclass

  // The derived class pins the frequency, as ibex_icache_env_cfg does.
  class freq_pinned extends freq_base;
    constraint pin_c { freq == 50; }
  endclass

  // The same pin, with no distribution behind it.
  class freq_no_dist;
    rand int unsigned freq;
    constraint range_c { freq inside {[5:100]}; }
    constraint pin_c   { freq == 50; }
  endclass

  int unsigned trials = 200;

  initial begin
    freq_base    plain  = new();
    freq_pinned  pinned = new();
    freq_no_dist no_dist = new();
    int unsigned ok_plain = 0, ok_pinned = 0, ok_no_dist = 0, ok_inline = 0;
    int unsigned seen_50 = 0;

    for (int unsigned i = 0; i < trials; i++) begin
      if (plain.randomize()) begin
        ok_plain++;
        if (plain.freq == 50) seen_50++;
      end
      if (pinned.randomize()) ok_pinned++;
      if (no_dist.randomize()) ok_no_dist++;
      // The same pin written inline rather than as a derived constraint.
      if (plain.randomize() with { freq == 50; }) ok_inline++;
    end

    $display("dist alone            : %0d/%0d succeeded, %0d drew 50",
             ok_plain, trials, seen_50);
    $display("dist + derived equality: %0d/%0d succeeded", ok_pinned, trials);
    $display("dist + inline equality : %0d/%0d succeeded", ok_inline, trials);
    $display("range + equality       : %0d/%0d succeeded", ok_no_dist, trials);
    $finish;
  end

endmodule
