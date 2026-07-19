module ipxact_regfile_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  logic clk;
  logic rst_n;
  logic apb_select;
  logic apb_enable;
  logic apb_write;
  logic [8:0] apb_address;
  logic [31:0] apb_write_data;
  logic [3:0] apb_strobe;
  logic [31:0] apb_read_data;
  logic apb_ready;
  logic apb_error;

  int unsigned checks;
  int unsigned failures;

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
      input logic [8:0] address,
      input logic [31:0] data
  );
    @(negedge clk);
    apb_address = address;
    apb_write_data = data;
    apb_strobe = 4'hf;
    apb_write = 1'b1;
    apb_select = 1'b1;
    apb_enable = 1'b0;
    @(posedge clk);
    @(negedge clk);
    apb_enable = 1'b1;
    do begin
      @(posedge clk);
      #1ps;
    end while (!apb_ready);
    expect_eq("APB write status", apb_error, 0);
    @(negedge clk);
    apb_idle();
  endtask

  task automatic apb_read_word(
      input logic [8:0] address,
      output logic [31:0] data
  );
    @(negedge clk);
    apb_address = address;
    apb_write = 1'b0;
    apb_select = 1'b1;
    apb_enable = 1'b0;
    @(posedge clk);
    @(negedge clk);
    apb_enable = 1'b1;
    do begin
      @(posedge clk);
      #1ps;
    end while (!apb_ready);
    data = apb_read_data;
    expect_eq("APB read status", apb_error, 0);
    @(negedge clk);
    apb_idle();
  endtask

  initial begin
    clk = 1'b0;
    forever #5ns clk = ~clk;
  end

  initial begin
    logic [31:0] data;
    logic [31:0] packet [0:2];

    checks = 0;
    failures = 0;
    rst_n = 1'b0;
    apb_idle();
    apb_address = '0;
    apb_write_data = '0;
    apb_strobe = '0;
    repeat (2) @(posedge clk);
    @(negedge clk);
    rst_n = 1'b1;

    apb_read_word(9'h000, data);
    expect_eq("control reset", data, 0);

    apb_write_word(9'h000, 32'h0000_0005);
    apb_read_word(9'h000, data);
    expect_eq("control readback", data, 32'h0000_0005);

    // Matches the generated model's combined enable/mode field update.
    apb_write_word(9'h000, 32'h0000_0003);
    apb_read_word(9'h004, data);
    expect_eq("live status", data, 32'h0000_0003);

    apb_write_word(9'h028, 32'h0000_1234);
    apb_read_word(9'h028, data);
    expect_eq("threshold readback", data, 32'h0000_1234);

    packet[0] = 32'h1122_3344;
    packet[1] = 32'ha5a5_5a5a;
    packet[2] = 32'hcafe_babe;
    for (int unsigned index = 0; index < 3; index++) begin
      apb_write_word(9'h110 + (index * 4), packet[index]);
    end
    for (int unsigned index = 0; index < 3; index++) begin
      apb_read_word(9'h110 + (index * 4), data);
      expect_eq("scratchpad readback", data, packet[index]);
    end

    $display("PURE_SV_IPXACT_REGFILE_RESULT checks=%0d failures=%0d",
             checks, failures);
    if (failures != 0) $fatal(1, "IP-XACT pure-SV testbench failed");
    $finish;
  end

  initial begin
    #1ms;
    $fatal(1, "IP-XACT pure-SV testbench timed out");
  end

  ipxact_regfile i_dut (.*);
endmodule
