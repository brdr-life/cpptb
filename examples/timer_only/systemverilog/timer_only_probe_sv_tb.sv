module timer_only_probe_sv_tb;
  timeunit 1ps;
  timeprecision 1ps;

  logic [31:0] fast_value;
  logic [31:0] fast_echo;
  logic [31:0] slow_value;
  logic [31:0] slow_echo;

  int unsigned iterations;
  longint unsigned checks;
  int unsigned failures;
  longint unsigned sim_cycles;

  task automatic expect_eq(
      input string label,
      input logic [63:0] actual,
      input logic [63:0] expected
  );
    checks++;
    if (actual !== expected) begin
      failures++;
      $error("%s: expected 0x%0h, got 0x%0h", label, expected, actual);
    end
  endtask

  task automatic fast_cadence();
    logic [31:0] value;
    longint unsigned expected_time_ps;

    for (int unsigned index = 0; index < iterations; index++) begin
      if (index == 0) #7ns;
      else #6999ps;
      value = 32'h1000 + index * 17;
      fast_value = value;
      #1ps;

      expected_time_ps = longint'(index + 1) * 7000 + 1;
      expect_eq("fast cadence exact settle time", $time, expected_time_ps);
      expect_eq("fast cadence settled echo", fast_echo,
                value ^ 32'h1357_9bdf);
    end
  endtask

  task automatic slow_cadence();
    logic [31:0] value;
    longint unsigned expected_time_ps;

    for (int unsigned index = 0; index < iterations; index++) begin
      if (index == 0) #11ns;
      else #10999ps;
      value = 32'h2000 + index * 29;
      slow_value = value;
      #1ps;

      expected_time_ps = longint'(index + 1) * 11000 + 1;
      expect_eq("slow cadence exact settle time", $time, expected_time_ps);
      expect_eq("slow cadence settled echo", slow_echo,
                value + 32'h0102_0304);
    end
  endtask

  initial begin
    int unsigned last;

    fast_value = '0;
    slow_value = '0;
    checks = 0;
    failures = 0;
    sim_cycles = 0;
    iterations = 9;
    void'($value$plusargs("CPPTB_TIMER_ONLY_ITERS=%d", iterations));

    fork
      fast_cadence();
      slow_cadence();
    join

    last = iterations - 1;
    expect_eq("timer-only final absolute time", $time,
              longint'(iterations) * 11000 + 1);
    expect_eq("timer-only final fast value", fast_echo,
              (32'h1000 + last * 17) ^ 32'h1357_9bdf);
    expect_eq("timer-only final slow value", slow_echo,
              (32'h2000 + last * 29) + 32'h0102_0304);

    $display(
        "PURE_SV_TIMER_ONLY_RESULT iterations=%0d checks=%0d sim_cycles=%0d failures=%0d",
        iterations, checks, sim_cycles, failures);
    if (failures != 0) begin
      $fatal(1, "timer_only_probe pure-SV testbench failed");
    end
    $finish;
  end

  initial begin
    #1ms;
    $fatal(1, "timer_only_probe pure-SV testbench timed out");
  end

  timer_only_probe i_dut (
      .fast_value(fast_value),
      .fast_echo(fast_echo),
      .slow_value(slow_value),
      .slow_echo(slow_echo)
  );
endmodule : timer_only_probe_sv_tb
