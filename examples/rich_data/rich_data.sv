package rich_data_types_pkg;
  typedef enum bit signed [2:0] {
    MODE_NEGATIVE = -1,
    MODE_IDLE = 0,
    MODE_RUN = 3
  } mode_t;

  typedef struct packed {
    bit [1:0] tag;
    bit [2:0] payload;
  } inner_t;

  typedef struct packed {
    bit [2:0] opcode;
    mode_t mode;
    inner_t inner;
  } packet_t;
endpackage

module rich_data (
    input  bit [136:0] wide_i,
    output bit [136:0] wide_o,
    input  logic signed [15:0] fixed_a_i,
    input  logic signed [15:0] fixed_b_i,
    output logic signed [15:0] fixed_y_o,
    input  logic [31:0] scalar_array_i [1:3],
    output logic [31:0] scalar_array_o [1:3],
    input  bit [64:0] matrix_i [2:1][-1:1],
    output bit [64:0] matrix_o [2:1][-1:1],
    input  rich_data_types_pkg::packet_t packet_i,
    output rich_data_types_pkg::packet_t packet_o
);
  logic signed [31:0] fixed_product;
  logic [31:0] fixed_magnitude;
  logic [17:0] fixed_quotient;
  logic [13:0] fixed_remainder;
  logic signed [32:0] fixed_rounded;

  assign wide_o = wide_i ^
      137'h155_01234567_5555aaaa_33333333_0f0f0f0f;

  always_comb begin
    for (int index = 1; index <= 3; index++) begin
      scalar_array_o[index] =
          scalar_array_i[index] ^ (32'h0101_0101 * index);
    end
    for (int row = 2; row >= 1; row--) begin
      for (int column = -1; column <= 1; column++) begin
        matrix_o[row][column] =
            matrix_i[row][column] ^ 65'h1_01234567_89abcdef;
      end
    end

    packet_o = packet_i;
    packet_o.opcode = packet_i.opcode ^ 3'b011;
    packet_o.inner.tag = packet_i.inner.tag + 1'b1;
    packet_o.inner.payload = packet_i.inner.payload ^ 3'b101;
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
endmodule
