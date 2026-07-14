module heavy_benchmark_dut (
    input  logic clk,
    input  logic rst_n,

    input  logic               fir_in_valid,
    output logic               fir_in_ready,
    input  logic signed [15:0] fir_in_sample,
    output logic               fir_out_valid,
    input  logic               fir_out_ready,
    output logic signed [31:0] fir_out_result,
    output logic [31:0]        fir_sample_count,

    input  logic        crc_in_valid,
    output logic        crc_in_ready,
    input  logic [7:0]  crc_in_data,
    input  logic        crc_in_last,
    output logic        crc_out_valid,
    input  logic        crc_out_ready,
    output logic [31:0] crc_out_result,
    output logic [31:0] crc_packet_count,

    input  logic               mat_load_valid,
    output logic               mat_load_ready,
    input  logic               mat_load_select,
    input  logic [3:0]         mat_load_index,
    input  logic signed [15:0] mat_load_data,
    input  logic               mat_start,
    output logic               mat_out_valid,
    input  logic               mat_out_ready,
    output logic [3:0]         mat_out_index,
    output logic signed [31:0] mat_out_data,
    output logic [31:0]        mat_block_count
);
  localparam int FirTaps = 32;

  logic signed [15:0] fir_history [0:FirTaps-1];
  logic signed [63:0] fir_accumulator;

  logic [31:0] crc_state;
  logic [31:0] crc_next;

  logic signed [15:0] matrix_a [0:15];
  logic signed [15:0] matrix_b [0:15];
  logic [3:0] mat_compute_index;
  logic mat_busy;
  logic signed [63:0] mat_accumulator;

  function automatic integer signed fir_coefficient(input integer index);
    fir_coefficient = ((index * 7) % 19) - 9;
  endfunction

  function automatic logic [31:0] crc32_byte(
      input logic [31:0] current,
      input logic [7:0] data
  );
    logic [31:0] value;
    value = current ^ data;
    for (int bit_index = 0; bit_index < 8; bit_index++) begin
      value = value[0] ? ((value >> 1) ^ 32'hedb8_8320) : (value >> 1);
    end
    return value;
  endfunction

  assign fir_in_ready = !fir_out_valid || fir_out_ready;
  assign crc_in_ready = !crc_out_valid || crc_out_ready;
  assign mat_load_ready = !mat_busy && !mat_out_valid;

  always_comb begin
    fir_accumulator = $signed(fir_in_sample) * fir_coefficient(0);
    for (int tap = 1; tap < FirTaps; tap++) begin
      fir_accumulator += $signed(fir_history[tap - 1]) *
                         fir_coefficient(tap);
    end
  end

  always_comb begin
    crc_next = crc32_byte(crc_state, crc_in_data);
  end

  always_comb begin
    int row;
    int column;
    row = mat_compute_index / 4;
    column = mat_compute_index % 4;
    mat_accumulator = 0;
    for (int element = 0; element < 4; element++) begin
      mat_accumulator += $signed(matrix_a[row * 4 + element]) *
                         $signed(matrix_b[element * 4 + column]);
    end
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      fir_out_valid <= 1'b0;
      fir_out_result <= '0;
      fir_sample_count <= '0;
      for (int tap = 0; tap < FirTaps; tap++) fir_history[tap] <= '0;

      crc_state <= 32'hffff_ffff;
      crc_out_valid <= 1'b0;
      crc_out_result <= '0;
      crc_packet_count <= '0;

      mat_compute_index <= '0;
      mat_busy <= 1'b0;
      mat_out_valid <= 1'b0;
      mat_out_index <= '0;
      mat_out_data <= '0;
      mat_block_count <= '0;
      for (int index = 0; index < 16; index++) begin
        matrix_a[index] <= '0;
        matrix_b[index] <= '0;
      end
    end else begin
      if (fir_out_valid && fir_out_ready) fir_out_valid <= 1'b0;
      if (fir_in_valid && fir_in_ready) begin
        for (int tap = FirTaps - 1; tap > 0; tap--)
          fir_history[tap] <= fir_history[tap - 1];
        fir_history[0] <= fir_in_sample;
        fir_out_result <= fir_accumulator[31:0];
        fir_out_valid <= 1'b1;
        fir_sample_count <= fir_sample_count + 1'b1;
      end

      if (crc_out_valid && crc_out_ready) crc_out_valid <= 1'b0;
      if (crc_in_valid && crc_in_ready) begin
        if (crc_in_last) begin
          crc_out_result <= ~crc_next;
          crc_out_valid <= 1'b1;
          crc_state <= 32'hffff_ffff;
          crc_packet_count <= crc_packet_count + 1'b1;
        end else begin
          crc_state <= crc_next;
        end
      end

      if (mat_load_valid && mat_load_ready) begin
        if (mat_load_select)
          matrix_b[mat_load_index] <= mat_load_data;
        else
          matrix_a[mat_load_index] <= mat_load_data;
      end

      if (mat_start && mat_load_ready) begin
        mat_busy <= 1'b1;
        mat_compute_index <= '0;
      end

      if (mat_out_valid && mat_out_ready) mat_out_valid <= 1'b0;
      if (mat_busy && (!mat_out_valid || mat_out_ready)) begin
        mat_out_index <= mat_compute_index;
        mat_out_data <= mat_accumulator[31:0];
        mat_out_valid <= 1'b1;
        if (mat_compute_index == 4'd15) begin
          mat_busy <= 1'b0;
          mat_block_count <= mat_block_count + 1'b1;
        end else begin
          mat_compute_index <= mat_compute_index + 1'b1;
        end
      end
    end
  end
endmodule
