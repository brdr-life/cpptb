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
  longint unsigned apb_compared;
  longint unsigned apb_checker_violations;
  bit apb_done;
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
    apb_actual.push_back(transaction);
    apb_compare_available();
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
    apb_publish_expected('{1'b1, address, data, strobe, 1'b0,
                           wait_cycles});
    transactions++;
    apb_component_count++;
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
    apb_publish_expected('{1'b0, address, data, 4'hf, 1'b0,
                           wait_cycles});
    transactions++;
    apb_component_count++;
    @(negedge clk);
    apb_psel_i = 1'b0;
    apb_penable_i = 1'b0;
    apb_pwrite_i = 1'b0;
  endtask

  task automatic run_apb_monitor();
    int unsigned completed;
    int unsigned wait_cycles;
    bit active;
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
        wait_cycles = 0;
      end else if (!apb_pready_o) begin
        if (active) wait_cycles++;
      end else begin
        apb_publish_actual('{apb_pwrite_i, apb_paddr_i,
                            apb_pwrite_i ? apb_pwdata_i : apb_prdata_o,
                            apb_pwrite_i ? apb_pstrb_i : 4'hf,
                            apb_pslverr_o, wait_cycles});
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
    $display("AUTHORING_CORE_RESULT mode=pure_sv kernel=test_lifecycle iterations=%0d transactions=0 checks=%0d sim_cycles=0 spawned_processes=%0d checksum=2166136261 failures=%0d task_value=0 clock_cycles=0 timeouts=0 timeout_hits=0 task_timeouts=0 task_timeout_hits=0 wait_until=0 event_set=0 event_wait=0 queue_send=0 queue_receive=0 queue_put=0 queue_get=0 lock_acquire=0 semaphore_acquire=0 wide64=0 wide_echo_137=0 wide_slice=0 fixed_mac=0 array_index=0 array_wide=0 array_multidim=0 mem_rw=0 hier_probe_reads=0 hier_probe_deposits=0 mem_backdoor_reads=0 mem_backdoor_deposits=0 probe_diag_reads=0 probe_diag_deposits=0 signal_edges=0 force_release=0 packed_view=0 hier_data_reads=0 hier_data_deposits=0 timing_phases=0 test_lifecycle=%0d dynamic_spawn=0 analysis_write=0 analysis_delivery=0 random_stimulus=0 constrained_packet=0 constraint_extensions=0 coverage_sampling=0 apb_component=0",
             iterations, checks, spawned_processes, failures,
             test_lifecycle_count);
    $finish;
  endtask

  task automatic dynamic_spawn_child(input logic [31:0] value,
                                     input int unsigned iteration);
    check32(value, stimulus(iteration), "dynamic process value");
  endtask

  task automatic report_dynamic_process();
    #1ps;
    $display("AUTHORING_CORE_RESULT mode=pure_sv kernel=%s iterations=%0d transactions=0 checks=%0d sim_cycles=0 spawned_processes=%0d checksum=2166136261 failures=%0d task_value=0 clock_cycles=0 timeouts=0 timeout_hits=0 task_timeouts=0 task_timeout_hits=0 wait_until=0 event_set=0 event_wait=0 queue_send=0 queue_receive=0 queue_put=0 queue_get=0 lock_acquire=0 semaphore_acquire=0 wide64=0 wide_echo_137=0 wide_slice=0 fixed_mac=0 array_index=0 array_wide=0 array_multidim=0 mem_rw=0 hier_probe_reads=0 hier_probe_deposits=0 mem_backdoor_reads=0 mem_backdoor_deposits=0 probe_diag_reads=0 probe_diag_deposits=0 signal_edges=0 force_release=0 packed_view=0 hier_data_reads=0 hier_data_deposits=0 timing_phases=0 test_lifecycle=0 dynamic_spawn=%0d analysis_write=0 analysis_delivery=0 random_stimulus=0 constrained_packet=0 constraint_extensions=0 coverage_sampling=0 apb_component=0",
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
    apb_compared = 0;
    apb_checker_violations = 0;
    apb_done = 1'b0;
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
    if (kernel != "test_lifecycle" && kernel != "dynamic_spawn" &&
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
      "test_lifecycle": run_test_lifecycle();
      "dynamic_spawn": run_dynamic_spawn();
      "dynamic_task": run_dynamic_task();
      "dynamic_spawn_scheduler": run_dynamic_spawn_scheduler();
      "dynamic_spawn_suspending": run_dynamic_spawn_suspending();
      "dynamic_monitor": run_dynamic_monitor();
      "analysis_fanout": run_analysis_fanout();
      "random_stimulus": run_random_stimulus();
      "constrained_packet": run_constrained_packet();
      "constraint_extensions": run_constraint_extensions();
      "coverage_sampling": run_coverage_sampling();
      "apb_component": run_apb_component();
      default: $fatal(1, "unknown AUTHORING_CORE_KERNEL=%s", kernel);
    endcase

    if (kernel != "timing_phases" && kernel != "apb_component" &&
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
      $display("AUTHORING_CORE_RESULT mode=pure_sv kernel=%s iterations=%0d transactions=%0d checks=%0d sim_cycles=%0d spawned_processes=%0d checksum=%0d failures=%0d task_value=%0d clock_cycles=%0d timeouts=%0d timeout_hits=%0d task_timeouts=%0d task_timeout_hits=%0d wait_until=%0d event_set=%0d event_wait=%0d queue_send=%0d queue_receive=%0d queue_put=%0d queue_get=%0d lock_acquire=%0d semaphore_acquire=%0d wide64=%0d wide_echo_137=%0d wide_slice=%0d fixed_mac=%0d array_index=%0d array_wide=%0d array_multidim=%0d mem_rw=%0d hier_probe_reads=%0d hier_probe_deposits=%0d mem_backdoor_reads=%0d mem_backdoor_deposits=%0d probe_diag_reads=%0d probe_diag_deposits=%0d signal_edges=%0d force_release=%0d packed_view=%0d hier_data_reads=%0d hier_data_deposits=%0d timing_phases=%0d test_lifecycle=%0d dynamic_spawn=%0d analysis_write=%0d analysis_delivery=%0d random_stimulus=%0d constrained_packet=%0d constraint_extensions=%0d coverage_sampling=%0d apb_component=%0d",
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
             apb_component_count);
      $finish;
    end
  end

  authoring_core_dut i_dut (.*);
endmodule
