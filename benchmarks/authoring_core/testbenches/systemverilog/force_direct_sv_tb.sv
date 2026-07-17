module force_direct_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  int unsigned iterations;
  int unsigned checks;
  int unsigned failures;
  logic [31:0] force_source_i;
  logic [31:0] force_fanout_o;

  function automatic logic [31:0] stimulus(input int unsigned iteration);
    return ((iteration + 1) * 32'h1f12_3bb5) ^ 32'hc001_d00d;
  endfunction

  initial begin
    logic [31:0] value;
    iterations = 10000;
    checks = 0;
    failures = 0;
    force_source_i = '0;
    void'($value$plusargs("AUTHORING_CORE_ITERS=%d", iterations));

    for (int unsigned i = 0; i < iterations; i++) begin
      value = stimulus(i) ^ 32'ha5a5_5a5a;
      force i_dut.force_target = value;
      checks++;
      if (i_dut.force_target != value) failures++;
      release i_dut.force_target;
    end

    $display("AUTHORING_CORE_RESULT mode=pure_sv kernel=force_direct iterations=%0d transactions=0 checks=%0d sim_cycles=0 spawned_processes=0 checksum=2166136261 failures=%0d task_value=0 clock_cycles=0 timeouts=0 timeout_hits=0 task_timeouts=0 task_timeout_hits=0 wait_until=0 event_set=0 event_wait=0 queue_send=0 queue_receive=0 queue_put=0 queue_get=0 lock_acquire=0 semaphore_acquire=0 wide64=0 wide_echo_137=0 wide_slice=0 fixed_mac=0 array_index=0 array_wide=0 array_multidim=0 mem_rw=0 hier_probe_reads=0 hier_probe_deposits=0 mem_backdoor_reads=0 mem_backdoor_deposits=0 probe_diag_reads=0 probe_diag_deposits=0 signal_edges=0 force_release=%0d packed_view=0 hier_data_reads=0 hier_data_deposits=0 timing_phases=0 test_lifecycle=0 dynamic_spawn=0 analysis_write=0 analysis_delivery=0 random_stimulus=0 constrained_packet=0 constraint_extensions=0 coverage_sampling=0 apb_component=0",
             iterations, checks, failures, iterations);
    $finish;
  end

  authoring_core_dut i_dut (
      .force_source_i(force_source_i),
      .force_fanout_o(force_fanout_o)
  );
endmodule
