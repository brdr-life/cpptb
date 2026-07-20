module timer_only_deadlock_sv_tb;
  timeunit 1ps;
  timeprecision 1ps;

  event response_ready;

  initial begin
    @response_ready;
  end

  final begin
    $fatal(1,
           "pure-SV deadlock: waiting for Event timer_only.response_ready");
  end
endmodule : timer_only_deadlock_sv_tb
