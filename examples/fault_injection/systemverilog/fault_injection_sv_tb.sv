module fault_injection_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  logic clk;
  logic rst_n;
  logic [7:0] source_i;
  logic [1:0] memory_address;
  logic memory_write;
  logic [15:0] memory_write_data;
  logic [7:0] resolved_o;
  logic [7:0] counter_o;
  logic [15:0] memory_read_data;

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
      $error("%s: expected 0x%0h, got 0x%0h", label, expected, actual);
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
    source_i = '0;
    memory_address = '0;
    memory_write = 1'b0;
    memory_write_data = '0;
    checks = 0;
    failures = 0;

    repeat (2) @(posedge clk);
    @(negedge clk);
    rst_n = 1'b1;

    @(posedge clk);
    #1ps;
    expect_eq("counter baseline", counter_o, 1);

    source_i = 8'h12;
    #1ps;
    expect_eq("resolved net baseline", resolved_o, 8'h48);

    force i_dut.resolved_value = 8'ha5;
    expect_eq("force is immediately readable", i_dut.resolved_value, 8'ha5);
    #1ps;
    expect_eq("forced net reaches output", resolved_o, 8'ha5);

    source_i = 8'h34;
    #1ps;
    expect_eq("force overrides changing driver", resolved_o, 8'ha5);

    release i_dut.resolved_value;
    #1ps;
    expect_eq("release restores resolved driver", resolved_o, 8'h6e);

    force i_dut.counter = 8'h55;
    repeat (2) @(posedge clk);
    #1ps;
    expect_eq("RTL writes do not override force", counter_o, 8'h55);

    release i_dut.counter;
    @(posedge clk);
    #1ps;
    expect_eq("RTL writes resume after release", counter_o, 8'h56);

    i_dut.memory[2] = 16'hbeef;
    expect_eq("deposit is immediately readable", i_dut.memory[2], 16'hbeef);
    memory_address = 2;
    #1ps;
    expect_eq("deposit reaches memory output", memory_read_data, 16'hbeef);

    force i_dut.memory[2] = 16'hcafe;
    expect_eq("memory force is immediately readable", i_dut.memory[2], 16'hcafe);
    #1ps;
    expect_eq("memory force reaches output", memory_read_data, 16'hcafe);
    release i_dut.memory[2];

    memory_write_data = 16'h1234;
    memory_write = 1'b1;
    @(posedge clk);
    #1ps;
    memory_write = 1'b0;
    expect_eq("front-door write follows release", memory_read_data, 16'h1234);

    $display(
        "PURE_SV_FAULT_INJECTION_RESULT iterations=%0d checks=%0d sim_cycles=%0d failures=%0d",
        1, checks, sim_cycles, failures);
    if (failures != 0) $fatal(1, "fault-injection pure-SV testbench failed");
    $finish;
  end

  initial begin
    #1ms;
    $fatal(1, "fault-injection pure-SV testbench timed out");
  end

  fault_injection i_dut (.*);
endmodule
