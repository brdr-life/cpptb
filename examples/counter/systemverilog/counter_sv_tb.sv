module counter_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  logic clk;
  logic rst_n;
  logic enable;
  logic [7:0] count;

  localparam int unsigned kCountCycles = 8;

  longint unsigned checks;
  int unsigned failures;
  longint unsigned sim_cycles;

  task automatic expect_eq(
      input string label,
      input logic [31:0] actual,
      input logic [31:0] expected
  );
    checks++;
    if (actual !== expected) begin
      failures++;
      $error("%s: expected %0d, got %0d", label, expected, actual);
    end
  endtask

  initial begin
    clk = 1'b0;
    sim_cycles = 0;
    forever begin
      #5ns clk = ~clk;
      if (clk) sim_cycles++;
    end
  end

  initial begin
    rst_n = 1'b0;
    enable = 1'b0;
    checks = 0;
    failures = 0;
    repeat (2) @(posedge clk);
    // Release at the edge through non-blocking assignments -- the same
    // schedule the C++ testbench gets from deferred writes.
    rst_n <= 1'b1;
    enable <= 1'b1;

    for (int unsigned expected = 1; expected <= kCountCycles; expected++) begin
      @(posedge clk);
      #1ps;
      expect_eq("enabled count", count, expected);
    end

    enable = 1'b0;
    @(posedge clk);
    #1ps;
    expect_eq("disabled count", count, kCountCycles);

    $display(
        "PURE_SV_COUNTER_RESULT iterations=%0d checks=%0d sim_cycles=%0d failures=%0d",
        1, checks, sim_cycles, failures);
    if (failures != 0) $fatal(1, "counter pure-SV testbench failed");
    $finish;
  end

  initial begin
    #1ms;
    $fatal(1, "counter pure-SV testbench timed out");
  end

  counter i_dut (.*);

// Wave dumping for the equivalence flow only: the wave build verilates
// with --trace +define+CPPTB_TWIN_WAVE and runs from the directory the
// dump belongs in. The normal twin build compiles this away, keeping the
// workload knob-free.
`ifdef CPPTB_TWIN_WAVE
  initial begin
    $dumpfile("twin.vcd");
    $dumpvars(0, counter_sv_tb);
  end
`endif

endmodule
