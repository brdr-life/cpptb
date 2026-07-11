module authoring_core_dut (
    input  logic        clk,
    input  logic        rst_n,
    input  logic        req_valid,
    input  logic [31:0] req_data,
    output logic        req_ready,
    output logic        rsp_valid,
    output logic [31:0] rsp_data,
    input  logic        rsp_ready,
    output logic        pulse,
    output logic [31:0] request_count,
    output logic [31:0] response_count
);
  logic pending;
  logic [1:0] delay_count;
  logic [31:0] pending_data;
  logic [31:0] cycle_count;

  assign req_ready = !pending && !rsp_valid;
  assign pulse = (cycle_count[1:0] == 2'b00);

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      pending <= 1'b0;
      delay_count <= '0;
      pending_data <= '0;
      rsp_valid <= 1'b0;
      rsp_data <= '0;
      cycle_count <= '0;
      request_count <= '0;
      response_count <= '0;
    end else begin
      cycle_count <= cycle_count + 1'b1;

      if (req_valid && req_ready) begin
        pending <= 1'b1;
        delay_count <= 2'd1;
        pending_data <= req_data;
        request_count <= request_count + 1'b1;
      end

      if (pending) begin
        if (delay_count != 0) begin
          delay_count <= delay_count - 1'b1;
        end else begin
          pending <= 1'b0;
          rsp_valid <= 1'b1;
          rsp_data <= (pending_data ^ 32'ha5a5_5a5a) + (request_count - 1'b1);
        end
      end

      if (rsp_valid && rsp_ready) begin
        rsp_valid <= 1'b0;
        response_count <= response_count + 1'b1;
      end
    end
  end
endmodule
