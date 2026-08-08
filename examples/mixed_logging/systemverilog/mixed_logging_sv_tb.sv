module mixed_logging_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  logic clk = 1'b0;
  logic rst_n = 1'b0;
  logic valid = 1'b0;
  logic [31:0] data = '0;
  logic [31:0] accepted_count;
  int unsigned checks = 0;
  int unsigned failures = 0;

  mixed_logging dut (.*);

  always #5ns clk = ~clk;

  task automatic expect_eq(input string label, input logic [31:0] actual,
                           input logic [31:0] expected);
    checks++;
    if (actual !== expected) begin
      failures++;
      $error("%s: actual=%0d expected=%0d", label, actual, expected);
    end
  endtask

  initial begin
    repeat (2) @(posedge clk);
    rst_n <= 1'b1;

    // Drive after the rising edge through non-blocking assignments -- the
    // same schedule the C++ testbench gets from deferred writes.
    for (int unsigned index = 0; index < 2; index++) begin
      @(posedge clk);
      data <= 32'h1234_0000 + index;
      valid <= 1'b1;
      @(posedge clk);
      #1ps;
      expect_eq("accepted count", accepted_count, index + 1);
      $display("%s:%0d: observed accepted count=%0d", `__FILE__, `__LINE__,
               index + 1);
      valid <= 1'b0;
    end

    $display("PURE_SV_MIXED_LOGGING_RESULT checks=%0d failures=%0d", checks,
             failures);
    $finish;
  end
endmodule
