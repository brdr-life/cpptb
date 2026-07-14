module hierarchy_catalog_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  logic clk;
  logic write_enable;
  logic [7:0] write_data;
  logic [7:0] value;
  logic event_out;

  longint unsigned checks;
  int unsigned failures;

  task automatic expect_eq(
      input string label,
      input logic [31:0] actual,
      input logic [31:0] expected
  );
    checks++;
    if (actual !== expected) begin
      failures++;
      $error("%s: expected 0x%0h, got 0x%0h", label, expected, actual);
    end
  endtask

  task automatic expect_wide_eq(
      input string label,
      input bit [136:0] actual,
      input bit [136:0] expected
  );
    checks++;
    if (actual != expected) begin
      failures++;
      $error("%s: expected 0x%0h, got 0x%0h", label, expected, actual);
    end
  endtask

  initial begin
    clk = 1'b0;
    forever #5ns clk = ~clk;
  end

  initial begin
    packet_t packet;

    write_enable = 1'b0;
    write_data = '0;
    checks = 0;
    failures = 0;
    @(posedge clk);

    i_dut.block1.storage = 8'h12;
    expect_eq("scalar deposit is immediately readable",
              i_dut.block1.storage, 8'h12);
    #1ps;
    expect_eq("scalar deposit reaches output", value, 8'h12);

    force i_dut.block1.storage = 8'h34;
    expect_eq("scalar force is immediately readable",
              i_dut.block1.storage, 8'h34);
    write_data = 8'h77;
    write_enable = 1'b1;
    @(posedge clk);
    #1ps;
    expect_eq("force overrides RTL assignment", value, 8'h34);
    release i_dut.block1.storage;
    @(posedge clk);
    #1ps;
    expect_eq("RTL assignment resumes after release", value, 8'h77);
    write_enable = 1'b0;

    expect_eq("derived net is readable", i_dut.block1.inverted, 8'h88);
    force i_dut.block1.inverted = 8'h56;
    expect_eq("derived net force is immediately readable",
              i_dut.block1.inverted, 8'h56);
    release i_dut.block1.inverted;

    i_dut.block1.memory[2] = 16'hbeef;
    expect_eq("non-zero memory index deposit",
              i_dut.block1.memory[2], 16'hbeef);
    force i_dut.block1.memory[2] = 16'hcafe;
    expect_eq("memory force is immediately readable",
              i_dut.block1.memory[2], 16'hcafe);
    release i_dut.block1.memory[2];

    i_dut.block1.matrix[1][3] = 8'h5a;
    expect_eq("multidimensional memory index",
              i_dut.block1.matrix[1][3], 8'h5a);

    i_dut.lanes[1].block2.storage = 8'h9c;
    expect_eq("generated instance hierarchy",
              i_dut.lanes[1].block2.storage, 8'h9c);

    i_dut.block1.state = DONE;
    expect_eq("typed enum deposit", i_dut.block1.state, DONE);
    force i_dut.block1.state = ACTIVE;
    expect_eq("typed enum force", i_dut.block1.state, ACTIVE);
    release i_dut.block1.state;

    packet = '{tag: 4'ha, payload: 8'h5c};
    i_dut.block1.packet = packet;
    expect_eq("packed struct tag", i_dut.block1.packet.tag, 4'ha);
    expect_eq("packed struct payload", i_dut.block1.packet.payload, 8'h5c);

    i_dut.block1.storage = 8'ha8;
    expect_eq("fixed-point view", i_dut.block1.storage, 8'ha8);

    i_dut.four_state_value = 4'b1010;
    expect_eq("four-state deposit transport", i_dut.four_state_value,
              4'b1010);
    force i_dut.four_state_value = 4'b0101;
    expect_eq("four-state force transport", i_dut.four_state_value,
              4'b0101);
    release i_dut.four_state_value;

    i_dut.wide_value = 137'h1_00000000_00000000_12345678_9abcdef0;
    expect_wide_eq("wide hierarchy deposit", i_dut.wide_value,
                   137'h1_00000000_00000000_12345678_9abcdef0);
    force i_dut.wide_value =
        137'h0_00000000_00000001_fedcba98_76543210;
    expect_wide_eq("wide hierarchy force", i_dut.wide_value,
                   137'h0_00000000_00000001_fedcba98_76543210);
    release i_dut.wide_value;

    @(posedge event_out);
    expect_eq("top-level output edge", event_out, 1);
    @(posedge i_dut.internal_flag);
    expect_eq("internal hierarchy edge", i_dut.internal_flag, 1);
    @(negedge i_dut.internal_flag);
    expect_eq("internal falling edge", i_dut.internal_flag, 0);
    @(i_dut.internal_flag);
    expect_eq("internal any edge", i_dut.internal_flag, 1);

    $display(
        "PURE_SV_HIERARCHY_CATALOG_RESULT checks=%0d failures=%0d",
        checks, failures);
    if (failures != 0) $fatal(1, "hierarchy catalog pure-SV test failed");
    $finish;
  end

  initial begin
    #1ms;
    $fatal(1, "hierarchy catalog pure-SV test timed out");
  end

  hierarchy_catalog i_dut (.*);
endmodule
