module apb_regfile_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  logic clk;
  logic rst_n;
  logic apb_select;
  logic apb_enable;
  logic apb_write;
  logic [7:0] apb_address;
  logic [31:0] apb_write_data;
  logic [31:0] apb_read_data;
  logic apb_ready;
  logic apb_error;

  localparam int unsigned kRegisterTransactions = 12;

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
      $error("%s: expected 0x%08x, got 0x%08x", label, expected, actual);
    end
  endtask

  task automatic apb_idle();
    apb_select = 1'b0;
    apb_enable = 1'b0;
    apb_write = 1'b0;
  endtask

  task automatic apb_write_word(
      input logic [7:0] address,
      input logic [31:0] data
  );
    @(negedge clk);
    apb_address = address;
    apb_write_data = data;
    apb_write = 1'b1;
    apb_select = 1'b1;
    apb_enable = 1'b0;

    @(posedge clk);
    @(negedge clk);
    apb_enable = 1'b1;

    @(posedge clk);
    #1ps;
    expect_eq("APB write ready", apb_ready, 1);

    @(negedge clk);
    apb_idle();
  endtask

  task automatic apb_read_word(
      input logic [7:0] address,
      output logic [31:0] data,
      output logic error
  );
    @(negedge clk);
    apb_address = address;
    apb_write = 1'b0;
    apb_select = 1'b1;
    apb_enable = 1'b0;

    @(posedge clk);
    @(negedge clk);
    apb_enable = 1'b1;

    @(posedge clk);
    #1ps;
    expect_eq("APB read ready", apb_ready, 1);
    data = apb_read_data;
    error = apb_error;

    @(negedge clk);
    apb_idle();
  endtask

  task automatic apb_read_expect(
      input string label,
      input logic [7:0] address,
      input logic [31:0] expected
  );
    logic [31:0] actual;
    logic error;
    apb_read_word(address, actual, error);
    expect_eq(label, actual, expected);
  endtask

  task automatic apb_read_error_expect(
      input string label,
      input logic [7:0] address,
      input logic [31:0] expected_data
  );
    logic [31:0] actual;
    logic error;
    apb_read_word(address, actual, error);
    expect_eq(label, actual, expected_data);
    expect_eq("APB error asserted", error, 1);
  endtask

  task automatic reset_dut();
    rst_n = 1'b0;
    apb_idle();
    apb_address = '0;
    apb_write_data = '0;
    repeat (2) @(posedge clk);
    @(negedge clk);
    rst_n = 1'b1;
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
    logic [31:0] state;
    logic [31:0] value;
    logic [7:0] address;

    checks = 0;
    failures = 0;
    reset_dut();

    state = 32'h1020_3040;
    for (int unsigned index = 0; index < kRegisterTransactions; index++) begin
      state = state * 32'd1664525 + 32'd1013904223;
      value = state;
      address = (index % 4) * 4;
      apb_write_word(address, value);
      apb_read_expect("APB register readback", address, value);
    end

    apb_read_expect("APB read-only ID", 8'h10, 32'h4350_5054);
    apb_read_error_expect("APB unmapped read data", 8'hfc, 32'd0);

    $display(
        "PURE_SV_APB_REGFILE_RESULT iterations=%0d checks=%0d sim_cycles=%0d failures=%0d",
        1, checks, sim_cycles, failures);
    if (failures != 0) $fatal(1, "APB regfile pure-SV testbench failed");
    $finish;
  end

  initial begin
    #1ms;
    $fatal(1, "APB regfile pure-SV testbench timed out");
  end

  apb_regfile i_dut (.*);
endmodule
