module authoring_core_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  logic clk;
  logic rst_n;
  logic req_valid;
  logic [31:0] req_data;
  logic req_ready;
  logic rsp_valid;
  logic [31:0] rsp_data;
  logic rsp_ready;
  logic pulse;
  logic [31:0] request_count;
  logic [31:0] response_count;

  int unsigned iterations;
  string kernel;
  longint unsigned checks;
  longint unsigned sim_cycles;
  int unsigned failures;
  int unsigned transactions;
  logic [31:0] checksum;
  longint unsigned task_value_count;
  longint unsigned clock_cycles_count;
  longint unsigned timeout_count;
  longint unsigned timeout_hits;
  longint unsigned task_timeout_count;
  longint unsigned task_timeout_hits;
  longint unsigned wait_until_count;
  longint unsigned event_set_count;
  longint unsigned event_wait_count;
  longint unsigned channel_send_count;
  longint unsigned channel_receive_count;
  event authored_event;
  bit authored_event_set;
  event channel_available;
  logic [31:0] event_token;
  logic [31:0] channel_queue[$];

  always #1ns clk = ~clk;
  always @(posedge clk) sim_cycles++;

  function automatic logic [31:0] stimulus(input int unsigned iteration);
    return ((iteration + 1) * 32'h1f12_3bb5) ^ 32'hc001_d00d;
  endfunction

  function automatic logic [31:0] expected_response(input int unsigned iteration);
    return (stimulus(iteration) ^ 32'ha5a5_5a5a) + iteration;
  endfunction

  task automatic check32(input logic [31:0] actual,
                         input logic [31:0] expected,
                         input string label);
    checks++;
    if (actual == expected) return;
    failures++;
    if (failures <= 8) begin
      $display("AUTHORING_CORE_MISMATCH mode=pure_sv kernel=%s label=%s actual=0x%08x expected=0x%08x",
               kernel, label, actual, expected);
    end
  endtask

  task automatic wait_ready_raw();
    while (!req_ready) @(posedge clk);
  endtask

  task automatic transact(input int unsigned iteration,
                          input logic [31:0] payload,
                          input bit ready_already);
    if (!ready_already) wait_ready_raw();
    @(negedge clk);
    req_data = payload;
    req_valid = 1'b1;
    @(posedge clk);
    @(negedge clk);
    req_valid = 1'b0;
    forever begin
      @(posedge clk);
      #1ps;
      if (rsp_valid) break;
    end
    check32(rsp_data, expected_response(iteration), "response");
    checksum = (checksum ^ rsp_data) * 32'h0100_0193;
    transactions++;
  endtask

  task automatic task_value_feature(input int unsigned iteration,
                                    output logic [31:0] value);
    value = stimulus(iteration);
  endtask

  task automatic timeout_feature(input int unsigned iteration);
    bit timed_out;
    timeout_count++;
    timed_out = 1'b0;
    if (iteration[0]) begin
      fork : timeout_odd
        begin @(posedge clk); end
        begin #500ps; timed_out = 1'b1; end
      join_any
      disable timeout_odd;
    end else begin
      fork : timeout_even
        begin @(posedge clk); end
        begin #3ns; timed_out = 1'b1; end
      join_any
      disable timeout_even;
    end
    if (timed_out) timeout_hits++;
    check32(timed_out, iteration[0], "timeout outcome");
  endtask

  task automatic delayed_task_value(input int unsigned iteration,
                                    input bit slow,
                                    output logic [31:0] value);
    if (slow) #3ns;
    else #500ps;
    value = stimulus(iteration);
  endtask

  task automatic task_timeout_feature(input int unsigned iteration,
                                      input logic [31:0] fallback,
                                      output logic [31:0] value);
    bit completed;
    bit timed_out;
    logic [31:0] completed_value;
    task_timeout_count++;
    completed = 1'b0;
    timed_out = 1'b0;
    completed_value = fallback;
    if (iteration[0]) begin
      fork : task_timeout_odd
        begin
          delayed_task_value(iteration, 1'b1, completed_value);
          completed = 1'b1;
        end
        begin #500ps; timed_out = 1'b1; end
      join_any
      disable task_timeout_odd;
    end else begin
      fork : task_timeout_even
        begin
          delayed_task_value(iteration, 1'b0, completed_value);
          completed = 1'b1;
        end
        begin #3ns; timed_out = 1'b1; end
      join_any
      disable task_timeout_even;
    end
    if (timed_out) task_timeout_hits++;
    check32((timed_out == iteration[0]) &&
            (completed == !iteration[0]), 1, "task timeout outcome");
    value = completed ? completed_value : fallback;
  endtask

  task automatic wait_until_feature();
    wait_until_count++;
    while (!req_ready) @(posedge clk);
    check32(req_ready, 1, "wait_until ready");
  endtask

  task automatic event_feature();
    authored_event_set = 1'b1;
    event_set_count++;
    -> authored_event;
    event_wait_count++;
    if (!authored_event_set) @authored_event;
    check32(authored_event_set, 1, "event sticky state");
    authored_event_set = 1'b0;
  endtask

  task automatic channel_feature(input logic [31:0] payload,
                                 output logic [31:0] received);
    channel_queue.push_back(payload);
    channel_send_count++;
    -> channel_available;
    channel_receive_count++;
    while (channel_queue.size() == 0) @channel_available;
    received = channel_queue.pop_front();
  endtask

  task automatic run_control();
    for (int unsigned i = 0; i < iterations; i++)
      transact(i, stimulus(i), 1'b0);
  endtask

  task automatic run_task_value();
    logic [31:0] payload;
    for (int unsigned i = 0; i < iterations; i++) begin
      task_value_feature(i, payload);
      task_value_count++;
      check32(payload, stimulus(i), "task value");
      transact(i, payload, 1'b0);
    end
  endtask

  task automatic run_clock_cycles();
    for (int unsigned i = 0; i < iterations; i++) begin
      clock_cycles_count++;
      repeat (1) @(posedge clk);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_timeout();
    for (int unsigned i = 0; i < iterations; i++) begin
      timeout_feature(i);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_task_timeout();
    logic [31:0] payload;
    for (int unsigned i = 0; i < iterations; i++) begin
      payload = stimulus(i);
      task_timeout_feature(i, payload, payload);
      transact(i, payload, 1'b0);
    end
  endtask

  task automatic run_wait_until();
    for (int unsigned i = 0; i < iterations; i++) begin
      wait_until_feature();
      transact(i, stimulus(i), 1'b1);
    end
  endtask

  task automatic run_event();
    for (int unsigned i = 0; i < iterations; i++) begin
      event_feature();
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_channel();
    logic [31:0] payload;
    for (int unsigned i = 0; i < iterations; i++) begin
      channel_feature(stimulus(i), payload);
      check32(payload, stimulus(i), "channel payload");
      transact(i, payload, 1'b0);
    end
  endtask

  task automatic run_all();
    logic [31:0] payload;
    logic [31:0] received;
    for (int unsigned i = 0; i < iterations; i++) begin
      task_value_feature(i, payload);
      task_value_count++;
      check32(payload, stimulus(i), "task value");
      clock_cycles_count++;
      repeat (1) @(posedge clk);
      timeout_feature(i);
      task_timeout_feature(i, payload, payload);
      wait_until_feature();
      event_feature();
      channel_feature(payload, received);
      check32(received, payload, "channel payload");
      payload = received;
      transact(i, payload, 1'b1);
    end
  endtask

  initial begin
    clk = 1'b0;
    rst_n = 1'b0;
    req_valid = 1'b0;
    req_data = '0;
    rsp_ready = 1'b1;
    iterations = 10000;
    kernel = "control";
    checks = 0;
    sim_cycles = 0;
    failures = 0;
    transactions = 0;
    checksum = 32'h811c_9dc5;
    task_value_count = 0;
    clock_cycles_count = 0;
    timeout_count = 0;
    timeout_hits = 0;
    task_timeout_count = 0;
    task_timeout_hits = 0;
    wait_until_count = 0;
    event_set_count = 0;
    event_wait_count = 0;
    authored_event_set = 1'b0;
    channel_send_count = 0;
    channel_receive_count = 0;
    void'($value$plusargs("AUTHORING_CORE_ITERS=%d", iterations));
    void'($value$plusargs("AUTHORING_CORE_KERNEL=%s", kernel));
    repeat (4) @(posedge clk);
    rst_n = 1'b1;

    case (kernel)
      "control": run_control();
      "task_value": run_task_value();
      "clock_cycles": run_clock_cycles();
      "timeout": run_timeout();
      "task_timeout": run_task_timeout();
      "wait_until": run_wait_until();
      "event": run_event();
      "channel": run_channel();
      "all": run_all();
      default: $fatal(1, "unknown AUTHORING_CORE_KERNEL=%s", kernel);
    endcase

    while (response_count != iterations) begin
      @(posedge clk);
      #1ps;
    end
    check32(request_count, iterations, "request count");
    check32(response_count, iterations, "response count");
    $display("AUTHORING_CORE_RESULT mode=pure_sv kernel=%s iterations=%0d transactions=%0d checks=%0d sim_cycles=%0d checksum=%0d failures=%0d task_value=%0d clock_cycles=%0d timeouts=%0d timeout_hits=%0d task_timeouts=%0d task_timeout_hits=%0d wait_until=%0d event_set=%0d event_wait=%0d channel_send=%0d channel_receive=%0d",
             kernel, iterations, transactions, checks, sim_cycles, checksum,
             failures, task_value_count, clock_cycles_count, timeout_count,
             timeout_hits, task_timeout_count, task_timeout_hits,
             wait_until_count, event_set_count, event_wait_count,
             channel_send_count, channel_receive_count);
    $finish;
  end

  authoring_core_dut i_dut (.*);
endmodule
