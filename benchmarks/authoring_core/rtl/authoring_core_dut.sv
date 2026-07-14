package authoring_types_pkg;
  typedef enum bit signed [2:0] {
    STATE_NEGATIVE = -1,
    STATE_IDLE = 0,
    STATE_RUN = 3
  } state_t;

  typedef struct packed {
    bit [1:0] tag;
    bit [2:0] payload;
  } inner_t;

  typedef struct packed {
    bit [2:0] opcode;
    state_t state;
    inner_t inner;
  } packet_t;
endpackage

module authoring_core_dut (
    input  logic        clk,
    input  logic        rst_n,
    input  logic        req_valid,
    input  logic [31:0] req_data,
    output logic        req_ready,
    output logic        rsp_valid,
    output logic [31:0] rsp_data,
    input  logic        rsp_ready,
    output logic        pulse,
    output logic [31:0] request_count,
    output logic [31:0] response_count,
    input  bit [63:0] wide64_i,
    output bit [63:0] wide64_o,
    input  bit [136:0] wide137_i,
    output bit [136:0] wide137_o,
    input  logic signed [15:0] fixed_a_i,
    input  logic signed [15:0] fixed_b_i,
    output logic signed [15:0] fixed_y_o,
    input  logic [31:0] array_i [1:8],
    output logic [31:0] array_o [1:8],
    input  bit [63:0] array_wide_i [3:0],
    output bit [63:0] array_wide_o [3:0],
    input  bit [64:0] array_multidim_i [2:1][-1:1],
    output bit [64:0] array_multidim_o [2:1][-1:1],
    input  bit [31:0] force_source_i,
    output bit [31:0] force_fanout_o,
    input  authoring_types_pkg::packet_t packed_view_i,
    output authoring_types_pkg::packet_t packed_view_o,
    input  logic [7:0] mem_addr_i,
    input  logic [31:0] mem_wdata_i,
    input  logic mem_we_i,
    output logic [31:0] mem_rdata_o
);
  logic pending;
  logic [1:0] delay_count;
  logic [31:0] pending_data;
  logic [31:0] cycle_count;
  logic signed [31:0] fixed_product;
  logic [31:0] fixed_magnitude;
  logic [17:0] fixed_quotient;
  logic [13:0] fixed_remainder;
  logic signed [32:0] fixed_rounded;
  logic [31:0] memory [0:255];
  bit [136:0] hierarchy_wide;
  logic [3:0] hierarchy_logic;
  wire [31:0] force_target = force_source_i ^ 32'h5a5a_a5a5;

  assign req_ready = !pending && !rsp_valid;
  assign pulse = (cycle_count[1:0] == 2'b00);
  assign wide64_o = wide64_i ^ 64'hd1b5_4a32_d192_ed03;
  assign wide137_o = wide137_i ^
      137'h1a5_5aa55aa5_01234567_89abcdef_deadbeef;
  assign force_fanout_o = force_target;

  always_comb begin
    for (int index = 1; index <= 8; index++) begin
      array_o[index] = array_i[index] ^ (32'h6d2b_79f5 + index);
    end
    for (int index = 0; index <= 3; index++) begin
      array_wide_o[index] =
          array_wide_i[index] ^ (64'h9e37_79b9_7f4a_7c15 + 64'(index));
    end
    for (int row = 2; row >= 1; row--) begin
      for (int column = -1; column <= 1; column++) begin
        array_multidim_o[row][column] =
            array_multidim_i[row][column] ^ 65'h1_01234567_89abcdef;
      end
    end
    packed_view_o = packed_view_i;
    packed_view_o.opcode = packed_view_i.opcode ^ 3'b011;
    packed_view_o.inner.tag = packed_view_i.inner.tag + 1'b1;
    packed_view_o.inner.payload = packed_view_i.inner.payload ^ 3'b101;
  end

  always_comb begin
    fixed_product = fixed_a_i * fixed_b_i;
    fixed_magnitude = fixed_product < 0 ? -fixed_product : fixed_product;
    fixed_quotient = fixed_magnitude[31:14];
    fixed_remainder = fixed_magnitude[13:0];
    if ((fixed_remainder > 14'h2000) ||
        ((fixed_remainder == 14'h2000) && fixed_quotient[0])) begin
      fixed_quotient = fixed_quotient + 1'b1;
    end
    fixed_rounded = fixed_product < 0 ?
        -$signed({15'b0, fixed_quotient}) :
         $signed({15'b0, fixed_quotient});
    if (fixed_rounded > 33'sd32767) begin
      fixed_y_o = 16'sh7fff;
    end else if (fixed_rounded < -33'sd32768) begin
      fixed_y_o = -16'sd32768;
    end else begin
      fixed_y_o = fixed_rounded[15:0];
    end
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      pending <= 1'b0;
      delay_count <= '0;
      pending_data <= '0;
      rsp_valid <= 1'b0;
      rsp_data <= '0;
      cycle_count <= '0;
      request_count <= '0;
      response_count <= '0;
      mem_rdata_o <= '0;
    end else begin
      cycle_count <= cycle_count + 1'b1;

      if (req_valid && req_ready) begin
        pending <= 1'b1;
        delay_count <= 2'd1;
        pending_data <= req_data;
        request_count <= request_count + 1'b1;
      end

      if (pending) begin
        if (delay_count != 0) begin
          delay_count <= delay_count - 1'b1;
        end else begin
          pending <= 1'b0;
          rsp_valid <= 1'b1;
          rsp_data <= (pending_data ^ 32'ha5a5_5a5a) + (request_count - 1'b1);
        end
      end

      if (rsp_valid && rsp_ready) begin
        rsp_valid <= 1'b0;
        response_count <= response_count + 1'b1;
      end

      if (mem_we_i) begin
        memory[mem_addr_i] <= mem_wdata_i;
      end else begin
        mem_rdata_o <= memory[mem_addr_i];
      end
    end
  end
endmodule
