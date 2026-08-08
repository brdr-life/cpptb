module apb_regfile_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  logic clk;
  logic rst_n;
  logic apb_select;
  logic apb_enable;
  logic apb_write;
  logic [7:0] apb_address;
  logic [31:0] apb_write_data;
  logic [31:0] apb_read_data;
  logic apb_ready;
  logic apb_error;

  localparam int unsigned kRegisterTransactions = 12;
  localparam int unsigned kObservedTransactions =
      kRegisterTransactions * 2 + 2;
  localparam logic [7:0] kIdAddress = 8'h10;
  localparam logic [31:0] kIdValue = 32'h4350_5054;
  localparam logic [63:0] kAllBytes = '1;

  typedef struct packed {
    logic write;
    logic [7:0] address;
    logic [31:0] data;
    logic [63:0] byte_enable;
    logic error;
    logic [31:0] wait_cycles;
  } transaction_t;

  longint unsigned checks;
  int unsigned failures;
  longint unsigned sim_cycles;
  bit test_started;

  transaction_t expected_transactions[$];
  transaction_t actual_transactions[$];
  int unsigned scoreboard_comparisons;
  int unsigned observed_transactions;
  int unsigned protocol_violations;
  int unsigned coverage_samples;
  int unsigned coverage_operation[2];
  int unsigned coverage_status[2];
  int unsigned coverage_cross[2][2];

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
        $error(
            "APB transaction mismatch: expected write=%0d address=0x%02x data=0x%08x bytes=0x%016x error=%0d waits=%0d, got write=%0d address=0x%02x data=0x%08x bytes=0x%016x error=%0d waits=%0d",
            expected.write, expected.address, expected.data,
            expected.byte_enable, expected.error, expected.wait_cycles,
            actual.write, actual.address, actual.data, actual.byte_enable,
            actual.error, actual.wait_cycles);
      end
      scoreboard_comparisons++;
    end
  endtask

  task automatic record_expected(input transaction_t transaction);
    expected_transactions.push_back(transaction);
    compare_available();
  endtask

  task automatic record_observed(input transaction_t transaction);
    int unsigned operation_index;
    int unsigned status_index;

    actual_transactions.push_back(transaction);
    observed_transactions++;

    operation_index = transaction.write ? 1 : 0;
    status_index = transaction.error ? 1 : 0;
    coverage_samples++;
    coverage_operation[operation_index]++;
    coverage_status[status_index]++;
    coverage_cross[operation_index][status_index]++;

    compare_available();
  endtask

  task automatic protocol_report(
      input bit condition,
      input string message
  );
    if (!condition) begin
      protocol_violations++;
      checks++;
      failures++;
      $error("%s", message);
    end
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

  task automatic reset_dut();
    rst_n = 1'b0;
    apb_idle();
    apb_address = '0;
    apb_write_data = '0;
    repeat (2) @(posedge clk);
    // Release at the edge through a non-blocking assignment -- the same
    // schedule the C++ testbench gets from deferred writes.
    rst_n <= 1'b1;
    test_started = 1'b1;
  endtask

  task automatic apb_monitor();
    bit active;
    int unsigned wait_cycles;
    transaction_t transaction;

    wait(test_started);
    active = 0;
    wait_cycles = 0;
    while (observed_transactions < kObservedTransactions) begin
      @(posedge clk);
      #1ps;

      if (!apb_select) begin
        active = 0;
        wait_cycles = 0;
      end else if (!apb_enable) begin
        active = 1;
        wait_cycles = 0;
      end else if (!apb_ready) begin
        if (active) wait_cycles++;
      end else begin
        transaction = '{
            write: apb_write,
            address: apb_address,
            data: apb_write ? apb_write_data : apb_read_data,
            byte_enable: kAllBytes,
            error: apb_error,
            wait_cycles: wait_cycles
        };
        record_observed(transaction);
        active = 0;
        wait_cycles = 0;
      end
    end
  endtask

  task automatic apb_protocol_checker();
    bit setup_seen;
    bit waiting;
    logic [7:0] captured_address;
    logic captured_write;
    logic [31:0] captured_write_data;
    bit selected;
    bit enabled;
    bit ready;
    bit access;

    wait(test_started);
    setup_seen = 0;
    waiting = 0;
    forever begin
      @(posedge clk);
      #1ps;

      selected = apb_select;
      enabled = apb_enable;
      ready = apb_ready;
      access = selected && enabled;

      protocol_report(!enabled || selected, "APB PENABLE requires PSEL");
      protocol_report(!access || setup_seen || waiting,
                      "APB access phase requires a setup phase");

      if (waiting) begin
        protocol_report(selected, "APB PSEL dropped before PREADY");
        protocol_report(enabled, "APB PENABLE dropped before PREADY");
        protocol_report(apb_address == captured_address,
                        "APB PADDR changed while waiting for PREADY");
        protocol_report(apb_write == captured_write,
                        "APB PWRITE changed while waiting for PREADY");
        protocol_report(!captured_write ||
                            apb_write_data == captured_write_data,
                        "APB PWDATA changed while waiting for PREADY");
        protocol_report(1'b1, "APB PSTRB changed while waiting for PREADY");
      end

      if (selected && !enabled) begin
        captured_address = apb_address;
        captured_write = apb_write;
        captured_write_data = apb_write_data;
        setup_seen = 1;
        waiting = 0;
      end else if (access) begin
        if (setup_seen) begin
          protocol_report(apb_address == captured_address,
                          "APB PADDR changed between setup and access");
          protocol_report(apb_write == captured_write,
                          "APB PWRITE changed between setup and access");
          protocol_report(!captured_write ||
                              apb_write_data == captured_write_data,
                          "APB PWDATA changed between setup and access");
          protocol_report(1'b1,
                          "APB PSTRB changed between setup and access");
        end
        waiting = !ready;
        setup_seen = 0;
      end else begin
        waiting = 0;
        setup_seen = 0;
      end
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
    logic [31:0] state;
    logic [31:0] value;
    logic [31:0] read_data;
    logic [7:0] address;
    logic error;
    int unsigned wait_cycles;
    transaction_t expected;

    checks = 0;
    failures = 0;
    test_started = 0;
    scoreboard_comparisons = 0;
    observed_transactions = 0;
    protocol_violations = 0;
    coverage_samples = 0;
    coverage_operation = '{default: 0};
    coverage_status = '{default: 0};
    coverage_cross = '{default: 0};

    fork
      reset_dut();
      apb_monitor();
      apb_protocol_checker();
    join_none
    wait(test_started);

    state = 32'h1020_3040;
    for (int unsigned index = 0; index < kRegisterTransactions; index++) begin
      state = state * 32'd1664525 + 32'd1013904223;
      value = state;
      address = (index % 4) * 4;

      apb_write_word(address, value, error, wait_cycles);
      expect_eq("APB write status", error, 0);
      expected = '{1'b1, address, value, kAllBytes, 1'b0, wait_cycles};
      record_expected(expected);

      apb_read_word(address, read_data, error, wait_cycles);
      expect_eq("APB register readback", read_data, value);
      expect_eq("APB read status", error, 0);
      expected = '{1'b0, address, value, kAllBytes, 1'b0, wait_cycles};
      record_expected(expected);
    end

    apb_read_word(kIdAddress, read_data, error, wait_cycles);
    expect_eq("APB read-only ID", read_data, kIdValue);
    expect_eq("APB ID status", error, 0);
    expected = '{1'b0, kIdAddress, kIdValue, kAllBytes, 1'b0, wait_cycles};
    record_expected(expected);

    apb_read_word(8'hfc, read_data, error, wait_cycles);
    expect_eq("APB unmapped read data", read_data, 0);
    expect_eq("APB unmapped status", error, 1);
    expected = '{1'b0, 8'hfc, 32'd0, kAllBytes, 1'b1, wait_cycles};
    record_expected(expected);

    wait(observed_transactions == kObservedTransactions);
    expect_eq("APB transaction expected pending",
              expected_transactions.size(), 0);
    expect_eq("APB transaction actual pending", actual_transactions.size(), 0);
    expect_eq("APB monitored transactions", scoreboard_comparisons,
              kObservedTransactions);
    expect_eq("APB protocol violations", protocol_violations, 0);
    expect_eq("APB coverage samples", coverage_samples,
              kObservedTransactions);
    expect_eq("APB operation coverage", coverage_operation[0],
              kRegisterTransactions + 2);

    $display(
        "PURE_SV_APB_REGFILE_RESULT iterations=%0d checks=%0d sim_cycles=%0d failures=%0d",
        1, checks, sim_cycles, failures);
    if (failures != 0) $fatal(1, "APB regfile pure-SV testbench failed");
    $finish;
  end

  initial begin
    #1ms;
    $fatal(1, "APB regfile pure-SV testbench timed out");
  end

  apb_regfile i_dut (.*);
endmodule
