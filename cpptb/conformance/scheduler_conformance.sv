module scheduler_conformance (
    input  logic       rst_n,
    input  logic       clk_a,
    input  logic       clk_b,
    input  logic       manual_clk,
    input  logic       derived_gate,
    input  logic       stable_signal,
    input  logic       predicate_signal,
    input  logic [7:0] drive_value,
    input  logic [7:0] addend,
    output logic       derived_clk,
    output logic [7:0] comb_sum,
    output logic [7:0] sampled_a,
    output logic [7:0] sampled_b,
    output logic [7:0] sampled_manual,
    output logic [7:0] sampled_derived,
    output logic [7:0] count_a,
    output logic [7:0] count_b,
    output logic [7:0] count_manual,
    output logic [7:0] count_derived
);
  assign comb_sum = drive_value + addend;
  assign derived_clk = clk_a & derived_gate;

  always_ff @(posedge clk_a or negedge rst_n) begin
    if (!rst_n) begin
      sampled_a <= '0;
      count_a <= '0;
    end else begin
      sampled_a <= drive_value;
      count_a <= count_a + 1'b1;
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
