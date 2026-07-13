module timer_only_probe (
    input  logic [31:0] fast_value,
    output logic [31:0] fast_echo,
    input  logic [31:0] slow_value,
    output logic [31:0] slow_echo
);
  assign fast_echo = fast_value ^ 32'h1357_9bdf;
  assign slow_echo = slow_value + 32'h0102_0304;
endmodule
