module component_fifo_sv_tb;
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
  int unsigned scoreboard_comparisons;
  int unsigned audit_transactions;
  event reset_done;
  logic [31:0] expected_words[$];
  logic [31:0] observed_words[$];
  mailbox #(logic [31:0]) audit_words;

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
    // Release at the edge through a non-blocking assignment -- the same
    // schedule the C++ testbench gets from deferred writes.
    rst_n <= 1'b1;
    ->reset_done;
  endtask

  task automatic input_sequence();
    logic [31:0] state;
    logic [31:0] word;
    @reset_done;
    state = 32'h3141_5926;

    for (int unsigned index = 0; index < kWordCount; index++) begin
      state = state * 32'd1664525 + 32'd1013904223;
      word = state;
      expected_words.push_back(word);
      // Assert after the rising edge through non-blocking assignments --
      // the same schedule the C++ driver gets from deferred writes -- and
      // hold valid through the stalls.
      @(posedge clk);
      in_data <= word;
      in_valid <= 1'b1;
      forever begin
        @(posedge clk);
        #1ps;
        if (!in_ready) begin
          input_stalls++;
          continue;
        end
        in_valid <= 1'b0;
        break;
      end
    end
  endtask

  task automatic output_ready_driver();
    int unsigned cycle;
    int unsigned accepted;
    logic pending;
    @reset_done;
    cycle = 0;
    accepted = 0;
    pending = 1'b0;

    // Drive ready at the rising edge through a non-blocking assignment --
    // the same schedule the C++ testbench gets from deferred writes -- and
    // count each handshake at the edge that commits it.
    while (accepted < kWordCount) begin
      @(posedge clk);
      if (pending) accepted++;
      out_ready <= cycle % 5 == 0;
      cycle++;
      @(negedge clk);
      pending = out_ready && out_valid;
    end

    @(posedge clk);
    out_ready <= 1'b0;
  endtask

  task automatic output_monitor();
    logic [31:0] expected;
    logic [31:0] actual;
    int unsigned observed;
    @reset_done;
    observed = 0;

    while (observed < kWordCount) begin
      @(negedge clk);
      #1ps;
      if (!out_valid || !out_ready) continue;

      observed_words.push_back(out_data);
      expected = expected_words.pop_front();
      actual = observed_words.pop_front();
      expect_eq("FIFO payload", actual, expected);
      scoreboard_comparisons++;
      if (!audit_words.try_put(out_data)) begin
        $fatal(1, "component FIFO audit buffer is full");
      end
      observed++;
    end
  endtask

  task automatic audit_stream();
    logic [31:0] state;
    logic [31:0] actual;
    state = 32'h3141_5926;
    for (int unsigned index = 0; index < kWordCount; index++) begin
      audit_words.get(actual);
      state = state * 32'd1664525 + 32'd1013904223;
      expect_eq("audit payload", actual, state);
      audit_transactions++;
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
    scoreboard_comparisons = 0;
    audit_transactions = 0;
    audit_words = new(4);

    fork
      reset_dut();
      input_sequence();
      output_ready_driver();
      output_monitor();
      audit_stream();
    join

    expect_eq("FIFO payload expected pending", expected_words.size(), 0);
    expect_eq("FIFO payload actual pending", observed_words.size(), 0);
    expect_eq("scoreboard comparisons", scoreboard_comparisons, kWordCount);
    expect_eq("audit transactions", audit_transactions, kWordCount);
    expect_eq("audit buffer drained", audit_words.num(), 0);
    expect_eq("FIFO drained", out_valid, 0);
    expect_eq("FIFO accepts after drain", in_ready, 1);
    expect_eq("input backpressure observed", input_stalls != 0, 1);

    $display(
        "PURE_SV_COMPONENT_FIFO_RESULT iterations=%0d checks=%0d sim_cycles=%0d failures=%0d",
        1, checks, sim_cycles, failures);
    if (failures != 0) $fatal(1, "component FIFO pure-SV testbench failed");
    $finish;
  end

  initial begin
    #1ms;
    $fatal(1, "component FIFO pure-SV testbench timed out");
  end

  component_fifo i_dut (.*);
endmodule
