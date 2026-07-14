module open_cores_benchmark_top;
  timeunit 1ns;
  timeprecision 1ps;

  logic        clk;
  logic        rst_n;
  logic        cpu_prog_we;
  logic [31:0] cpu_prog_addr;
  logic [31:0] cpu_prog_data;
  logic        cpu_done;
  logic        cpu_trap;
  logic [31:0] cpu_result;
  logic [31:0] cpu_instruction_count;
  logic        aes_cs;
  logic        aes_we;
  logic [7:0]  aes_address;
  logic [31:0] aes_write_data;
  logic [31:0] aes_read_data;
  logic [63:0] fcs_tdata;
  logic [7:0]  fcs_tkeep;
  logic        fcs_tvalid;
  logic        fcs_tready;
  logic        fcs_tlast;
  logic [31:0] fcs_result;
  logic        fcs_valid;

  open_cores_benchmark_dut dut (.*);
endmodule
