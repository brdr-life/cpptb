module authoring_core_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;
  import authoring_types_pkg::*;

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
  logic [63:0] wide64_i;
  logic [63:0] wide64_o;
  logic [136:0] wide137_i;
  logic [136:0] wide137_o;
  logic signed [15:0] fixed_a_i;
  logic signed [15:0] fixed_b_i;
  logic signed [15:0] fixed_y_o;
  logic [31:0] array_i [1:8];
  logic [31:0] array_o [1:8];
  bit [63:0] array_wide_i [3:0];
  bit [63:0] array_wide_o [3:0];
  bit [64:0] array_multidim_i [2:1][-1:1];
  bit [64:0] array_multidim_o [2:1][-1:1];
  bit [31:0] force_source_i;
  bit [31:0] force_fanout_o;
  packet_t packed_view_i;
  packet_t packed_view_o;
  logic [7:0] mem_addr_i;
  logic [31:0] mem_wdata_i;
  logic mem_we_i;
  logic [31:0] mem_rdata_o;
  logic apb_psel_i;
  logic apb_penable_i;
  logic apb_pwrite_i;
  logic [7:0] apb_paddr_i;
  logic [31:0] apb_pwdata_i;
  logic [3:0] apb_pstrb_i;
  logic [31:0] apb_prdata_o;
  logic apb_pready_o;
  logic apb_pslverr_o;

  int unsigned iterations;
  string kernel;
  longint unsigned checks;
  longint unsigned sim_cycles;
  longint unsigned spawned_processes;
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
  longint unsigned queue_send_count;
  longint unsigned queue_receive_count;
  longint unsigned queue_put_count;
  longint unsigned queue_get_count;
  longint unsigned lock_acquire_count;
  longint unsigned semaphore_acquire_count;
  longint unsigned wide64_count;
  longint unsigned wide_echo_137_count;
  longint unsigned wide_slice_count;
  longint unsigned fixed_mac_count;
  longint unsigned array_index_count;
  longint unsigned array_wide_count;
  longint unsigned array_multidim_count;
  longint unsigned mem_rw_count;
  longint unsigned hier_probe_reads;
  longint unsigned hier_probe_deposits;
  longint unsigned mem_backdoor_reads;
  longint unsigned mem_backdoor_deposits;
  longint unsigned probe_diag_reads;
  longint unsigned probe_diag_deposits;
  longint unsigned signal_edges;
  longint unsigned force_release_count;
  longint unsigned packed_view_count;
  longint unsigned hier_data_reads;
  longint unsigned hier_data_deposits;
  longint unsigned timing_phases_count;
  longint unsigned test_lifecycle_count;
  longint unsigned dynamic_spawn_count;
  longint unsigned analysis_write_count;
  longint unsigned analysis_delivery_count;
  longint unsigned random_stimulus_count;
  longint unsigned constrained_packet_count;
  longint unsigned constraint_extensions_count;
  longint unsigned coverage_sampling_count;
  longint unsigned apb_component_count;
  longint unsigned memory_model_count;
  longint unsigned memory_model_direct_count;
  longint unsigned register_prediction_validity_count;
  longint unsigned register_backdoor_count;
  longint unsigned register_hierarchy_count;
  longint unsigned register_split_count;
  longint unsigned register_wide_count;
  longint unsigned register_enum_count;
  longint unsigned register_predictor_reads;
  longint unsigned register_predictor_writes;
  longint unsigned coverage_opcode_hits [0:3];
  longint unsigned coverage_length_hits [0:4];
  longint unsigned coverage_cross_hits [0:2][0:2];
  longint unsigned coverage_transition_hits;
  bit coverage_have_previous_opcode;
  logic [1:0] coverage_previous_opcode;
  longint unsigned dynamic_monitor_edges;
  bit dynamic_process_ready;
  bit dynamic_process_release;
  event authored_event;
  bit authored_event_set;
  event queue_available;
  logic [31:0] event_token;
  logic [31:0] queue_items[$];
  mailbox #(logic [31:0]) bounded_queue;
  mailbox #(logic [31:0]) dynamic_monitor_queue;
  mailbox #(logic [31:0]) process_expected_queue;
  mailbox #(logic [31:0]) process_observed_queue;
  mailbox #(logic [31:0]) analysis_buffer;
  logic [31:0] analysis_expected[$];
  logic [31:0] analysis_actual[$];
  typedef struct packed {
    bit write;
    logic [7:0] address;
    logic [31:0] data;
    logic [3:0] strobe;
    bit error;
    int unsigned wait_cycles;
  } apb_transaction_t;
  apb_transaction_t apb_expected[$];
  apb_transaction_t apb_actual[$];
  string transaction_trace_stream[$];
  string transaction_trace_type[$];
  longint unsigned transaction_trace_sequence[$];
  time transaction_trace_begin[$];
  time transaction_trace_end[$];
  int unsigned transaction_trace_disposition[$];
  string transaction_trace_json[$];
  longint unsigned apb_compared;
  longint unsigned apb_checker_violations;
  bit apb_done;
  bit memory_model_mode;
  bit transaction_recording_mode;
  byte unsigned memory_model_bytes [longint unsigned];
  longint unsigned memory_model_reads;
  longint unsigned memory_model_writes;
  longint unsigned memory_model_mismatches;
  int unsigned analysis_buffer_drops;
  bit analysis_monitor_complete;
  semaphore queue_credits;
  semaphore authored_lock;
  logic [63:0] random_seed_state;
  logic [63:0] random_s0;
  logic [63:0] random_s1;
  logic [63:0] random_s2;
  logic [63:0] random_s3;

  always begin
    #1ns clk = ~clk;
    if (clk) sim_cycles++;
  end

  function automatic logic [31:0] stimulus(input int unsigned iteration);
    return ((iteration + 1) * 32'h1f12_3bb5) ^ 32'hc001_d00d;
  endfunction

  function automatic logic [31:0] probe_memory_seed(
      input int unsigned address);
    return 32'h1357_9bdf ^ (address * 32'h9e37_79b9);
  endfunction

  function automatic logic [31:0] probe_memory_value(
      input int unsigned iteration);
    return stimulus(iteration) ^ 32'h6a09_e667;
  endfunction

  function automatic logic [31:0] expected_response(input int unsigned iteration);
    return (stimulus(iteration) ^ 32'ha5a5_5a5a) + iteration;
  endfunction

  function automatic logic [31:0] expected_payload_response(
      input int unsigned iteration,
      input logic [31:0] payload);
    return (payload ^ 32'ha5a5_5a5a) + iteration;
  endfunction

  function automatic logic [63:0] rotate_left64(
      input logic [63:0] value,
      input int unsigned shift);
    return (value << shift) | (value >> (64 - shift));
  endfunction

  function automatic logic [63:0] splitmix_next();
    logic [63:0] value;
    random_seed_state = random_seed_state + 64'h9e37_79b9_7f4a_7c15;
    value = random_seed_state;
    value = (value ^ (value >> 30)) * 64'hbf58_476d_1ce4_e5b9;
    value = (value ^ (value >> 27)) * 64'h94d0_49bb_1331_11eb;
    return value ^ (value >> 31);
  endfunction

  task automatic initialize_random();
    random_seed_state = 64'd1;
    random_s0 = splitmix_next();
    random_s1 = splitmix_next();
    random_s2 = splitmix_next();
    random_s3 = splitmix_next();
  endtask

  function automatic logic [63:0] random_next_u64();
    logic [63:0] result;
    logic [63:0] shifted;
    result = rotate_left64(random_s1 * 64'd5, 7) * 64'd9;
    shifted = random_s1 << 17;
    random_s2 = random_s2 ^ random_s0;
    random_s3 = random_s3 ^ random_s1;
    random_s1 = random_s1 ^ random_s2;
    random_s0 = random_s0 ^ random_s3;
    random_s2 = random_s2 ^ shifted;
    random_s3 = rotate_left64(random_s3, 45);
    return result;
  endfunction

  function automatic longint unsigned random_below(
      input longint unsigned bound);
    longint unsigned threshold;
    longint unsigned value;
    threshold = (-bound) % bound;
    forever begin
      value = random_next_u64();
      if (value >= threshold) return value % bound;
    end
  endfunction

  function automatic logic [31:0] random_payload();
    logic [31:0] payload;
    logic [63:0] random_wide;
    logic [63:0] random_top;
    int unsigned order [0:3];
    int unsigned remaining;
    int unsigned selected;
    int unsigned temporary;

    payload = random_next_u64()[31:0];
    case (random_below(10))
      0: payload = payload ^ 32'h0000_0000;
      1, 2: payload = payload ^ 32'h0101_0101;
      3, 4, 5: payload = payload ^ 32'h1357_9bdf;
      default: payload = payload ^ 32'ha5a5_5a5a;
    endcase
    random_wide = random_next_u64();
    random_top = random_next_u64();
    payload = payload ^ random_wide[31:0] ^ random_wide[63:32];
    if (random_top[0]) payload = payload ^ 32'h8000_0000;

    order = '{0, 1, 2, 3};
    for (remaining = 4; remaining > 1; remaining--) begin
      selected = random_below(remaining);
      temporary = order[remaining - 1];
      order[remaining - 1] = order[selected];
      order[selected] = temporary;
    end
    payload = payload ^ order[0] ^ (order[1] << 4) ^
              (order[2] << 8) ^ (order[3] << 12);
    return payload;
  endfunction

  function automatic logic [31:0] constrained_packet_payload();
    logic [7:0] opcode;
    logic [15:0] length;
    logic [15:0] address;
    logic [7:0] tag;
    forever begin
      opcode = random_below(7);
      length = 16'd64 + random_below(1437);
      address = 16'h1000 + random_below(4096);
      tag = random_below(256);
      if ((length % 4) != 0) continue;
      if ((address % 4) != 0) continue;
      if ((opcode == 6) && (length > 256)) continue;
      return ({24'b0, opcode} << 29) ^ ({16'b0, length} << 16) ^
             ({16'b0, address} << 1) ^ {24'b0, tag};
    end
  endfunction

  function automatic logic [31:0] constraint_extensions_payload();
    logic [7:0] opcode;
    logic [15:0] length;
    logic [7:0] route;
    logic [7:0] byte0;
    logic [7:0] byte1;
    logic [31:0] token0;
    logic [31:0] token1;
    logic token2;
    longint unsigned selected;
    forever begin
      selected = random_below(3);
      case (selected)
        0: opcode = 1;
        1: opcode = 3;
        default: opcode = 5;
      endcase

      selected = random_below(4);
      if (selected == 0) begin
        length = 64 + random_below(1);
      end else begin
        length = 128 + random_below(4);
      end
      route = 2 + random_below(1);
      byte0 = random_below(256);
      byte1 = random_below(256);
      token0 = random_below(64'h0000_0001_0000_0000);
      token1 = random_below(64'h0000_0001_0000_0000);
      token2 = 1 + random_below(1);
      if (byte0 == byte1) continue;

      return ({24'b0, opcode} << 29) ^ ({16'b0, length} << 16) ^
             ({24'b0, route} << 24) ^ ({24'b0, byte0} << 8) ^
             {24'b0, byte1} ^ token0 ^ token1 ^
             ({31'b0, token2} << 31);
    end
  endfunction

  function automatic logic [63:0] wide64_stimulus(input int unsigned iteration);
    return {stimulus(iteration * 2 + 1), stimulus(iteration * 2)};
  endfunction

  function automatic logic [136:0] wide137_stimulus(input int unsigned iteration);
    logic [136:0] value;
    value[31:0] = stimulus(iteration * 5);
    value[63:32] = stimulus(iteration * 5 + 1);
    value[95:64] = stimulus(iteration * 5 + 2);
    value[127:96] = stimulus(iteration * 5 + 3);
    value[136:128] = stimulus(iteration * 5 + 4)[8:0];
    return value;
  endfunction

  function automatic logic [64:0] array_multidim_stimulus(
      input int unsigned iteration,
      input int row,
      input int column);
    int unsigned ordinal;
    int unsigned element;
    logic [64:0] value;
    ordinal = (2 - row) * 3 + column + 1;
    element = iteration * 6 + ordinal;
    value[31:0] = stimulus(element * 3);
    value[63:32] = stimulus(element * 3 + 1);
    value[64] = stimulus(element * 3 + 2)[0];
    return value;
  endfunction

  function automatic logic signed [15:0] fixed_product_expected(
      input logic signed [15:0] a,
      input logic signed [15:0] b);
    logic signed [31:0] product;
    logic [31:0] magnitude;
    logic [17:0] quotient;
    logic [13:0] remainder;
    logic signed [32:0] rounded;
    product = a * b;
    magnitude = product < 0 ? -product : product;
    quotient = magnitude[31:14];
    remainder = magnitude[13:0];
    if ((remainder > 14'h2000) ||
        ((remainder == 14'h2000) && quotient[0])) begin
      quotient = quotient + 1'b1;
    end
    rounded = product < 0 ? -$signed({15'b0, quotient}) :
                            $signed({15'b0, quotient});
    if (rounded > 33'sd32767) return 16'sh7fff;
    if (rounded < -33'sd32768) return -16'sd32768;
    return rounded[15:0];
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

  task automatic check64(input logic [63:0] actual,
                         input logic [63:0] expected,
                         input string label);
    checks++;
    if (actual == expected) return;
    failures++;
    if (failures <= 8) begin
      $display("AUTHORING_CORE_MISMATCH mode=pure_sv kernel=%s label=%s actual=0x%016x expected=0x%016x",
               kernel, label, actual, expected);
    end
  endtask

  task automatic check128(input logic [127:0] actual,
                          input logic [127:0] expected,
                          input string label);
    checks++;
    if (actual == expected) return;
    failures++;
    if (failures <= 8) begin
      $display("AUTHORING_CORE_MISMATCH mode=pure_sv kernel=%s label=%s actual_word0=0x%08x expected_word0=0x%08x",
               kernel, label, actual[31:0], expected[31:0]);
    end
  endtask

  task automatic check65(input logic [64:0] actual,
                         input logic [64:0] expected,
                         input string label);
    checks++;
    if (actual == expected) return;
    failures++;
    if (failures <= 8) begin
      $display("AUTHORING_CORE_MISMATCH mode=pure_sv kernel=%s label=%s actual_word0=0x%08x expected_word0=0x%08x",
               kernel, label, actual[31:0], expected[31:0]);
    end
  endtask

  task automatic check137(input logic [136:0] actual,
                          input logic [136:0] expected,
                          input string label);
    checks++;
    if (actual == expected) return;
    failures++;
    if (failures <= 8) begin
      $display("AUTHORING_CORE_MISMATCH mode=pure_sv kernel=%s label=%s actual_word0=0x%08x expected_word0=0x%08x",
               kernel, label, actual[31:0], expected[31:0]);
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
    check32(rsp_data, expected_payload_response(iteration, payload), "response");
    checksum = (checksum ^ rsp_data) * 32'h0100_0193;
    transactions++;
  endtask

  task automatic drive_request(input logic [31:0] payload);
    wait_ready_raw();
    @(negedge clk);
    req_data = payload;
    req_valid = 1'b1;
    @(posedge clk);
    @(negedge clk);
    req_valid = 1'b0;
  endtask

  task automatic transact_signal_edge(input int unsigned iteration,
                                      input logic [31:0] payload);
    wait_ready_raw();
    @(negedge clk);
    req_data = payload;
    req_valid = 1'b1;
    @(posedge clk);
    @(negedge clk);
    req_valid = 1'b0;
    @(posedge rsp_valid);
    signal_edges++;
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

  task automatic queue_feature(input logic [31:0] payload,
                                 output logic [31:0] received);
    queue_items.push_back(payload);
    queue_send_count++;
    -> queue_available;
    queue_receive_count++;
    while (queue_items.size() == 0) @queue_available;
    received = queue_items.pop_front();
  endtask

  task automatic queue_sync_producer();
    for (int unsigned i = 0; i < iterations; i++) begin
      queue_credits.get(1);
      semaphore_acquire_count++;
      bounded_queue.put(stimulus(i));
      queue_put_count++;
    end
  endtask

  task automatic queue_sync_consumer();
    logic [31:0] payload;
    for (int unsigned i = 0; i < iterations; i++) begin
      bounded_queue.get(payload);
      queue_get_count++;
      authored_lock.get(1);
      lock_acquire_count++;
      check32(payload, stimulus(i), "bounded queue payload");
      authored_lock.put(1);
      queue_credits.put(1);
      transact(i, payload, 1'b0);
    end
  endtask

  task automatic wide64_feature(input int unsigned iteration);
    logic [63:0] value;
    wide64_count++;
    value = wide64_stimulus(iteration);
    @(negedge clk);
    wide64_i = value;
    @(posedge clk);
    #1ps;
    check64(wide64_o, value ^ 64'hd1b5_4a32_d192_ed03, "wide64");
  endtask

  task automatic wide137_feature(input int unsigned iteration);
    logic [136:0] value;
    wide_echo_137_count++;
    value = wide137_stimulus(iteration);
    @(negedge clk);
    wide137_i = value;
    @(posedge clk);
    #1ps;
    check137(wide137_o,
             value ^ 137'h1a5_5aa55aa5_01234567_89abcdef_deadbeef,
             "wide137");
  endtask

  task automatic wide_slice_feature(input int unsigned iteration);
    logic [136:0] value;
    logic [136:0] expected;
    logic [63:0] replacement;
    wide_slice_count++;
    value = wide137_stimulus(iteration);
    replacement = wide64_stimulus(iteration) ^ 64'h4f1b_bcdd_812f_6a31;
    value[37 +: 64] = replacement;
    @(negedge clk);
    wide137_i = value;
    @(posedge clk);
    #1ps;
    expected = value ^ 137'h1a5_5aa55aa5_01234567_89abcdef_deadbeef;
    check64(wide137_o[37 +: 64], expected[37 +: 64], "wide slice");
  endtask

  task automatic fixed_mac_feature(input int unsigned iteration);
    logic signed [15:0] a;
    logic signed [15:0] b;
    fixed_mac_count++;
    a = stimulus(iteration * 2);
    b = stimulus(iteration * 2 + 1);
    @(negedge clk);
    fixed_a_i = a;
    fixed_b_i = b;
    @(posedge clk);
    #1ps;
    check32({16'b0, fixed_y_o}, {16'b0, fixed_product_expected(a, b)},
            "fixed mac");
  endtask

  task automatic array_index_feature(input int unsigned iteration);
    logic [31:0] value;
    array_index_count++;
    @(negedge clk);
    for (int index = 1; index <= 8; index++) begin
      array_i[index] = stimulus(iteration * 8 + index - 1);
    end
    @(posedge clk);
    #1ps;
    for (int index = 1; index <= 8; index++) begin
      value = stimulus(iteration * 8 + index - 1);
      check32(array_o[index],
              value ^ (32'h6d2b_79f5 + index), "array index");
    end
  endtask

  task automatic array_wide_feature(input int unsigned iteration);
    logic [63:0] value;
    array_wide_count++;
    @(negedge clk);
    for (int index = 0; index <= 3; index++) begin
      array_wide_i[index] = wide64_stimulus(iteration * 4 + index);
    end
    @(posedge clk);
    #1ps;
    for (int index = 0; index <= 3; index++) begin
      value = wide64_stimulus(iteration * 4 + index);
      check64(array_wide_o[index],
              value ^ (64'h9e37_79b9_7f4a_7c15 + 64'(index)),
              "wide array");
    end
  endtask

  task automatic array_multidim_feature(input int unsigned iteration);
    logic [64:0] value;
    array_multidim_count++;
    @(negedge clk);
    for (int row = 2; row >= 1; row--) begin
      for (int column = -1; column <= 1; column++) begin
        array_multidim_i[row][column] =
            array_multidim_stimulus(iteration, row, column);
      end
    end
    @(posedge clk);
    #1ps;
    for (int row = 2; row >= 1; row--) begin
      for (int column = -1; column <= 1; column++) begin
        value = array_multidim_stimulus(iteration, row, column);
        check65(array_multidim_o[row][column],
                value ^ 65'h1_01234567_89abcdef,
                "multidimensional array");
      end
    end
  endtask

  task automatic mem_rw_feature(input int unsigned iteration);
    logic [7:0] address;
    logic [31:0] value;
    mem_rw_count++;
    address = iteration[7:0];
    value = stimulus(iteration) ^ 32'h51a7_c3e9;
    @(negedge clk);
    mem_addr_i = address;
    mem_wdata_i = value;
    mem_we_i = 1'b1;
    @(posedge clk);
    @(negedge clk);
    mem_we_i = 1'b0;
    @(posedge clk);
    #1ps;
    check32(mem_rdata_o, value, "memory read");
  endtask

  task automatic hier_probe_feature(input int unsigned iteration,
                                    inout logic [31:0] previous_cycle_count);
    logic [31:0] cycle_count;
    logic [31:0] value;
    value = stimulus(iteration) ^ 32'h3c6e_f372;

    @(negedge clk);
    cycle_count = i_dut.cycle_count;
    hier_probe_reads++;
    check32(iteration == 0 || cycle_count > previous_cycle_count, 1,
            "hierarchical cycle count");
    previous_cycle_count = cycle_count;

    i_dut.pending_data = value;
    hier_probe_deposits++;
    #1ps;
    hier_probe_reads++;
    check32(i_dut.pending_data, value, "hierarchical deposit");
  endtask

  task automatic hier_data_feature(input int unsigned iteration);
    bit [136:0] wide;
    logic [3:0] logic_value;
    wide = wide137_stimulus(iteration);
    logic_value = stimulus(iteration)[3:0];

    @(negedge clk);
    hier_data_deposits++;
    i_dut.hierarchy_wide = wide;
    hier_data_reads++;
    check137(i_dut.hierarchy_wide, wide, "hierarchy wide data");

    hier_data_deposits++;
    i_dut.hierarchy_logic = logic_value;
    hier_data_reads++;
    check32(i_dut.hierarchy_logic, logic_value, "hierarchy four-state data");
  endtask

  task automatic mem_backdoor_feature(input int unsigned iteration);
    logic [7:0] address;
    logic [31:0] value;
    address = iteration[7:0];
    value = stimulus(iteration) ^ 32'h8a5c_19d7;

    @(negedge clk);
    i_dut.memory[address] = value;
    mem_backdoor_deposits++;
    #1ps;
    mem_backdoor_reads++;
    check32(i_dut.memory[address], value, "memory backdoor read");

    mem_addr_i = address;
    mem_we_i = 1'b0;
    @(posedge clk);
    #1ps;
    check32(mem_rdata_o, value, "memory backdoor frontdoor visibility");
  endtask

  task automatic seed_probe_memory();
    for (int unsigned address = 0; address < 256; address++) begin
      @(negedge clk);
      mem_addr_i = address[7:0];
      mem_wdata_i = probe_memory_seed(address);
      mem_we_i = 1'b1;
      @(posedge clk);
    end
    @(negedge clk);
    mem_we_i = 1'b0;
  endtask

  task automatic mem_probe_read_feature(input int unsigned iteration);
    logic [7:0] address;
    logic [31:0] expected;
    logic [31:0] internal;
    address = iteration[7:0];
    expected = probe_memory_seed(address);

    @(negedge clk);
    mem_addr_i = address;
    internal = i_dut.memory[address];
    probe_diag_reads++;

    #1ps;
    check32(internal, expected, "memory probe read");
    @(posedge clk);
    #1ps;
    check32(mem_rdata_o, expected, "memory probe read frontdoor");
  endtask

  task automatic mem_probe_deposit_feature(input int unsigned iteration);
    logic [7:0] address;
    logic [31:0] value;
    address = iteration[7:0];
    value = probe_memory_value(iteration);

    @(negedge clk);
    mem_addr_i = address;
    i_dut.memory[address] = value;
    probe_diag_deposits++;

    #1ps;
    @(posedge clk);
    #1ps;
    check32(mem_rdata_o, value, "memory probe deposit frontdoor");
  endtask

  task automatic mem_probe_read_deposit_feature(
      input int unsigned iteration);
    logic [7:0] address;
    logic [31:0] expected_before;
    logic [31:0] internal;
    logic [31:0] value;
    address = iteration[7:0];
    expected_before = iteration < 256
        ? probe_memory_seed(address)
        : probe_memory_value(iteration - 256);
    value = probe_memory_value(iteration);

    @(negedge clk);
    mem_addr_i = address;
    internal = i_dut.memory[address];
    probe_diag_reads++;
    i_dut.memory[address] = value;
    probe_diag_deposits++;

    #1ps;
    check32(internal, expected_before, "memory probe read before deposit");
    @(posedge clk);
    #1ps;
    check32(mem_rdata_o, value, "memory probe read/deposit frontdoor");
  endtask

  task automatic force_release_feature(input int unsigned iteration);
    logic [31:0] source;
    logic [31:0] value;
    source = stimulus(iteration);
    value = source ^ 32'ha5a5_5a5a;

    force_release_count++;
    force_source_i = source;
    force i_dut.force_target = value;
    #1ps;
    check32(force_fanout_o, value, "forced internal net fanout");

    release i_dut.force_target;
    #1ps;
    check32(force_fanout_o, source ^ 32'h5a5a_a5a5,
            "released internal net driver");
  endtask

  task automatic packed_view_feature(input int unsigned iteration);
    packet_t value;
    logic [2:0] opcode;
    logic [1:0] tag;
    logic [2:0] payload;
    opcode = iteration[2:0];
    tag = iteration[1:0];
    payload = stimulus(iteration)[2:0];
    value = '0;
    value.opcode = opcode;
    value.state = STATE_RUN;
    value.inner.tag = tag;
    value.inner.payload = payload;

    packed_view_count++;
    packed_view_i = value;
    #1ps;
    check32({29'b0, packed_view_o.opcode}, {29'b0, opcode ^ 3'b011},
            "packed view opcode");
    check32(packed_view_o.state == STATE_RUN, 1, "packed view signed enum");
    check32({30'b0, packed_view_o.inner.tag},
            {30'b0, ((tag + 1'b1) & 2'b11)}, "packed view nested tag");
    check32({29'b0, packed_view_o.inner.payload},
            {29'b0, payload ^ 3'b101}, "packed view nested payload");
  endtask

  task automatic run_control();
    for (int unsigned i = 0; i < iterations; i++)
      transact(i, stimulus(i), 1'b0);
  endtask

  task automatic run_random_stimulus();
    logic [31:0] payload;
    initialize_random();
    for (int unsigned i = 0; i < iterations; i++) begin
      payload = random_payload();
      random_stimulus_count++;
      transact(i, payload, 1'b0);
    end
  endtask

  task automatic run_constrained_packet();
    logic [31:0] payload;
    initialize_random();
    for (int unsigned i = 0; i < iterations; i++) begin
      payload = constrained_packet_payload();
      constrained_packet_count++;
      transact(i, payload, 1'b0);
    end
  endtask

  task automatic run_constraint_extensions();
    logic [31:0] payload;
    initialize_random();
    for (int unsigned i = 0; i < iterations; i++) begin
      payload = constraint_extensions_payload();
      constraint_extensions_count++;
      transact(i, payload, 1'b0);
    end
  endtask

  task automatic coverage_sample(input int unsigned iteration);
    logic [1:0] opcode;
    int unsigned length;
    int unsigned length_bin;
    opcode = iteration[1:0];
    length = (iteration * 37) % 1600;

    coverage_opcode_hits[opcode]++;
    if (coverage_have_previous_opcode &&
        coverage_previous_opcode == 0 && opcode == 1)
      coverage_transition_hits++;
    coverage_previous_opcode = opcode;
    coverage_have_previous_opcode = 1'b1;

    if (length == 0) begin
      coverage_length_hits[0]++;
      length_bin = 3;
    end else if (length <= 63) begin
      coverage_length_hits[1]++;
      length_bin = 0;
    end else if (length <= 511) begin
      coverage_length_hits[2]++;
      length_bin = 1;
    end else if (length <= 1500) begin
      coverage_length_hits[3]++;
      length_bin = 2;
    end else begin
      coverage_length_hits[4]++;
      length_bin = 3;
    end
    if (opcode != 3 && length_bin < 3)
      coverage_cross_hits[opcode][length_bin]++;
    coverage_sampling_count++;
  endtask

  task automatic run_coverage_sampling();
    longint unsigned opcode_accounted;
    longint unsigned length_accounted;
    longint unsigned cross_accounted;
    longint unsigned expected_cross;
    int unsigned length;
    for (int unsigned i = 0; i < iterations; i++) begin
      coverage_sample(i);
      transact(i, stimulus(i), 1'b0);
    end

    opcode_accounted = 0;
    for (int unsigned i = 0; i < 4; i++)
      opcode_accounted += coverage_opcode_hits[i];
    length_accounted = 0;
    for (int unsigned i = 0; i < 5; i++)
      length_accounted += coverage_length_hits[i];
    cross_accounted = 0;
    for (int unsigned opcode = 0; opcode < 3; opcode++)
      for (int unsigned length_bin = 0; length_bin < 3; length_bin++)
        cross_accounted += coverage_cross_hits[opcode][length_bin];
    expected_cross = 0;
    for (int unsigned i = 0; i < iterations; i++) begin
      length = (i * 37) % 1600;
      if (i[1:0] != 3 && length != 0 && length <= 1500)
        expected_cross++;
    end
    check32(coverage_sampling_count, iterations, "coverage samples");
    check32(opcode_accounted, iterations, "coverage opcode accounting");
    check32(length_accounted, iterations, "coverage length accounting");
    check32(coverage_transition_hits, (iterations + 2) / 4,
            "coverage transition hits");
    check32(cross_accounted, expected_cross, "coverage cross hits");
  endtask

  task automatic apb_compare_available();
    apb_transaction_t expected;
    apb_transaction_t actual;
    while (apb_expected.size() != 0 && apb_actual.size() != 0) begin
      expected = apb_expected.pop_front();
      actual = apb_actual.pop_front();
      checks++;
      if (actual !== expected) failures++;
      apb_compared++;
    end
  endtask

  task automatic apb_publish_expected(input apb_transaction_t transaction);
    apb_expected.push_back(transaction);
    apb_compare_available();
  endtask

  task automatic apb_publish_actual(input apb_transaction_t transaction);
    if (memory_model_mode) begin
      apb_transaction_t expected;
      expected = transaction;
      expected.error = 1'b0;
      if (transaction.write) begin
        for (int unsigned byte_index = 0; byte_index < 4; byte_index++) begin
          if (transaction.strobe[byte_index])
            memory_model_bytes[transaction.address + byte_index] =
                transaction.data[byte_index * 8 +: 8];
        end
        memory_model_writes++;
      end else begin
        expected.data = '0;
        for (int unsigned byte_index = 0; byte_index < 4; byte_index++) begin
          if (memory_model_bytes.exists(transaction.address + byte_index))
            expected.data[byte_index * 8 +: 8] =
                memory_model_bytes[transaction.address + byte_index];
        end
        memory_model_reads++;
      end
      checks++;
      if (transaction !== expected) begin
        failures++;
        memory_model_mismatches++;
      end
    end else begin
      apb_actual.push_back(transaction);
      apb_compare_available();
    end
  endtask

  task automatic record_apb_transaction(
      input apb_transaction_t transaction,
      input time begin_time,
      input time end_time
  );
    string operation;
    string status;
    operation = transaction.write ? "write" : "read";
    status = transaction.error ? "slave_error" : "okay";
    analysis_write_count++;
    analysis_delivery_count++;
    transaction_trace_stream.push_back("apb0.observed");
    transaction_trace_type.push_back("memory_transaction");
    transaction_trace_sequence.push_back(transaction_trace_sequence.size());
    transaction_trace_begin.push_back(begin_time);
    transaction_trace_end.push_back(end_time);
    transaction_trace_disposition.push_back(0);
    transaction_trace_json.push_back($sformatf(
        "{\"operation\":\"%s\",\"address\":%0d,\"data\":%0d,\"byte_enable\":%0d,\"status\":\"%s\",\"wait_cycles\":%0d}",
        operation, transaction.address, transaction.data,
        transaction.strobe, status, transaction.wait_cycles));
  endtask

  task automatic apb_write(input logic [7:0] address,
                           input logic [31:0] data,
                           input logic [3:0] strobe);
    int unsigned wait_cycles;
    @(negedge clk);
    apb_paddr_i = address;
    apb_pwdata_i = data;
    apb_pstrb_i = strobe;
    apb_pwrite_i = 1'b1;
    apb_psel_i = 1'b1;
    apb_penable_i = 1'b0;
    @(posedge clk);
    @(negedge clk);
    apb_penable_i = 1'b1;
    wait_cycles = 0;
    do begin
      @(posedge clk);
      if (!apb_pready_o) wait_cycles++;
    end while (!apb_pready_o);
    check32(apb_pslverr_o, 0, "APB write status");
    if (!memory_model_mode)
      apb_publish_expected('{1'b1, address, data, strobe, 1'b0,
                             wait_cycles});
    transactions++;
    if (memory_model_mode) memory_model_count++;
    else if (!transaction_recording_mode) apb_component_count++;
    @(negedge clk);
    apb_psel_i = 1'b0;
    apb_penable_i = 1'b0;
    apb_pwrite_i = 1'b0;
  endtask

  task automatic apb_read(input logic [7:0] address,
                          output logic [31:0] data);
    int unsigned wait_cycles;
    @(negedge clk);
    apb_paddr_i = address;
    apb_pstrb_i = '0;
    apb_pwrite_i = 1'b0;
    apb_psel_i = 1'b1;
    apb_penable_i = 1'b0;
    @(posedge clk);
    @(negedge clk);
    apb_penable_i = 1'b1;
    wait_cycles = 0;
    do begin
      @(posedge clk);
      if (!apb_pready_o) wait_cycles++;
    end while (!apb_pready_o);
    data = apb_prdata_o;
    check32(apb_pslverr_o, 0, "APB read status");
    if (!memory_model_mode)
      apb_publish_expected('{1'b0, address, data, 4'hf, 1'b0,
                             wait_cycles});
    transactions++;
    if (memory_model_mode) memory_model_count++;
    else if (!transaction_recording_mode) apb_component_count++;
    @(negedge clk);
    apb_psel_i = 1'b0;
    apb_penable_i = 1'b0;
    apb_pwrite_i = 1'b0;
  endtask

  task automatic run_apb_monitor();
    int unsigned completed;
    int unsigned wait_cycles;
    bit active;
    time started;
    apb_transaction_t transaction;
    completed = 0;
    wait_cycles = 0;
    active = 1'b0;
    while (completed < iterations * 2) begin
      @(posedge clk);
      if (!apb_psel_i) begin
        active = 1'b0;
        wait_cycles = 0;
      end else if (!apb_penable_i) begin
        active = 1'b1;
        started = $time;
        wait_cycles = 0;
      end else if (!apb_pready_o) begin
        if (active) wait_cycles++;
      end else begin
        transaction = '{apb_pwrite_i, apb_paddr_i,
                        apb_pwrite_i ? apb_pwdata_i : apb_prdata_o,
                        apb_pwrite_i ? apb_pstrb_i : 4'hf,
                        apb_pslverr_o, wait_cycles};
        if (transaction_recording_mode) begin
          analysis_write_count++;
          analysis_delivery_count += 2;
        end
        apb_publish_actual(transaction);
        if (transaction_recording_mode)
          record_apb_transaction(transaction, started, $time);
        active = 1'b0;
        wait_cycles = 0;
        completed++;
      end
    end
  endtask

  task automatic apb_checker_report(input bit condition);
    if (!condition) begin
      apb_checker_violations++;
      checks++;
      failures++;
    end
  endtask

  task automatic run_apb_checker();
    bit setup_seen;
    bit waiting;
    logic [7:0] saved_address;
    logic saved_write;
    logic [31:0] saved_write_data;
    logic [3:0] saved_strobe;
    setup_seen = 1'b0;
    waiting = 1'b0;
    while (!apb_done) begin
      @(posedge clk);
      apb_checker_report(!apb_penable_i || apb_psel_i);
      apb_checker_report(!(apb_psel_i && apb_penable_i) ||
                         setup_seen || waiting);
      if (waiting) begin
        apb_checker_report(apb_psel_i);
        apb_checker_report(apb_penable_i);
        apb_checker_report(apb_paddr_i == saved_address);
        apb_checker_report(apb_pwrite_i == saved_write);
        apb_checker_report(!saved_write ||
                           apb_pwdata_i == saved_write_data);
        apb_checker_report(apb_pstrb_i == saved_strobe);
      end
      if (apb_psel_i && !apb_penable_i) begin
        apb_checker_report(apb_pwrite_i || apb_pstrb_i == '0);
        saved_address = apb_paddr_i;
        saved_write = apb_pwrite_i;
        saved_write_data = apb_pwdata_i;
        saved_strobe = apb_pstrb_i;
        setup_seen = 1'b1;
        waiting = 1'b0;
      end else if (apb_psel_i && apb_penable_i) begin
        if (setup_seen) begin
          apb_checker_report(apb_paddr_i == saved_address);
          apb_checker_report(apb_pwrite_i == saved_write);
          apb_checker_report(!saved_write ||
                             apb_pwdata_i == saved_write_data);
          apb_checker_report(apb_pstrb_i == saved_strobe);
        end
        waiting = !apb_pready_o;
        setup_seen = 1'b0;
      end else begin
        waiting = 1'b0;
        setup_seen = 1'b0;
      end
    end
  endtask

  task automatic run_apb_sequence();
    logic [31:0] value;
    logic [31:0] read_data;
    for (int unsigned i = 0; i < iterations; i++) begin
      value = stimulus(i);
      apb_write((i & 15) * 4, value, 4'hf);
      apb_read((i & 15) * 4, read_data);
      check32(read_data, value, "APB read data");
      checksum = (checksum ^ read_data) * 32'h0100_0193;
    end
    apb_done = 1'b1;
  endtask

  task automatic run_apb_component();
    fork
      run_apb_sequence();
      run_apb_monitor();
      run_apb_checker();
    join
    check32(apb_expected.size(), 0, "APB expected pending");
    check32(apb_actual.size(), 0, "APB actual pending");
    check32(apb_compared, iterations * 2, "APB scoreboard comparisons");
    check32(apb_checker_violations, 0, "APB protocol violations");
  endtask

  task automatic run_transaction_recording();
    transaction_recording_mode = 1'b1;
    fork
      run_apb_sequence();
      run_apb_monitor();
      run_apb_checker();
    join
    check32(apb_expected.size(), 0, "recorded APB expected pending");
    check32(apb_actual.size(), 0, "recorded APB actual pending");
    check32(apb_compared, iterations * 2,
            "recorded APB scoreboard comparisons");
    check32(apb_checker_violations, 0,
            "recorded APB protocol violations");
    check32(transaction_trace_sequence.size(), iterations * 2,
            "recorded APB transaction count");
    check32(transaction_trace_sequence[0], 0,
            "recorded APB first sequence");
    check32(transaction_trace_sequence[iterations * 2 - 1],
            iterations * 2 - 1, "recorded APB last sequence");
    check32(transaction_trace_end[0] > transaction_trace_begin[0], 1,
            "recorded APB interval");
    check32(transaction_trace_json[0].substr(0, 19) ==
                "{\"operation\":\"write\"",
            1,
            "recorded APB typed payload");
  endtask

  task automatic run_memory_model();
    memory_model_mode = 1'b1;
    fork
      run_apb_sequence();
      run_apb_monitor();
      run_apb_checker();
    join
    check32(memory_model_reads, iterations, "memory-model reads");
    check32(memory_model_writes, iterations, "memory-model writes");
    check32(memory_model_mismatches, 0, "memory-model mismatches");
    check32(apb_checker_violations, 0,
            "memory-model APB protocol violations");
  endtask

  task automatic run_memory_model_direct();
    logic [31:0] value;
    logic [31:0] read_data;
    longint unsigned address;
    memory_model_bytes.delete();
    for (int unsigned i = 0; i < iterations; i++) begin
      address = (i & 15) * 4;
      value = stimulus(i);
      for (int unsigned byte_index = 0; byte_index < 4; byte_index++)
        memory_model_bytes[address + byte_index] =
            value[byte_index * 8 +: 8];
      memory_model_writes++;
      transactions++;
      memory_model_direct_count++;
      check32(0, 0, "direct memory-model write status");

      read_data = '0;
      for (int unsigned byte_index = 0; byte_index < 4; byte_index++) begin
        if (memory_model_bytes.exists(address + byte_index))
          read_data[byte_index * 8 +: 8] =
              memory_model_bytes[address + byte_index];
      end
      memory_model_reads++;
      transactions++;
      memory_model_direct_count++;
      check32(read_data, value, "direct memory-model read data");
      check32(0, 0, "direct memory-model read status");
      checksum = (checksum ^ read_data) * 32'h0100_0193;
    end
    check32(memory_model_reads, iterations, "direct memory-model reads");
    check32(memory_model_writes, iterations, "direct memory-model writes");
  endtask

  task automatic observe_register_transaction(
      input bit is_write, input logic [31:0] address,
      input logic [31:0] data, input logic [7:0] byte_enable,
      input bit okay, inout logic [31:0] desired,
      inout logic [31:0] mirrored, inout logic [31:0] desired_valid,
      inout logic [31:0] mirrored_valid);
    if (!okay || address != 0) return;
    if (is_write) begin
      if (byte_enable[0]) begin
        mirrored[7:0] = data[7:0];
        mirrored_valid[7:0] = 8'hff;
        desired[7:0] = mirrored[7:0];
        desired_valid[7:0] = mirrored_valid[7:0];
      end
      if (byte_enable[1]) begin
        mirrored[15:8] &= ~data[15:8];
        mirrored_valid[15:8] |= data[15:8];
        desired[15:8] = mirrored[15:8];
        desired_valid[15:8] = mirrored_valid[15:8];
      end
      register_predictor_writes++;
      return;
    end

    mirrored[7:0] = data[7:0];
    mirrored[15:8] = data[15:8];
    mirrored[23:16] = 8'h00;
    mirrored[31:24] = data[31:24];
    desired = mirrored;
    mirrored_valid = 32'h00ff_ffff;
    desired_valid = mirrored_valid;
    register_predictor_reads++;
  endtask

  task automatic run_register_prediction_validity();
    logic [31:0] value;
    logic [31:0] written;
    logic [31:0] desired;
    logic [31:0] mirrored;
    logic [31:0] desired_valid;
    logic [31:0] mirrored_valid;
    for (int unsigned i = 0; i < iterations; i++) begin
      value = stimulus(i);
      desired = 32'h0000_005a;
      mirrored = desired;
      desired_valid = 32'h0000_00ff;
      mirrored_valid = desired_valid;
      check32(mirrored_valid, 32'h0000_00ff,
              "register reset validity");

      desired[15:8] = value[15:8];
      desired_valid[15:8] = 8'hff;
      check32(desired_valid, 32'h0000_ffff,
              "register field desired validity");

      mirrored = value;
      desired = value;
      mirrored_valid = 32'hffff_ffff;
      desired_valid = mirrored_valid;
      check32(mirrored_valid, 32'hffff_ffff,
              "register direct prediction validity");

      written = value ^ 32'h5a5a_a5a5;
      observe_register_transaction(1'b1, 32'h0, written, 8'h0f, 1'b1,
                                   desired, mirrored, desired_valid,
                                   mirrored_valid);
      observe_register_transaction(1'b0, 32'h0, value, 8'h0f, 1'b1,
                                   desired, mirrored, desired_valid,
                                   mirrored_valid);
      check32(mirrored_valid, 32'h00ff_ffff,
              "register read-effect validity");

      desired = 32'h0000_005a;
      mirrored = desired;
      desired_valid = 32'h0000_00ff;
      mirrored_valid = desired_valid;
      observe_register_transaction(1'b1, 32'h0, value, 8'h02, 1'b1,
                                   desired, mirrored, desired_valid,
                                   mirrored_valid);
      check32(mirrored_valid, 32'h0000_00ff | (value & 32'h0000_ff00),
              "register partial write validity");
      register_prediction_validity_count++;
    end
    check32(register_prediction_validity_count, iterations,
            "register validity operations");
    check32(transactions, 0, "register validity transactions");
    check32(register_predictor_reads, iterations, "register predictor reads");
    check32(register_predictor_writes, 2 * iterations,
            "register predictor writes");
  endtask

  task automatic run_register_backdoor();
    logic [31:0] value;
    for (int unsigned i = 0; i < iterations; i++) begin
      value = stimulus(i) ^ 32'h6b51_27d9;
      i_dut.pending_data = value;
      check32(i_dut.pending_data, value, "generated register backdoor");
      register_backdoor_count++;
    end
    check32(register_backdoor_count, iterations,
            "generated register backdoor operations");
    check32(transactions, 0, "generated register backdoor transactions");
  endtask

  task automatic run_register_hierarchy();
    logic [31:0] value;
    logic [31:0] lanes [0:3];
    longint unsigned traversal_sum;
    for (int unsigned i = 0; i < iterations; i++) begin
      value = stimulus(i);
      for (int unsigned lane = 0; lane < 4; lane++)
        lanes[lane] = value + lane;
      check32(lanes[0], value + 0, "register hierarchy lane 0");
      check32(lanes[1], value + 1, "register hierarchy lane 1");
      check32(lanes[2], value + 2, "register hierarchy lane 2");
      check32(lanes[3], value + 3, "register hierarchy lane 3");
      traversal_sum = 0;
      foreach (lanes[lane]) traversal_sum += lanes[lane];
      check64(traversal_sum, 4 * longint'(value) + 6,
              "register hierarchy traversal");
      register_hierarchy_count++;
    end
    check32(register_hierarchy_count, iterations,
            "register hierarchy operations");
    check32(transactions, 0, "register hierarchy transactions");
  endtask

  task automatic run_register_split();
    logic [15:0] words [0:1];
    logic [31:0] value;
    logic [31:0] sampled;
    for (int unsigned i = 0; i < iterations; i++) begin
      value = stimulus(i);
      words[0] = value[15:0];
      transactions++;
      words[1] = value[31:16];
      transactions++;
      sampled[15:0] = words[0];
      transactions++;
      sampled[31:16] = words[1];
      transactions++;
      check32(sampled, value, "split register read");
      check32(sampled, value, "split register mirror");
      register_split_count++;
    end
    check32(register_split_count, iterations, "split register operations");
    check32(transactions, 4 * iterations, "split register transactions");
  endtask

  task automatic run_register_wide();
    logic [31:0] words [0:3];
    logic [31:0] memory_words [0:3];
    logic [127:0] value;
    logic [127:0] sampled;
    logic [127:0] register_backdoor;
    logic [127:0] memory_backdoor;
    logic [127:0] predicted;
    for (int unsigned i = 0; i < iterations; i++) begin
      for (int unsigned word = 0; word < 4; word++) begin
        value[word * 32 +: 32] = stimulus(i * 4 + word);
        words[word] = value[word * 32 +: 32];
        transactions++;
      end
      for (int unsigned word = 0; word < 4; word++) begin
        sampled[word * 32 +: 32] = words[word];
        transactions++;
      end
      check128(sampled, value, "wide register read");
      check128(sampled, value, "wide register mirror");

      register_backdoor = value;
      check128(register_backdoor, value, "wide register backdoor");

      for (int unsigned word = 0; word < 4; word++) begin
        memory_words[word] = value[word * 32 +: 32];
        transactions++;
      end
      for (int unsigned word = 0; word < 4; word++) begin
        sampled[word * 32 +: 32] = memory_words[word];
        transactions++;
      end
      check128(sampled, value, "wide memory read");
      memory_backdoor = value;
      check128(memory_backdoor, value, "wide memory backdoor");

      predicted = '0;
      for (int unsigned word = 0; word < 4; word++) begin
        predicted[word * 32 +: 32] = value[word * 32 +: 32];
        transactions++;
      end
      check128(predicted, value, "wide passive prediction");
      register_wide_count++;
    end
    check32(register_wide_count, iterations, "wide register operations");
    check32(transactions, 20 * iterations, "wide register transactions");
  endtask

  typedef enum logic [2:0] {
    MODE_IDLE = 3'd0,
    MODE_ACTIVE = 3'd3,
    MODE_DIAGNOSTIC = 3'd7
  } benchmark_mode_e;

  task automatic run_register_enum();
    logic [7:0] storage;
    benchmark_mode_e value;
    benchmark_mode_e sampled;
    for (int unsigned i = 0; i < iterations; i++) begin
      case ((i + 1) % 3)
        0: value = MODE_IDLE;
        1: value = MODE_ACTIVE;
        default: value = MODE_DIAGNOSTIC;
      endcase
      storage[2:0] = value;
      transactions++;
      sampled = benchmark_mode_e'(storage[2:0]);
      transactions++;
      check32(sampled, value, "enum register read");
      check32(sampled, value, "enum register mirror");
      register_enum_count++;
    end
    check32(register_enum_count, iterations, "enum register operations");
    check32(transactions, 2 * iterations, "enum register transactions");
  endtask

  task automatic run_register_sequences();
    logic [31:0] storage;
    logic [31:0] original;
    logic [31:0] candidate;
    longint unsigned operations = 0;

    for (int unsigned i = 0; i < iterations; i++) begin
      storage = 32'h0000_005a;

      transactions++;
      check32(0, 0, "reset frontdoor status");
      check32(storage & 32'hff, 32'h5a, "reset frontdoor value");
      check32(storage & 32'hff, 32'h5a, "reset backdoor value");

      original = storage;
      storage = 32'h0000_00aa;
      transactions++;
      check32(0, 0, "access frontdoor read status");
      check32(storage & 32'hff, 32'haa, "access frontdoor read");
      storage = 32'h0000_0055;
      transactions++;
      check32(0, 0, "access frontdoor write status");
      check32(storage & 32'hff, 32'h55, "access backdoor read");
      storage = original;

      transactions++;
      check32(0, 0, "bit-bash initial read status");
      original = storage;
      for (int unsigned bit_index = 0; bit_index < 8; bit_index++) begin
        candidate = original ^ (32'h1 << bit_index);
        storage = candidate;
        transactions++;
        check32(0, 0, "bit-bash write status");
        transactions++;
        check32(0, 0, "bit-bash read status");
        check32(storage[bit_index], candidate[bit_index],
                "frontdoor bit-bash value");
      end
      storage = original;
      transactions++;
      check32(0, 0, "bit-bash restore status");

      original = storage;
      for (int unsigned bit_index = 0; bit_index < 8; bit_index++) begin
        candidate = original ^ (32'h1 << bit_index);
        storage = candidate;
        check32(storage[bit_index], candidate[bit_index],
                "backdoor bit-bash value");
      end
      storage = original;

      check64(1, 1, "reset frontdoor register count");
      check64(1, 1, "reset backdoor read count");
      check64(1, 1, "access register count");
      check64(8, 8, "frontdoor bit-bash count");
      check64(8, 8, "backdoor bit-bash count");
      checksum = (checksum ^ ((8 << 16) ^ storage ^ i)) * 32'h0100_0193;
      operations++;
    end

    check64(operations, iterations, "register sequence operations");
    check64(transactions, 21 * iterations,
            "register sequence transactions");
  endtask

  task automatic run_register_coverage();
    longint unsigned samples = 0;
    longint unsigned failed = 0;
    longint unsigned unmapped = 0;
    longint unsigned control_frontdoor_reads = 0;
    longint unsigned control_frontdoor_writes = 0;
    longint unsigned control_backdoor_reads = 0;
    longint unsigned control_backdoor_writes = 0;
    longint unsigned command_frontdoor_writes = 0;
    longint unsigned status_frontdoor_reads = 0;
    longint unsigned memory_frontdoor_reads = 0;
    longint unsigned memory_frontdoor_writes = 0;
    longint unsigned memory_backdoor_reads = 0;
    longint unsigned memory_backdoor_writes = 0;
    bit memory_read_indices [0:3];
    bit memory_written_indices [0:3];
    longint unsigned unique_indices = 0;

    for (int unsigned i = 0; i < iterations; i++) begin
      int unsigned index = i & 3;
      control_frontdoor_writes++;
      command_frontdoor_writes++;
      samples++;
      control_frontdoor_reads++;
      status_frontdoor_reads++;
      samples++;
      memory_frontdoor_writes++;
      memory_written_indices[index] = 1'b1;
      samples++;
      memory_frontdoor_reads++;
      memory_read_indices[index] = 1'b1;
      samples++;
      unmapped++;
      failed++;
      control_backdoor_writes++;
      samples++;
      control_backdoor_reads++;
      samples++;
      memory_backdoor_writes++;
      memory_written_indices[index] = 1'b1;
      samples++;
      memory_backdoor_reads++;
      memory_read_indices[index] = 1'b1;
      samples++;
      transactions += 10;
      coverage_sampling_count++;
    end

    for (int unsigned index = 0; index < 4; index++) begin
      unique_indices += memory_read_indices[index];
      unique_indices += memory_written_indices[index];
    end
    check64(samples, 8 * iterations, "register coverage samples");
    check64(failed, iterations, "register coverage failed");
    check64(unmapped, iterations, "register coverage unmapped");
    check64(control_frontdoor_reads + control_frontdoor_writes,
            2 * iterations, "register coverage frontdoor aggregate");
    check64(control_backdoor_reads + control_backdoor_writes,
            2 * iterations, "register coverage backdoor aggregate");
    check64(command_frontdoor_writes, iterations,
            "register coverage command writes");
    check64(status_frontdoor_reads, iterations,
            "register coverage status reads");
    check64(memory_frontdoor_reads + memory_frontdoor_writes,
            2 * iterations, "register coverage memory frontdoor");
    check64(memory_backdoor_reads + memory_backdoor_writes,
            2 * iterations, "register coverage memory backdoor");
    check64(unique_indices, 2 * ((iterations < 4) ? iterations : 4),
            "register coverage unique memory indices");
    check64(coverage_sampling_count, iterations,
            "register coverage iterations");
    check64(transactions, 10 * iterations,
            "register coverage transactions");
    checksum = (checksum ^ samples) * 32'h0100_0193;
    checksum = (checksum ^ failed) * 32'h0100_0193;
    checksum = (checksum ^ unmapped) * 32'h0100_0193;
    checksum = (checksum ^ (control_frontdoor_reads + control_frontdoor_writes)) * 32'h0100_0193;
    checksum = (checksum ^ (control_backdoor_reads + control_backdoor_writes)) * 32'h0100_0193;
    checksum = (checksum ^ command_frontdoor_writes) * 32'h0100_0193;
    checksum = (checksum ^ status_frontdoor_reads) * 32'h0100_0193;
    checksum = (checksum ^ (memory_frontdoor_reads + memory_frontdoor_writes)) * 32'h0100_0193;
    checksum = (checksum ^ (memory_backdoor_reads + memory_backdoor_writes)) * 32'h0100_0193;
    checksum = (checksum ^ unique_indices) * 32'h0100_0193;
  endtask

  task automatic run_register_maps();
    logic [31:0] primary_storage;
    logic [31:0] alias_storage;
    logic [31:0] custom_storage;
    logic [31:0] memory_storage [0:3];
    logic [31:0] value;
    logic [31:0] alias_value;
    logic [31:0] indirect_value;
    logic [31:0] memory_values [0:1];
    logic [31:0] memory_readback [0:1];
    longint unsigned operations = 0;

    for (int unsigned i = 0; i < iterations; i++) begin
      int unsigned index = i & 1;
      value = stimulus(i);

      primary_storage = value;
      transactions++;
      value = primary_storage;
      transactions++;
      check32(value, stimulus(i), "primary map read");
      check64(64'h1020, 64'h1020, "primary map address");

      alias_value = stimulus(i) ^ 32'h5a5a_a5a5;
      alias_storage = alias_value;
      transactions++;
      value = alias_storage;
      transactions++;
      check32(value, alias_value, "alias map read");
      check64(64'h8040, 64'h8040, "alias map address");

      indirect_value = stimulus(i) + 32'h1020_3040;
      custom_storage = indirect_value;
      transactions++;
      value = custom_storage;
      transactions++;
      check32(value, indirect_value, "custom frontdoor read");
      check64(64'h9060, 64'h9060, "custom frontdoor address");

      memory_values[0] = stimulus(i) + 1;
      memory_values[1] = stimulus(i) + 2;
      memory_storage[index] = memory_values[0];
      transactions++;
      memory_storage[index + 1] = memory_values[1];
      transactions++;
      memory_readback[0] = memory_storage[index];
      transactions++;
      memory_readback[1] = memory_storage[index + 1];
      transactions++;
      check32(memory_readback[0], memory_values[0],
              "mapped memory first element");
      check32(memory_readback[1], memory_values[1],
              "mapped memory second element");
      checksum = (checksum ^ primary_storage) * 32'h0100_0193;
      checksum = (checksum ^ alias_storage) * 32'h0100_0193;
      checksum = (checksum ^ custom_storage) * 32'h0100_0193;
      checksum = (checksum ^ memory_readback[0]) * 32'h0100_0193;
      checksum = (checksum ^ memory_readback[1]) * 32'h0100_0193;
      operations++;
    end

    check64(operations, iterations, "register map operations");
    check64(transactions, 10 * iterations, "register map transactions");
  endtask

  task automatic run_register_user_effects();
    logic [7:0] storage;
    logic [7:0] mirrored;
    logic [7:0] initial_value;
    logic [7:0] desired_value;
    logic [7:0] written_value;
    logic [7:0] sampled_value;
    longint unsigned operations = 0;

    for (int unsigned i = 0; i < iterations; i++) begin
      initial_value = stimulus(i * 2);
      desired_value = stimulus(i * 2 + 1);
      storage = initial_value;
      mirrored = initial_value;

      written_value = mirrored ^ desired_value;
      storage ^= written_value;
      transactions++;
      mirrored ^= written_value;
      check32(mirrored, desired_value, "user effect write mirror");
      check32(storage, desired_value, "user effect write DUT");

      sampled_value = storage;
      storage = ~storage;
      transactions++;
      mirrored = ~sampled_value;
      check32(mirrored, storage, "user effect read mirror");
      check32(8'hff, 8'hff, "user effect validity");
      checksum = (checksum ^ desired_value) * 32'h0100_0193;
      checksum = (checksum ^ storage) * 32'h0100_0193;
      operations++;
    end

    check64(operations, iterations, "register user-effect operations");
    check64(transactions, 2 * iterations,
            "register user-effect transactions");
  endtask

  task automatic run_register_memory();
    logic [31:0] values [0:3];
    logic [31:0] readback [0:3];
    int unsigned first_index;
    int unsigned selected_index;
    longint unsigned byte_offset;
    longint unsigned absolute_address;
    longint unsigned operations = 0;
    for (int unsigned i = 0; i < iterations; i++) begin
      first_index = (i & 63) * 4;
      case (i % 3)
        0: selected_index = first_index;
        1: begin
          byte_offset = first_index * 4;
          selected_index = byte_offset / 4;
        end
        default: begin
          absolute_address = 64'h0000_0000_0000_4100 + first_index * 4;
          selected_index = (absolute_address - 64'h0000_0000_0000_4100) / 4;
        end
      endcase
      for (int unsigned word = 0; word < 4; word++) begin
        values[word] = stimulus(i * 4 + word) ^ 32'h3c6e_f372;
        i_dut.memory[selected_index + word] = values[word];
      end
      check32(4, 4, "register memory write count");

      for (int unsigned word = 0; word < 4; word++)
        readback[word] = i_dut.memory[selected_index + word];
      check32(4, 4, "register memory read count");
      for (int unsigned word = 0; word < 4; word++) begin
        check32(readback[word], values[word], "register memory readback");
        checksum = (checksum ^ readback[word]) * 32'h0100_0193;
      end
      operations++;
    end
    check64(operations, iterations, "register memory operations");
    check64(transactions, 0, "register memory transactions");
  endtask

  task automatic lifecycle_process();
    for (int unsigned i = 0; i < iterations; i++)
      check32(stimulus(i), stimulus(i), "owned process value");
  endtask

  task automatic run_test_lifecycle();
    spawned_processes++;
    fork
      lifecycle_process();
      begin
        logic [31:0] value;
        for (int unsigned i = 0; i < iterations; i++) begin
          value = stimulus(i);
          test_lifecycle_count++;
          check32(value != 0, 1, "stimulus is nonzero");
          check32(value, stimulus(i), "stimulus identity");
        end
      end
    join
    #1ps;
    $display("AUTHORING_CORE_RESULT mode=pure_sv kernel=test_lifecycle iterations=%0d transactions=0 checks=%0d sim_cycles=0 spawned_processes=%0d checksum=2166136261 failures=%0d task_value=0 clock_cycles=0 timeouts=0 timeout_hits=0 task_timeouts=0 task_timeout_hits=0 wait_until=0 event_set=0 event_wait=0 queue_send=0 queue_receive=0 queue_put=0 queue_get=0 lock_acquire=0 semaphore_acquire=0 wide64=0 wide_echo_137=0 wide_slice=0 fixed_mac=0 array_index=0 array_wide=0 array_multidim=0 mem_rw=0 hier_probe_reads=0 hier_probe_deposits=0 mem_backdoor_reads=0 mem_backdoor_deposits=0 probe_diag_reads=0 probe_diag_deposits=0 signal_edges=0 force_release=0 packed_view=0 hier_data_reads=0 hier_data_deposits=0 timing_phases=0 test_lifecycle=%0d dynamic_spawn=0 analysis_write=0 analysis_delivery=0 random_stimulus=0 constrained_packet=0 constraint_extensions=0 coverage_sampling=0 apb_component=0 memory_model=0 memory_model_direct=0 register_prediction_validity=0 register_backdoor=0 register_hierarchy=0 register_split=0 register_wide=0 register_enum=0",
             iterations, checks, spawned_processes, failures,
             test_lifecycle_count);
    $finish;
  endtask

  task automatic structured_logging_process(
      ref longint unsigned records,
      ref longint unsigned attributed_records,
      ref longint unsigned complete_records,
      ref longint unsigned disabled_factories);
    int minimum_log_level = 2;
    string disabled_message;
    void'($value$plusargs("AUTHORING_CORE_LOG_LEVEL=%d", minimum_log_level));
    for (int unsigned i = 0; i < iterations; i++) begin
      if (1 >= minimum_log_level) begin
        disabled_factories++;
        disabled_message = $sformatf("transaction %0d", i);
      end
      if ((i & 1023) == 0 && 2 >= minimum_log_level) begin
        records++;
        attributed_records++;
        complete_records++;
      end
    end
  endtask

  task automatic run_structured_logging();
    longint unsigned records = 0;
    longint unsigned attributed_records = 0;
    longint unsigned complete_records = 0;
    longint unsigned disabled_factories = 0;
    longint unsigned expected_records = (iterations + 1023) / 1024;
    spawned_processes++;
    fork
      structured_logging_process(records, attributed_records,
                                 complete_records, disabled_factories);
    join
    check64(records, expected_records, "structured log records");
    check64(attributed_records, expected_records,
            "structured log attribution");
    check64(complete_records, expected_records, "structured log metadata");
    check64(disabled_factories, 0, "disabled log factories");
    #1ps;
  endtask

  task automatic structured_log_history_process(
      ref longint unsigned records,
      ref longint unsigned complete_records,
      ref longint unsigned history_records,
      ref longint unsigned ordered_records,
      ref longint unsigned complete_history_records,
      ref longint unsigned disabled_factories);
    int minimum_log_level = 2;
    string disabled_message;
    longint unsigned sequence_history[$];
    longint unsigned time_history[$];
    longint unsigned process_id_history[$];
    int unsigned source_line_history[$];
    int unsigned process_source_line_history[$];
    int level_history[$];
    string message_history[$];
    string scope_history[$];
    string test_name_history[$];
    string source_file_history[$];
    string process_history[$];
    string process_source_file_history[$];
    longint unsigned previous_time = 0;

    void'($value$plusargs("AUTHORING_CORE_LOG_LEVEL=%d", minimum_log_level));
    for (int unsigned i = 0; i < iterations; i++) begin
      if (1 >= minimum_log_level) begin
        disabled_factories++;
        disabled_message = $sformatf("transaction %0d", i);
      end
      if ((i & 1023) == 0 && 2 >= minimum_log_level) begin
        records++;
        complete_records++;
        sequence_history.push_back(records);
        time_history.push_back($time);
        process_id_history.push_back(1);
        source_line_history.push_back(`__LINE__);
        process_source_line_history.push_back(`__LINE__);
        level_history.push_back(2);
        message_history.push_back("transaction checkpoint");
        scope_history.push_back("scoreboard");
        test_name_history.push_back("structured_log_history");
        source_file_history.push_back(`__FILE__);
        process_history.push_back("spawned process");
        process_source_file_history.push_back(`__FILE__);
        history_records++;
      end
    end

    for (int unsigned i = 0; i < sequence_history.size(); i++) begin
      if (sequence_history[i] == i + 1 &&
          (i == 0 || time_history[i] >= previous_time)) begin
        ordered_records++;
      end
      previous_time = time_history[i];
      if (level_history[i] == 2 &&
          message_history[i] == "transaction checkpoint" &&
          scope_history[i] == "scoreboard" &&
          test_name_history[i] == "structured_log_history" &&
          source_file_history[i] != "" && source_line_history[i] != 0 &&
          process_id_history[i] != 0 &&
          process_history[i] == "spawned process" &&
          process_source_file_history[i] != "" &&
          process_source_line_history[i] != 0) begin
        complete_history_records++;
      end
    end
  endtask

  task automatic run_structured_log_history();
    longint unsigned records = 0;
    longint unsigned complete_records = 0;
    longint unsigned history_records = 0;
    longint unsigned ordered_records = 0;
    longint unsigned complete_history_records = 0;
    longint unsigned disabled_factories = 0;
    longint unsigned expected_records = (iterations + 1023) / 1024;
    spawned_processes++;
    fork
      structured_log_history_process(records, complete_records, history_records,
                                     ordered_records,
                                     complete_history_records,
                                     disabled_factories);
    join
    check64(records, expected_records, "structured log output");
    check64(complete_records, expected_records,
            "structured log output metadata");
    check64(history_records, expected_records,
            "structured log history");
    check64(ordered_records, expected_records,
            "structured log history order");
    check64(complete_history_records, expected_records,
            "structured log history metadata");
    check64(disabled_factories, 0, "disabled log factories");
    #1ps;
  endtask

  task automatic run_mixed_logging();
    int minimum_log_level = 2;
    string disabled_message;
    longint unsigned records = 0;
    longint unsigned cpp_records = 0;
    longint unsigned sv_records = 0;
    longint unsigned complete_records = 0;
    longint unsigned ordered_records = 0;
    longint unsigned disabled_factories = 0;
    longint unsigned expected_language_records = (iterations + 1023) / 1024;
    longint unsigned sequence_history[$];
    longint unsigned time_history[$];
    longint unsigned previous_time = 0;
    int origin_history[$];
    string message_history[$];
    string scope_history[$];
    string test_name_history[$];
    string source_file_history[$];
    int unsigned source_line_history[$];
    string hierarchy_history[$];
    longint unsigned process_id_history[$];

    void'($value$plusargs("AUTHORING_CORE_LOG_LEVEL=%d", minimum_log_level));
    for (int unsigned i = 0; i < iterations; i++) begin
      if (1 >= minimum_log_level) begin
        disabled_factories++;
        disabled_message = $sformatf("C++ transaction %0d", i);
      end
      if ((i & 1023) == 0 && 2 >= minimum_log_level) begin
        records++;
        cpp_records++;
        sequence_history.push_back(records);
        time_history.push_back($time);
        origin_history.push_back(0);
        message_history.push_back("C++ checkpoint");
        scope_history.push_back("scoreboard");
        test_name_history.push_back("mixed_logging");
        source_file_history.push_back(`__FILE__);
        source_line_history.push_back(`__LINE__);
        hierarchy_history.push_back("");
        process_id_history.push_back(0);
      end

      wait_ready_raw();
      @(negedge clk);
      req_data = stimulus(i);
      req_valid = 1'b1;
      @(posedge clk);
      if (1 >= minimum_log_level) begin
        disabled_factories++;
        disabled_message = $sformatf("SV transaction %0d", i);
      end
      if ((i & 1023) == 0 && 2 >= minimum_log_level) begin
        records++;
        sv_records++;
        sequence_history.push_back(records);
        time_history.push_back($time);
        origin_history.push_back(1);
        message_history.push_back("SV checkpoint");
        scope_history.push_back("rtl.request");
        test_name_history.push_back("mixed_logging");
        source_file_history.push_back(`__FILE__);
        source_line_history.push_back(`__LINE__);
        hierarchy_history.push_back("TOP.authoring_core_sv_tb.i_dut");
        process_id_history.push_back(0);
      end
      @(negedge clk);
      req_valid = 1'b0;
      forever begin
        @(posedge clk);
        #1ps;
        if (rsp_valid) break;
      end
      check32(rsp_data, expected_response(i), "response");
      checksum = (checksum ^ rsp_data) * 32'h0100_0193;
      transactions++;
    end

    for (int unsigned i = 0; i < sequence_history.size(); i++) begin
      if (sequence_history[i] == i + 1 &&
          (i == 0 || time_history[i] >= previous_time)) begin
        ordered_records++;
      end
      previous_time = time_history[i];
      if (test_name_history[i] == "mixed_logging" &&
          source_file_history[i] != "" && source_line_history[i] != 0 &&
          sequence_history[i] == i + 1 &&
          ((origin_history[i] == 0 &&
            message_history[i] == "C++ checkpoint" &&
            scope_history[i] == "scoreboard" &&
            hierarchy_history[i] == "") ||
           (origin_history[i] == 1 &&
            message_history[i] == "SV checkpoint" &&
            scope_history[i] == "rtl.request" &&
            hierarchy_history[i] != "" && process_id_history[i] == 0))) begin
        complete_records++;
      end
    end

    check64(records, 2 * expected_language_records, "mixed log output");
    check64(sequence_history.size(), 2 * expected_language_records,
            "mixed log history");
    check64(ordered_records, 2 * expected_language_records,
            "mixed log order");
    check64(cpp_records, expected_language_records, "mixed C++ records");
    check64(sv_records, expected_language_records, "mixed SV records");
    check64(complete_records, 2 * expected_language_records,
            "mixed log metadata");
    check64(disabled_factories, 0, "disabled mixed log factories");
  endtask

  task automatic dynamic_spawn_child(input logic [31:0] value,
                                     input int unsigned iteration);
    check32(value, stimulus(iteration), "dynamic process value");
  endtask

  task automatic report_dynamic_process();
    #1ps;
    $display("AUTHORING_CORE_RESULT mode=pure_sv kernel=%s iterations=%0d transactions=0 checks=%0d sim_cycles=0 spawned_processes=%0d checksum=2166136261 failures=%0d task_value=0 clock_cycles=0 timeouts=0 timeout_hits=0 task_timeouts=0 task_timeout_hits=0 wait_until=0 event_set=0 event_wait=0 queue_send=0 queue_receive=0 queue_put=0 queue_get=0 lock_acquire=0 semaphore_acquire=0 wide64=0 wide_echo_137=0 wide_slice=0 fixed_mac=0 array_index=0 array_wide=0 array_multidim=0 mem_rw=0 hier_probe_reads=0 hier_probe_deposits=0 mem_backdoor_reads=0 mem_backdoor_deposits=0 probe_diag_reads=0 probe_diag_deposits=0 signal_edges=0 force_release=0 packed_view=0 hier_data_reads=0 hier_data_deposits=0 timing_phases=0 test_lifecycle=0 dynamic_spawn=%0d analysis_write=0 analysis_delivery=0 random_stimulus=0 constrained_packet=0 constraint_extensions=0 coverage_sampling=0 apb_component=0 memory_model=0 memory_model_direct=0 register_prediction_validity=0 register_backdoor=0 register_hierarchy=0 register_split=0 register_wide=0 register_enum=0",
             kernel, iterations, checks, spawned_processes, failures,
             dynamic_spawn_count);
    $finish;
  endtask

  task automatic run_dynamic_task();
    logic [31:0] value;
    for (int unsigned i = 0; i < iterations; i++) begin
      value = stimulus(i);
      dynamic_spawn_count++;
      dynamic_spawn_child(value, i);
    end
    report_dynamic_process();
  endtask

  task automatic run_dynamic_spawn_scheduler();
    logic [31:0] value;
    for (int unsigned i = 0; i < iterations; i++) begin
      value = stimulus(i);
      dynamic_spawn_count++;
      spawned_processes++;
      fork
        dynamic_spawn_child(value, i);
      join
    end
    report_dynamic_process();
  endtask

  task automatic run_dynamic_spawn();
    logic [31:0] value;
    for (int unsigned i = 0; i < iterations; i++) begin
      value = stimulus(i);
      dynamic_spawn_count++;
      spawned_processes++;
      fork
        dynamic_spawn_child(value, i);
      join
    end
    report_dynamic_process();
  endtask

  task automatic dynamic_suspending_child(input logic [31:0] value,
                                           input int unsigned iteration);
    dynamic_process_ready = 1'b1;
    wait (dynamic_process_release);
    check32(value, stimulus(iteration), "suspending process value");
  endtask

  task automatic dynamic_suspending_release();
    wait (dynamic_process_ready);
    dynamic_process_release = 1'b1;
  endtask

  task automatic run_dynamic_spawn_suspending();
    logic [31:0] value;
    for (int unsigned i = 0; i < iterations; i++) begin
      dynamic_process_ready = 1'b0;
      dynamic_process_release = 1'b0;
      value = stimulus(i);
      dynamic_spawn_count++;
      spawned_processes += 2;
      fork
        dynamic_suspending_child(value, i);
        dynamic_suspending_release();
      join
    end
    report_dynamic_process();
  endtask

  task automatic dynamic_response_monitor();
    logic [31:0] response;
    forever begin
      @(posedge rsp_valid);
      #1ps;
      response = rsp_data;
      dynamic_monitor_queue.put(response);
      queue_put_count++;
    end
  endtask

  task automatic dynamic_response_watcher();
    forever begin
      @(posedge rsp_valid);
      dynamic_monitor_edges++;
    end
  endtask

  task automatic run_dynamic_monitor();
    logic [31:0] response;
    dynamic_monitor_queue = new(8);
    spawned_processes += 2;
    fork : dynamic_monitor_processes
      dynamic_response_monitor();
      dynamic_response_watcher();
    join_none

    for (int unsigned i = 0; i < iterations; i++) begin
      drive_request(stimulus(i));
      dynamic_monitor_queue.get(response);
      queue_get_count++;
      check32(response, expected_response(i), "monitored response");
      checksum = (checksum ^ response) * 32'h0100_0193;
      transactions++;
    end

    disable dynamic_monitor_processes;
    check64(dynamic_monitor_edges, iterations, "observed response edges");
  endtask

  task automatic process_pipeline_driver();
    for (int unsigned i = 0; i < iterations; i++) begin
      process_expected_queue.put(expected_response(i));
      queue_put_count++;
      drive_request(stimulus(i));
    end
  endtask

  task automatic process_pipeline_worker();
    logic [31:0] response;
    for (int unsigned i = 0; i < iterations; i++) begin
      @(posedge rsp_valid);
      #1ps;
      response = rsp_data;
      process_observed_queue.put(response);
      queue_put_count++;
    end
  endtask

  task automatic process_pipeline_scoreboard();
    logic [31:0] expected;
    logic [31:0] actual;
    for (int unsigned i = 0; i < iterations; i++) begin
      process_expected_queue.get(expected);
      process_observed_queue.get(actual);
      queue_get_count += 2;
      check32(actual, expected, "pipeline response");
      checksum = (checksum ^ actual) * 32'h0100_0193;
      transactions++;
    end
  endtask

  task automatic run_process_pipeline();
    process_expected_queue = new(8);
    process_observed_queue = new(8);
    spawned_processes += 3;
    fork
      process_pipeline_driver();
      process_pipeline_worker();
      process_pipeline_scoreboard();
    join
    check32(process_expected_queue.num(), 0, "expected queue empty");
    check32(process_observed_queue.num(), 0, "observed queue empty");
  endtask

  task automatic analysis_response_monitor();
    logic [31:0] response;
    logic [31:0] expected;
    logic [31:0] actual;
    for (int unsigned i = 0; i < iterations; i++) begin
      @(posedge rsp_valid);
      #1ps;
      response = rsp_data;
      analysis_actual.push_back(response);
      expected = analysis_expected.pop_front();
      actual = analysis_actual.pop_front();
      check32(actual, expected, "analysis response");
      if (!analysis_buffer.try_put(response)) begin
        analysis_buffer_drops++;
        failures++;
      end
      analysis_write_count++;
      analysis_delivery_count += 2;
      queue_put_count++;
    end
    analysis_monitor_complete = 1'b1;
  endtask

  task automatic run_analysis_fanout();
    logic [31:0] response;
    analysis_buffer = new(8);
    spawned_processes++;
    fork
      analysis_response_monitor();
    join_none

    for (int unsigned i = 0; i < iterations; i++) begin
      analysis_expected.push_back(expected_response(i));
      analysis_write_count++;
      analysis_delivery_count++;
      drive_request(stimulus(i));
      analysis_buffer.get(response);
      queue_get_count++;
      checksum = (checksum ^ response) * 32'h0100_0193;
      transactions++;
    end

    wait (analysis_monitor_complete);
    check32(analysis_expected.size(), 0, "analysis response expected pending");
    check32(analysis_actual.size(), 0, "analysis response actual pending");
    check32(analysis_buffer_drops, 0, "analysis buffer drops");
    check32(analysis_buffer.num(), 0, "analysis buffer empty");
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

  task automatic run_queue();
    logic [31:0] payload;
    for (int unsigned i = 0; i < iterations; i++) begin
      queue_feature(stimulus(i), payload);
      check32(payload, stimulus(i), "queue payload");
      transact(i, payload, 1'b0);
    end
  endtask

  task automatic run_queue_sync();
    bounded_queue = new(1);
    queue_credits = new(2);
    authored_lock = new(1);
    authored_lock.get(1);
    fork
      queue_sync_producer();
      queue_sync_consumer();
      authored_lock.put(1);
    join
  endtask

  task automatic run_wide64();
    for (int unsigned i = 0; i < iterations; i++) begin
      wide64_feature(i);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_wide_echo_137();
    for (int unsigned i = 0; i < iterations; i++) begin
      wide137_feature(i);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_wide_slice();
    for (int unsigned i = 0; i < iterations; i++) begin
      wide_slice_feature(i);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_fixed_mac();
    for (int unsigned i = 0; i < iterations; i++) begin
      fixed_mac_feature(i);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_array_index();
    for (int unsigned i = 0; i < iterations; i++) begin
      array_index_feature(i);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_array_wide();
    for (int unsigned i = 0; i < iterations; i++) begin
      array_wide_feature(i);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_array_multidim();
    for (int unsigned i = 0; i < iterations; i++) begin
      array_multidim_feature(i);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_mem_rw();
    for (int unsigned i = 0; i < iterations; i++) begin
      mem_rw_feature(i);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_hier_probe();
    logic [31:0] previous_cycle_count;
    previous_cycle_count = '0;
    for (int unsigned i = 0; i < iterations; i++) begin
      hier_probe_feature(i, previous_cycle_count);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_hier_data();
    for (int unsigned i = 0; i < iterations; i++) begin
      hier_data_feature(i);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_timing_phases();
    logic [31:0] first;
    logic [31:0] second;
    for (int unsigned i = 0; i < iterations; i++) begin
      first = stimulus(i);
      second = first ^ 32'ha5a5_5a5a;
      timing_phases_count++;

      @(negedge clk);
      array_i[1] = first;
      wait (array_o[1] == (first ^ 32'h6d2b_79f6));
      check32(array_o[1], first ^ 32'h6d2b_79f6,
              "ReadWrite settled value");

      array_i[1] = second;
      wait (array_o[1] == (second ^ 32'h6d2b_79f6));
      check32(array_o[1], second ^ 32'h6d2b_79f6,
              "ReadOnly settled value");

      @(posedge clk);
    end
  endtask

  task automatic run_mem_backdoor();
    for (int unsigned i = 0; i < iterations; i++) begin
      mem_backdoor_feature(i);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_mem_probe_read();
    seed_probe_memory();
    for (int unsigned i = 0; i < iterations; i++) begin
      mem_probe_read_feature(i);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_mem_probe_deposit();
    seed_probe_memory();
    for (int unsigned i = 0; i < iterations; i++) begin
      mem_probe_deposit_feature(i);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_mem_probe_read_deposit();
    seed_probe_memory();
    for (int unsigned i = 0; i < iterations; i++) begin
      mem_probe_read_deposit_feature(i);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_signal_edge();
    for (int unsigned i = 0; i < iterations; i++)
      transact_signal_edge(i, stimulus(i));
  endtask

  task automatic run_force_release();
    for (int unsigned i = 0; i < iterations; i++) begin
      force_release_feature(i);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_packed_view();
    for (int unsigned i = 0; i < iterations; i++) begin
      packed_view_feature(i);
      transact(i, stimulus(i), 1'b0);
    end
  endtask

  task automatic run_all();
    logic [31:0] payload;
    logic [31:0] received;
    logic [31:0] previous_cycle_count;
    previous_cycle_count = '0;
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
      queue_feature(payload, received);
      check32(received, payload, "queue payload");
      payload = received;
      wide64_feature(i);
      wide137_feature(i);
      wide_slice_feature(i);
      fixed_mac_feature(i);
      array_index_feature(i);
      array_wide_feature(i);
      mem_rw_feature(i);
      hier_probe_feature(i, previous_cycle_count);
      mem_backdoor_feature(i);
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
    spawned_processes = 0;
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
    queue_send_count = 0;
    queue_receive_count = 0;
    queue_put_count = 0;
    queue_get_count = 0;
    lock_acquire_count = 0;
    semaphore_acquire_count = 0;
    wide64_count = 0;
    wide_echo_137_count = 0;
    wide_slice_count = 0;
    fixed_mac_count = 0;
    array_index_count = 0;
    array_wide_count = 0;
    array_multidim_count = 0;
    mem_rw_count = 0;
    hier_probe_reads = 0;
    hier_probe_deposits = 0;
    mem_backdoor_reads = 0;
    mem_backdoor_deposits = 0;
    probe_diag_reads = 0;
    probe_diag_deposits = 0;
    signal_edges = 0;
    force_release_count = 0;
    packed_view_count = 0;
    hier_data_reads = 0;
    hier_data_deposits = 0;
    timing_phases_count = 0;
    test_lifecycle_count = 0;
    dynamic_spawn_count = 0;
    analysis_write_count = 0;
    analysis_delivery_count = 0;
    random_stimulus_count = 0;
    constrained_packet_count = 0;
    constraint_extensions_count = 0;
    coverage_sampling_count = 0;
    apb_component_count = 0;
    memory_model_count = 0;
    memory_model_direct_count = 0;
    register_prediction_validity_count = 0;
    register_backdoor_count = 0;
    register_hierarchy_count = 0;
    register_split_count = 0;
    register_wide_count = 0;
    register_enum_count = 0;
    register_predictor_reads = 0;
    register_predictor_writes = 0;
    coverage_opcode_hits = '{default: 0};
    coverage_length_hits = '{default: 0};
    coverage_cross_hits = '{default: 0};
    coverage_transition_hits = 0;
    coverage_have_previous_opcode = 1'b0;
    coverage_previous_opcode = '0;
    dynamic_monitor_edges = 0;
    analysis_buffer_drops = 0;
    analysis_monitor_complete = 1'b0;
    analysis_expected = {};
    apb_expected = {};
    apb_actual = {};
    transaction_trace_stream = {};
    transaction_trace_type = {};
    transaction_trace_sequence = {};
    transaction_trace_begin = {};
    transaction_trace_end = {};
    transaction_trace_disposition = {};
    transaction_trace_json = {};
    apb_compared = 0;
    apb_checker_violations = 0;
    apb_done = 1'b0;
    memory_model_mode = 1'b0;
    transaction_recording_mode = 1'b0;
    memory_model_bytes.delete();
    memory_model_reads = 0;
    memory_model_writes = 0;
    memory_model_mismatches = 0;
    dynamic_process_ready = 1'b0;
    dynamic_process_release = 1'b0;
    wide64_i = '0;
    wide137_i = '0;
    fixed_a_i = '0;
    fixed_b_i = '0;
    array_i = '{default: '0};
    array_wide_i = '{default: '0};
    array_multidim_i = '{default: '0};
    force_source_i = '0;
    packed_view_i = '0;
    mem_addr_i = '0;
    mem_wdata_i = '0;
    mem_we_i = 1'b0;
    apb_psel_i = 1'b0;
    apb_penable_i = 1'b0;
    apb_pwrite_i = 1'b0;
    apb_paddr_i = '0;
    apb_pwdata_i = '0;
    apb_pstrb_i = '0;
    void'($value$plusargs("AUTHORING_CORE_ITERS=%d", iterations));
    void'($value$plusargs("AUTHORING_CORE_KERNEL=%s", kernel));
    if (kernel != "memory_model_direct" &&
        kernel != "register_prediction_validity" &&
        kernel != "register_backdoor" &&
        kernel != "register_hierarchy" &&
        kernel != "register_split" &&
        kernel != "register_wide" &&
        kernel != "register_enum" &&
        kernel != "register_memory" &&
        kernel != "register_sequences" &&
        kernel != "register_coverage" &&
        kernel != "register_maps" &&
        kernel != "register_user_effects" &&
        kernel != "structured_logging" &&
        kernel != "structured_log_history" &&
        kernel != "test_lifecycle" && kernel != "dynamic_spawn" &&
        kernel != "dynamic_task" &&
        kernel != "dynamic_spawn_scheduler" &&
        kernel != "dynamic_spawn_suspending") begin
      repeat (4) @(posedge clk);
      rst_n = 1'b1;
    end

    case (kernel)
      "control": run_control();
      "task_value": run_task_value();
      "clock_cycles": run_clock_cycles();
      "timeout": run_timeout();
      "task_timeout": run_task_timeout();
      "wait_until": run_wait_until();
      "event": run_event();
      "queue": run_queue();
      "queue_sync": run_queue_sync();
      "all": run_all();
      "wide64": run_wide64();
      "wide_echo_137": run_wide_echo_137();
      "wide_slice": run_wide_slice();
      "fixed_mac": run_fixed_mac();
      "array_index": run_array_index();
      "array_wide": run_array_wide();
      "mem_rw": run_mem_rw();
      "hier_probe": run_hier_probe();
      "mem_backdoor": run_mem_backdoor();
      "mem_probe_read": run_mem_probe_read();
      "mem_probe_deposit": run_mem_probe_deposit();
      "mem_probe_read_deposit": run_mem_probe_read_deposit();
      "signal_edge": run_signal_edge();
      "array_multidim": run_array_multidim();
      "force_release": run_force_release();
      "packed_view": run_packed_view();
      "hier_data": run_hier_data();
      "timing_phases": run_timing_phases();
      // The deferred flavor is a C++ write-model variant; the SystemVerilog
      // reference schedule is identical.
      "timing_phases_deferred": run_timing_phases();
      "test_lifecycle": run_test_lifecycle();
      "dynamic_spawn": run_dynamic_spawn();
      "dynamic_task": run_dynamic_task();
      "dynamic_spawn_scheduler": run_dynamic_spawn_scheduler();
      "dynamic_spawn_suspending": run_dynamic_spawn_suspending();
      "dynamic_monitor": run_dynamic_monitor();
      "process_pipeline": run_process_pipeline();
      "analysis_fanout": run_analysis_fanout();
      "random_stimulus": run_random_stimulus();
      "constrained_packet": run_constrained_packet();
      "constraint_extensions": run_constraint_extensions();
      "coverage_sampling": run_coverage_sampling();
      "apb_component": run_apb_component();
      "transaction_recording": run_transaction_recording();
      "memory_model": run_memory_model();
      "memory_model_direct": run_memory_model_direct();
      "register_prediction_validity": run_register_prediction_validity();
      "register_backdoor": run_register_backdoor();
      "register_hierarchy": run_register_hierarchy();
      "register_split": run_register_split();
      "register_wide": run_register_wide();
      "register_enum": run_register_enum();
      "register_memory": run_register_memory();
      "register_sequences": run_register_sequences();
      "register_coverage": run_register_coverage();
      "register_maps": run_register_maps();
      "register_user_effects": run_register_user_effects();
      "structured_logging": run_structured_logging();
      "structured_log_history": run_structured_log_history();
      "mixed_logging": run_mixed_logging();
      default: $fatal(1, "unknown AUTHORING_CORE_KERNEL=%s", kernel);
    endcase

    if (kernel != "timing_phases" &&
        kernel != "timing_phases_deferred" && kernel != "apb_component" &&
        kernel != "transaction_recording" &&
        kernel != "memory_model" && kernel != "memory_model_direct" &&
        kernel != "register_prediction_validity" &&
        kernel != "register_backdoor" &&
        kernel != "register_hierarchy" &&
        kernel != "register_split" &&
        kernel != "register_wide" &&
        kernel != "register_enum" &&
        kernel != "register_memory" &&
        kernel != "register_sequences" &&
        kernel != "register_coverage" &&
        kernel != "register_maps" &&
        kernel != "register_user_effects" &&
        kernel != "structured_logging" &&
        kernel != "structured_log_history" &&
        kernel != "test_lifecycle" &&
        kernel != "dynamic_spawn" && kernel != "dynamic_task" &&
        kernel != "dynamic_spawn_scheduler" &&
        kernel != "dynamic_spawn_suspending") begin
      while (response_count != iterations) begin
        @(posedge clk);
        #1ps;
      end
      check32(request_count, iterations, "request count");
      check32(response_count, iterations, "response count");
    end
    if (kernel != "test_lifecycle" && kernel != "dynamic_spawn" &&
        kernel != "dynamic_task" &&
        kernel != "dynamic_spawn_scheduler" &&
        kernel != "dynamic_spawn_suspending") begin
      $display("AUTHORING_CORE_RESULT mode=pure_sv kernel=%s iterations=%0d transactions=%0d checks=%0d sim_cycles=%0d spawned_processes=%0d checksum=%0d failures=%0d task_value=%0d clock_cycles=%0d timeouts=%0d timeout_hits=%0d task_timeouts=%0d task_timeout_hits=%0d wait_until=%0d event_set=%0d event_wait=%0d queue_send=%0d queue_receive=%0d queue_put=%0d queue_get=%0d lock_acquire=%0d semaphore_acquire=%0d wide64=%0d wide_echo_137=%0d wide_slice=%0d fixed_mac=%0d array_index=%0d array_wide=%0d array_multidim=%0d mem_rw=%0d hier_probe_reads=%0d hier_probe_deposits=%0d mem_backdoor_reads=%0d mem_backdoor_deposits=%0d probe_diag_reads=%0d probe_diag_deposits=%0d signal_edges=%0d force_release=%0d packed_view=%0d hier_data_reads=%0d hier_data_deposits=%0d timing_phases=%0d test_lifecycle=%0d dynamic_spawn=%0d analysis_write=%0d analysis_delivery=%0d random_stimulus=%0d constrained_packet=%0d constraint_extensions=%0d coverage_sampling=%0d apb_component=%0d memory_model=%0d memory_model_direct=%0d register_prediction_validity=%0d register_backdoor=%0d register_hierarchy=%0d register_split=%0d register_wide=%0d register_enum=%0d",
             kernel, iterations, transactions, checks, sim_cycles,
             spawned_processes, checksum,
             failures, task_value_count, clock_cycles_count, timeout_count,
             timeout_hits, task_timeout_count, task_timeout_hits,
             wait_until_count, event_set_count, event_wait_count,
             queue_send_count, queue_receive_count, queue_put_count,
             queue_get_count, lock_acquire_count, semaphore_acquire_count,
             wide64_count,
             wide_echo_137_count, wide_slice_count, fixed_mac_count,
             array_index_count, array_wide_count, array_multidim_count,
             mem_rw_count,
             hier_probe_reads, hier_probe_deposits, mem_backdoor_reads,
             mem_backdoor_deposits, probe_diag_reads, probe_diag_deposits,
             signal_edges, force_release_count, packed_view_count,
             hier_data_reads, hier_data_deposits, timing_phases_count,
             test_lifecycle_count, dynamic_spawn_count,
             analysis_write_count, analysis_delivery_count,
             random_stimulus_count, constrained_packet_count,
             constraint_extensions_count, coverage_sampling_count,
             apb_component_count, memory_model_count,
             memory_model_direct_count, register_prediction_validity_count,
             register_backdoor_count, register_hierarchy_count,
             register_split_count, register_wide_count, register_enum_count);
      $finish;
    end
  end

  authoring_core_dut i_dut (.*);
endmodule
