module scheduler_conformance (
    input  logic       rst_n,
    input  logic       clk_a,
    input  logic       clk_b,
    input  logic       manual_clk,
    input  logic       derived_gate,
    input  logic       stable_signal,
    input  logic       predicate_signal,
    input  logic       event_drive,
    input  logic [7:0] force_net_source,
    input  logic [7:0] drive_value,
    input  logic [7:0] addend,
    input  bit [64:0] packed65_i,
    output bit [64:0] packed65_o,
    input  bit [136:0] packed137_i,
    output bit [136:0] packed137_o,
    input  bit [72:0] array73_i [7:4],
    output bit [72:0] array73_o [7:4],
    input  bit [64:0] matrix65_i [2:1] [-1:1],
    output bit [64:0] matrix65_o [2:1] [-1:1],
    output logic       derived_clk,
    output logic       event_observed,
    output logic [7:0] comb_sum,
    output logic [7:0] sampled_a,
    output logic [7:0] sampled_b,
    output logic [7:0] sampled_manual,
    output logic [7:0] sampled_derived,
    output logic [7:0] count_a,
    output logic [7:0] count_b,
    output logic [7:0] count_manual,
    output logic [7:0] count_derived,
    output bit [63:0] internal_comb_fanout,
    output bit [63:0] internal_clocked_fanout,
    output logic [7:0] internal_net_fanout
);
  assign comb_sum = drive_value + addend;
  assign derived_clk = clk_a & derived_gate;
  assign event_observed = event_drive;
  assign packed65_o = packed65_i ^ 65'h1_01234567_89abcdef;
  assign packed137_o = packed137_i ^
      137'h155_aa55aa55_01234567_89abcdef_deadbeef;

  bit [72:0] internal_wide;
  bit [63:0] internal_u64;
  bit [72:0] internal_memory [7:4];
  bit [72:0] force_variable_wide;
  bit [63:0] force_variable_u64;
  bit [72:0] force_memory [7:4];
  logic [31:0] force_counter;
  wire [7:0] internal_net = force_net_source ^ 8'h5a;

  assign internal_comb_fanout = internal_u64;
  assign internal_net_fanout = internal_net;

  always_comb begin
    for (int index = 4; index <= 7; index++) begin
      array73_o[index] =
          array73_i[index] ^ 73'h1ff_01234567_89abcdef;
    end
    for (int row = 1; row <= 2; row++) begin
      for (int column = -1; column <= 1; column++) begin
        matrix65_o[row][column] =
            matrix65_i[row][column] ^ 65'h1_01234567_89abcdef;
      end
    end
  end

  always_ff @(posedge clk_a or negedge rst_n) begin
    if (!rst_n) begin
      sampled_a <= '0;
      count_a <= '0;
      internal_clocked_fanout <= '0;
      force_counter <= '0;
    end else begin
      sampled_a <= drive_value;
      count_a <= count_a + 1'b1;
      internal_clocked_fanout <= internal_u64;
      force_counter <= force_counter + 1'b1;
    end
  end

  always_ff @(posedge clk_b or negedge rst_n) begin
    if (!rst_n) begin
      sampled_b <= '0;
      count_b <= '0;
    end else begin
      sampled_b <= drive_value;
      count_b <= count_b + 1'b1;
    end
  end

  always_ff @(posedge manual_clk or negedge rst_n) begin
    if (!rst_n) begin
      sampled_manual <= '0;
      count_manual <= '0;
    end else begin
      sampled_manual <= drive_value;
      count_manual <= count_manual + 1'b1;
    end
  end

  always_ff @(posedge derived_clk or negedge rst_n) begin
    if (!rst_n) begin
      sampled_derived <= '0;
      count_derived <= '0;
    end else begin
      sampled_derived <= drive_value;
      count_derived <= count_derived + 1'b1;
    end
  end

  logic unused_stable;
  logic unused_predicate;
  assign unused_stable = stable_signal;
  assign unused_predicate = predicate_signal;
endmodule
