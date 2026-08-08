module stalling_responder_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  logic clk;
  logic rst_n;
  logic request;
  logic stall;
  logic [7:0] latency;
  logic [31:0] request_data;
  logic response_valid;
  logic [31:0] response_data;
  logic busy;

  localparam int unsigned kTransactionCount = 8;

  longint unsigned checks;
  int unsigned failures;
  longint unsigned sim_cycles;
  logic last_completed;
  logic [31:0] last_result;
  logic last_triggered;

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
    request = 1'b0;
    stall = 1'b0;
    latency = '0;
    request_data = '0;
    repeat (2) @(posedge clk);
    // Drive at the edge through non-blocking assignments -- the same
    // schedule the C++ testbench gets from deferred writes.
    rst_n <= 1'b1;
  endtask

  task automatic drive_request(
      input logic [31:0] data,
      input logic [7:0] request_latency,
      input logic should_stall
  );
    @(posedge clk);
    request_data <= data;
    latency <= request_latency;
    stall <= should_stall;
    request <= 1'b1;
    @(posedge clk);
    request <= 1'b0;
  endtask

  task automatic timed_transaction(
      input logic [31:0] data,
      input logic [7:0] request_latency,
      input logic should_stall,
      input time timeout_duration
  );
    logic response_finished;
    response_finished = 1'b0;
    last_completed = 1'b0;
    last_result = '0;

    fork : transaction_race
      begin
        drive_request(data, request_latency, should_stall);
        @(posedge response_valid);
        #1ps;
        last_result = response_data;
        response_finished = 1'b1;
      end
      begin
        #(timeout_duration);
      end
    join_any
    disable transaction_race;
    last_completed = response_finished;
  endtask

  task automatic timed_response_edge(
      input time timeout_duration
  );
    logic edge_finished;
    edge_finished = 1'b0;
    fork : edge_race
      begin
        @(posedge response_valid);
        edge_finished = 1'b1;
      end
      begin
        #(timeout_duration);
      end
    join_any
    disable edge_race;
    last_triggered = edge_finished;
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
    logic [31:0] state;
    logic [31:0] word;
    logic monitor_completed;

    checks = 0;
    failures = 0;
    reset_dut();
    state = 32'h5566_7788;

    for (int unsigned index = 0; index < kTransactionCount; index++) begin
      state = state * 32'd1664525 + 32'd1013904223;
      word = state;
      timed_transaction(word, 2 + index % 3, 1'b0, 200ns);
      expect_eq("transaction completed", last_completed, 1);
      if (last_completed) begin
        expect_eq("transaction response", last_result,
                  word ^ 32'ha5a5_5a5a);
      end
    end

    state = state * 32'd1664525 + 32'd1013904223;
    word = state;
    drive_request(word, 3, 1'b0);
    timed_response_edge(100ns);
    expect_eq("response edge beat deadline", last_triggered, 1);
    #1ps;
    expect_eq("response after edge timeout", response_data,
              word ^ 32'ha5a5_5a5a);

    state = state * 32'd1664525 + 32'd1013904223;
    word = state;
    timed_transaction(word, 2, 1'b1, 60ns);
    expect_eq("stalled transaction timed out", last_completed, 0);

    monitor_completed = 1'b0;
    // The stalled DUT cannot respond during this window. This named process
    // is the pure-SV counterpart of cancelling the parked C++ monitor.
    fork : dormant_monitor
      begin
        @(posedge response_valid);
        monitor_completed = 1'b1;
      end
    join_none
    #3ns;
    disable dormant_monitor;
    expect_eq("cancelled monitor reports cancellation",
              monitor_completed, 0);

    $display(
        "PURE_SV_WATCHDOG_TIMEOUT_RESULT iterations=%0d checks=%0d sim_cycles=%0d failures=%0d",
        1, checks, sim_cycles, failures);
    if (failures != 0) $fatal(1, "watchdog timeout pure-SV testbench failed");
    $finish;
  end

  initial begin
    #5ms;
    $fatal(1, "watchdog timeout pure-SV testbench timed out");
  end

  stalling_responder i_dut (.*);
endmodule
