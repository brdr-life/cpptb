module stream_fifo_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  logic clk;
  logic rst_n;
  logic in_valid;
  logic in_ready;
  logic [31:0] in_data;
  logic out_valid;
  logic out_ready;
  logic [31:0] out_data;

  localparam int unsigned kWordCount = 24;

  longint unsigned checks;
  int unsigned failures;
  longint unsigned sim_cycles;
  int unsigned input_stalls;
  event reset_done;
  mailbox #(logic [31:0]) expected_words;
  mailbox #(logic [31:0]) observed_words;

  task automatic expect_eq(
      input string label,
      input logic [31:0] actual,
      input logic [31:0] expected
  );
    checks++;
    if (actual !== expected) begin
      failures++;
      $error("%s: expected 0x%08x, got 0x%08x", label, expected, actual);
    end
  endtask

  task automatic reset_dut();
    rst_n = 1'b0;
    in_valid = 1'b0;
    in_data = '0;
    out_ready = 1'b0;
    repeat (2) @(posedge clk);
    @(negedge clk);
    rst_n = 1'b1;
    ->reset_done;
  endtask

  task automatic input_driver();
    logic [31:0] state;
    logic [31:0] word;
    @reset_done;
    state = 32'h3141_5926;

    for (int unsigned index = 0; index < kWordCount; index++) begin
      state = state * 32'd1664525 + 32'd1013904223;
      word = state;
      forever begin
        @(negedge clk);
        in_data = word;
        in_valid = 1'b1;
        #1ps;
        if (!in_ready) begin
          input_stalls++;
          continue;
        end
        expected_words.put(word);
        @(posedge clk);
        #1ps;
        in_valid = 1'b0;
        break;
      end
    end
  endtask

  task automatic output_ready_driver();
    int unsigned cycle;
    int unsigned accepted;
    logic ready;
    @reset_done;
    cycle = 0;
    accepted = 0;

    while (accepted < kWordCount) begin
      @(negedge clk);
      ready = !((cycle % 5 == 1) || (cycle % 5 == 2));
      out_ready = ready;
      #2ps;
      if (ready && out_valid) accepted++;
      cycle++;
    end

    @(posedge clk);
    #1ps;
    out_ready = 1'b0;
  endtask

  task automatic output_monitor();
    int unsigned observed;
    @reset_done;
    observed = 0;

    while (observed < kWordCount) begin
      @(negedge clk);
      #1ps;
      if (!out_valid || !out_ready) continue;
      observed_words.put(out_data);
      observed++;
    end
  endtask

  task automatic scoreboard();
    logic [31:0] expected;
    logic [31:0] actual;
    for (int unsigned index = 0; index < kWordCount; index++) begin
      expected_words.get(expected);
      observed_words.get(actual);
      expect_eq("FIFO payload", actual, expected);
    end
  endtask

  initial begin
    clk = 1'b0;
    sim_cycles = 0;
    forever begin
      #5ns clk = ~clk;
      if (clk) sim_cycles++;
    end
  end

  initial begin
    checks = 0;
    failures = 0;
    input_stalls = 0;
    expected_words = new();
    observed_words = new();

    fork
      reset_dut();
      input_driver();
      output_ready_driver();
      output_monitor();
      scoreboard();
    join

    #1ps;
    expect_eq("FIFO drained", out_valid, 0);
    expect_eq("FIFO accepts after drain", in_ready, 1);
    expect_eq("input backpressure observed", input_stalls != 0, 1);

    $display(
        "PURE_SV_FIFO_SCOREBOARD_RESULT iterations=%0d checks=%0d sim_cycles=%0d failures=%0d",
        1, checks, sim_cycles, failures);
    if (failures != 0) $fatal(1, "FIFO scoreboard pure-SV testbench failed");
    $finish;
  end

  initial begin
    #1ms;
    $fatal(1, "FIFO scoreboard pure-SV testbench timed out");
  end

  stream_fifo i_dut (.*);
endmodule
