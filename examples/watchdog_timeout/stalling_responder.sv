module stalling_responder (
    input  logic        clk,
    input  logic        rst_n,
    input  logic        request,
    input  logic        stall,
    input  logic [7:0]  latency,
    input  logic [31:0] request_data,
    output logic        response_valid,
    output logic [31:0] response_data,
    output logic        busy
);
  timeunit 1ns;
  timeprecision 1ps;

  logic [7:0] countdown;
  logic [31:0] pending_data;
  logic stalled_request;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      response_valid <= 1'b0;
      response_data <= 32'd0;
      busy <= 1'b0;
      countdown <= 8'd0;
      pending_data <= 32'd0;
      stalled_request <= 1'b0;
    end else begin
      response_valid <= 1'b0;

      if (request && !busy) begin
        busy <= 1'b1;
        countdown <= latency;
        pending_data <= request_data;
        stalled_request <= stall;
      end else if (busy && !stalled_request) begin
        if (countdown <= 8'd1) begin
          response_valid <= 1'b1;
          response_data <= pending_data ^ 32'ha5a5_5a5a;
          busy <= 1'b0;
        end else begin
          countdown <= countdown - 8'd1;
        end
      end
    end
  end
endmodule
