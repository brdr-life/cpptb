module authoring_core_cocotb_top;
  timeunit 1ns;
  timeprecision 1ps;

  logic         clk;
  logic         rst_n;
  logic         req_valid;
  logic [31:0]  req_data;
  logic         req_ready;
  logic         rsp_valid;
  logic [31:0]  rsp_data;
  logic         rsp_ready;
  logic [31:0]  request_count;
  logic [31:0]  response_count;
  bit [136:0]   wide137_i;
  bit [136:0]   wide137_o;

  authoring_core_dut dut (
    .clk,
    .rst_n,
    .req_valid,
    .req_data,
    .req_ready,
    .rsp_valid,
    .rsp_data,
    .rsp_ready,
    .request_count,
    .response_count,
    .wide137_i,
    .wide137_o
  );
endmodule
