module apb_trace_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  logic clk = 1'b0;
  logic rst_n;
  logic apb_select;
  logic apb_enable;
  logic apb_write;
  logic [7:0] apb_address;
  logic [31:0] apb_write_data;
  logic [31:0] apb_read_data;
  logic apb_ready;
  logic apb_error;

  localparam int unsigned kTransferPairs = 128;
  localparam int unsigned kTransactionCount = kTransferPairs * 2;
  localparam logic [63:0] kAllBytes = '1;

  typedef struct packed {
    logic write;
    logic [7:0] address;
    logic [31:0] data;
    logic [63:0] byte_enable;
    logic error;
    logic [31:0] wait_cycles;
  } transaction_t;

  transaction_t expected_transactions[$];
  transaction_t actual_transactions[$];
  string recorded_streams[$];
  string recorded_types[$];
  longint unsigned recorded_sequences[$];
  time recorded_begin_times[$];
  time recorded_end_times[$];
  int unsigned recorded_dispositions[$];
  string recorded_json[$];

  int unsigned checks;
  int unsigned failures;
  int unsigned compared;
  int unsigned observed_count;
  int unsigned total_wait_cycles;
  bit monitor_active;
  time monitor_started;
  int unsigned monitor_waits;

  apb_trace dut (.*);

  always #5ns clk = ~clk;

  task automatic check_true(input string label, input bit condition);
    checks++;
    if (!condition) begin
      failures++;
      $error("%s", label);
    end
  endtask

  task automatic compare_available();
    transaction_t expected;
    transaction_t actual;
    while (expected_transactions.size() != 0 &&
           actual_transactions.size() != 0) begin
      expected = expected_transactions.pop_front();
      actual = actual_transactions.pop_front();
      checks++;
      if (actual !== expected) begin
        failures++;
        $error("APB trace mismatch at comparison %0d", compared);
      end
      compared++;
    end
  endtask

  task automatic publish_expected(input transaction_t transaction);
    expected_transactions.push_back(transaction);
    compare_available();
  endtask

  task automatic publish_observed(
      input transaction_t transaction,
      input time begin_time,
      input time end_time
  );
    string operation;
    string status;
    actual_transactions.push_back(transaction);
    operation = transaction.write ? "write" : "read";
    status = transaction.error ? "slave_error" : "okay";
    recorded_streams.push_back("apb0.observed");
    recorded_types.push_back("memory_transaction");
    recorded_sequences.push_back(recorded_sequences.size());
    recorded_begin_times.push_back(begin_time);
    recorded_end_times.push_back(end_time);
    recorded_dispositions.push_back(0);
    recorded_json.push_back($sformatf(
        "{\"operation\":\"%s\",\"address\":%0d,\"data\":%0d,\"byte_enable\":%0d,\"status\":\"%s\",\"wait_cycles\":%0d}",
        operation, transaction.address, transaction.data,
        transaction.byte_enable, status, transaction.wait_cycles));
    observed_count++;
    compare_available();
  endtask

  task automatic apb_idle();
    apb_select = 1'b0;
    apb_enable = 1'b0;
    apb_write = 1'b0;
  endtask

  task automatic apb_write_word(
      input logic [7:0] address,
      input logic [31:0] data,
      output logic error,
      output int unsigned wait_cycles
  );
    @(negedge clk);
    apb_address = address;
    apb_write_data = data;
    apb_write = 1'b1;
    apb_select = 1'b1;
    apb_enable = 1'b0;
    @(posedge clk);
    @(negedge clk);
    apb_enable = 1'b1;
    wait_cycles = 0;
    forever begin
      @(posedge clk);
      #1ps;
      if (apb_ready) break;
      wait_cycles++;
    end
    error = apb_error;
    @(negedge clk);
    apb_idle();
  endtask

  task automatic apb_read_word(
      input logic [7:0] address,
      output logic [31:0] data,
      output logic error,
      output int unsigned wait_cycles
  );
    @(negedge clk);
    apb_address = address;
    apb_write = 1'b0;
    apb_select = 1'b1;
    apb_enable = 1'b0;
    @(posedge clk);
    @(negedge clk);
    apb_enable = 1'b1;
    wait_cycles = 0;
    forever begin
      @(posedge clk);
      #1ps;
      if (apb_ready) break;
      wait_cycles++;
    end
    data = apb_read_data;
    error = apb_error;
    @(negedge clk);
    apb_idle();
  endtask

  function automatic logic [31:0] next_word(input logic [31:0] state);
    return state * 32'd1664525 + 32'd1013904223;
  endfunction

  task automatic trace_sequence();
    logic [31:0] state = 32'h1357_9bdf;
    logic [31:0] value;
    logic [31:0] read_data;
    logic [7:0] address;
    logic error;
    int unsigned waits;
    int unsigned expected_waits;

    for (int unsigned index = 0; index < kTransferPairs; index++) begin
      address = ((index * 13) & 63) * 4;
      state = next_word(state);
      value = state;
      expected_waits = address[2] ? 1 : 0;

      apb_write_word(address, value, error, waits);
      total_wait_cycles += waits;
      check_true("APB trace write status", !error);
      publish_expected(
          '{1'b1, address, value, kAllBytes, 1'b0, expected_waits});

      apb_read_word(address, read_data, error, waits);
      total_wait_cycles += waits;
      check_true("APB trace read data", read_data == value);
      check_true("APB trace read status", !error);
      publish_expected(
          '{1'b0, address, read_data, kAllBytes, 1'b0, expected_waits});
    end
  endtask

  task automatic monitor();
    transaction_t transaction;
    monitor_active = 1'b0;
    monitor_waits = 0;
    while (observed_count < kTransactionCount) begin
      @(posedge clk);
      #1ps;
      if (!apb_select) begin
        monitor_active = 1'b0;
        monitor_waits = 0;
      end else if (!apb_enable) begin
        monitor_active = 1'b1;
        monitor_started = $time;
        monitor_waits = 0;
      end else if (!apb_ready) begin
        if (monitor_active) monitor_waits++;
      end else begin
        transaction = '{
            write: apb_write,
            address: apb_address,
            data: apb_write ? apb_write_data : apb_read_data,
            byte_enable: kAllBytes,
            error: apb_error,
            wait_cycles: monitor_waits
        };
        publish_observed(transaction, monitor_started, $time);
        monitor_active = 1'b0;
        monitor_waits = 0;
      end
    end
  endtask

  initial begin
    rst_n = 1'b0;
    total_wait_cycles = 0;
    apb_idle();
    apb_address = '0;
    apb_write_data = '0;
    repeat (2) @(posedge clk);
    // Release at the edge through a non-blocking assignment -- the same
    // schedule the C++ testbench gets from deferred writes.
    rst_n <= 1'b1;

    fork
      trace_sequence();
      monitor();
    join

    check_true("all expected APB transactions matched",
               expected_transactions.size() == 0);
    check_true("all observed APB transactions matched",
               actual_transactions.size() == 0);
    check_true("APB scoreboard comparison count",
               compared == kTransactionCount);
    check_true("recorded APB wait-cycle count", total_wait_cycles == 128);
    check_true("recorded APB transaction count",
               recorded_sequences.size() == kTransactionCount);
    check_true("first APB record sequence", recorded_sequences[0] == 0);
    check_true("last APB record sequence",
               recorded_sequences[kTransactionCount-1] ==
                   kTransactionCount-1);
    check_true("APB record has a nonzero interval",
               recorded_end_times[0] > recorded_begin_times[0]);
    check_true("APB trace retains typed fields",
               recorded_json[0].substr(0, 19) == "{\"operation\":\"write\"");

    if (failures != 0) $fatal(1, "APB trace failed: %0d failures", failures);
    $display(
        "PURE_SV_APB_TRACE_RESULT iterations=1 checks=%0d sim_cycles=%0d failures=0",
        checks, // One cycle of reset overhead, not two: the release moved from
        // the falling edge to a non-blocking assignment at the rising
        // edge, so the first setup phase lands one cycle earlier.
        kTransactionCount * 3 + 1 + total_wait_cycles);
    $finish;
  end
endmodule
