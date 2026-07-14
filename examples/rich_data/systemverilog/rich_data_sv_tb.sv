module rich_data_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  import rich_data_types_pkg::*;

  bit [136:0] wide_i;
  bit [136:0] wide_o;
  logic signed [15:0] fixed_a_i;
  logic signed [15:0] fixed_b_i;
  logic signed [15:0] fixed_y_o;
  logic [31:0] scalar_array_i [1:3];
  logic [31:0] scalar_array_o [1:3];
  bit [64:0] matrix_i [2:1][-1:1];
  bit [64:0] matrix_o [2:1][-1:1];
  packet_t packet_i;
  packet_t packet_o;

  longint unsigned checks;
  int unsigned failures;

  task automatic expect_bits(
      input string label,
      input bit [136:0] actual,
      input bit [136:0] expected
  );
    checks++;
    if (actual !== expected) begin
      failures++;
      $error("%s: expected 0x%0h, got 0x%0h", label, expected, actual);
    end
  endtask

  initial begin
    bit [136:0] expected_wide;
    checks = 0;
    failures = 0;

    wide_i = 137'h1ab_01234567_89abcdef_deadbeef_c001d00d;
    wide_i[63:48] = 16'hbeef;
    fixed_a_i = 16'h6000;
    fixed_b_i = 16'h2000;

    for (int index = 1; index <= 3; index++) begin
      scalar_array_i[index] = 32'h1020_3040 + index;
    end
    for (int row = 2; row >= 1; row--) begin
      for (int column = -1; column <= 1; column++) begin
        automatic int unsigned ordinal = (row - 1) * 3 + column + 1;
        matrix_i[row][column] = {
          bit'(ordinal & 1),
          32'h5060_7080 + ordinal,
          32'h1020_3040 + ordinal
        };
      end
    end

    packet_i = '0;
    packet_i.opcode = 3'd5;
    packet_i.mode = MODE_RUN;
    packet_i.inner.tag = 2'd1;
    packet_i.inner.payload = 3'd2;

    #1ps;

    expected_wide = wide_i ^
        137'h155_01234567_5555aaaa_33333333_0f0f0f0f;
    expect_bits("137-bit value", wide_o, expected_wide);
    expect_bits("arbitrary output slice", {121'b0, wide_o[63:48]},
                {121'b0, expected_wide[63:48]});
    expect_bits("Q2.14 multiply", {121'b0, fixed_y_o},
                {121'b0, 16'h3000});

    for (int index = 1; index <= 3; index++) begin
      expect_bits(
          "unpacked array element", {105'b0, scalar_array_o[index]},
          {105'b0, (32'h1020_3040 + index) ^ (32'h0101_0101 * index)});
    end
    for (int row = 2; row >= 1; row--) begin
      for (int column = -1; column <= 1; column++) begin
        automatic int unsigned ordinal = (row - 1) * 3 + column + 1;
        automatic bit [64:0] input_value = {
          bit'(ordinal & 1),
          32'h5060_7080 + ordinal,
          32'h1020_3040 + ordinal
        };
        expect_bits("multidimensional wide array element",
                    {72'b0, matrix_o[row][column]},
                    {72'b0, input_value ^ 65'h1_01234567_89abcdef});
      end
    end

    expect_bits("packed struct opcode", {134'b0, packet_o.opcode},
                {134'b0, 3'd6});
    expect_bits("signed enum", {134'b0, packet_o.mode},
                {134'b0, MODE_RUN});
    expect_bits("nested struct tag", {135'b0, packet_o.inner.tag},
                {135'b0, 2'd2});
    expect_bits("nested struct payload", {134'b0, packet_o.inner.payload},
                {134'b0, 3'd7});

    fixed_a_i = 16'h7000;
    fixed_b_i = 16'h7000;
    #1ps;
    expect_bits("Q2.14 saturation", {121'b0, fixed_y_o},
                {121'b0, 16'h7fff});

    $display(
        "PURE_SV_RICH_DATA_RESULT iterations=%0d checks=%0d sim_cycles=%0d failures=%0d",
        1, checks, 0, failures);
    if (failures != 0) $fatal(1, "rich-data pure-SV testbench failed");
    $finish;
  end

  initial begin
    #1ms;
    $fatal(1, "rich-data pure-SV testbench timed out");
  end

  rich_data i_dut (.*);
endmodule
