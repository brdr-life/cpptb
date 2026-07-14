typedef enum logic [1:0] {
  IDLE,
  ACTIVE,
  DONE
} state_t;

typedef struct packed {
  logic [3:0] tag;
  logic [7:0] payload;
} packet_t;

module hierarchy_leaf #(
    parameter int WIDTH = 8,
    localparam int DOUBLE_WIDTH = WIDTH * 2
) (
    input  logic             clk,
    input  logic             write_enable,
    input  logic [WIDTH-1:0] write_data,
    output logic [WIDTH-1:0] value
);
  logic [WIDTH-1:0] storage;
  logic [15:0] memory [2:5];
  logic [7:0] matrix [1:0] [4:2];
  wire [WIDTH-1:0] inverted = ~storage;
  state_t state;
  packet_t packet;

  assign value = storage;

  always_ff @(posedge clk) begin
    if (write_enable) begin
      storage <= write_data;
      state <= ACTIVE;
      packet <= '{tag: 4'ha, payload: write_data};
    end
  end
endmodule

module hierarchy_catalog (
    input  logic       clk,
    input  logic       write_enable,
    input  logic [7:0] write_data,
    output logic [7:0] value,
    output logic       event_out
);
  logic internal_flag = 1'b0;
  logic [3:0] four_state_value;
  bit [136:0] wide_value;

  always_ff @(posedge clk) begin
    internal_flag <= ~internal_flag;
  end

  assign event_out = internal_flag;

  hierarchy_leaf #(.WIDTH(8)) block1 (
      .clk,
      .write_enable,
      .write_data,
      .value
  );

  for (genvar index = 0; index < 2; ++index) begin : lanes
    logic [7:0] lane_value;
    hierarchy_leaf #(.WIDTH(8)) block2 (
        .clk,
        .write_enable,
        .write_data,
        .value(lane_value)
    );
  end
endmodule
