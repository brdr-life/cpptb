module heavy_benchmark_vpi_top;
  timeunit 1ns;
  timeprecision 1ps;

  logic clk;
  logic rst_n;
  logic fir_in_valid;
  logic fir_in_ready;
  logic signed [15:0] fir_in_sample;
  logic fir_out_valid;
  logic fir_out_ready;
  logic signed [31:0] fir_out_result;
  logic [31:0] fir_sample_count;
  logic crc_in_valid;
  logic crc_in_ready;
  logic [7:0] crc_in_data;
  logic crc_in_last;
  logic crc_out_valid;
  logic crc_out_ready;
  logic [31:0] crc_out_result;
  logic [31:0] crc_packet_count;
  logic mat_load_valid;
  logic mat_load_ready;
  logic mat_load_select;
  logic [3:0] mat_load_index;
  logic signed [15:0] mat_load_data;
  logic mat_start;
  logic mat_out_valid;
  logic mat_out_ready;
  logic [3:0] mat_out_index;
  logic signed [31:0] mat_out_data;
  logic [31:0] mat_block_count;

  heavy_benchmark_dut dut (.*);
endmodule
