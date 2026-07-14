module dual_clock_mailbox (
    input  logic       rst_n,

    input  logic       write_clk,
    input  logic [7:0] write_data,
    input  logic       write_valid,
    output logic       write_ready,
    output logic [31:0] write_count,

    input  logic       read_clk,
    output logic [7:0] read_data,
    output logic       read_valid,
    input  logic       read_ready,
    output logic [31:0] read_count,

    input  logic [7:0] probe_in,
    output logic [7:0] probe_echo,
    output logic       output_clk
);
  logic [7:0] payload;
  logic request_toggle;
  logic acknowledge_toggle;
  logic acknowledge_sync_1;
  logic acknowledge_sync_2;
  logic request_sync_1;
  logic request_sync_2;

  assign write_ready = acknowledge_sync_2 == request_toggle;
    assign probe_echo = probe_in;

    always_ff @(posedge write_clk or negedge rst_n) begin
      if (!rst_n) begin
        output_clk <= 1'b0;
      end else begin
        output_clk <= ~output_clk;
      end
    end

  always_ff @(posedge write_clk or negedge rst_n) begin
    if (!rst_n) begin
      payload <= '0;
      request_toggle <= 1'b0;
      acknowledge_sync_1 <= 1'b0;
      acknowledge_sync_2 <= 1'b0;
      write_count <= '0;
    end else begin
      acknowledge_sync_1 <= acknowledge_toggle;
      acknowledge_sync_2 <= acknowledge_sync_1;
      if (write_valid && write_ready) begin
        payload <= write_data;
        request_toggle <= ~request_toggle;
        write_count <= write_count + 1'b1;
      end
    end
  end

  always_ff @(posedge read_clk or negedge rst_n) begin
    if (!rst_n) begin
      request_sync_1 <= 1'b0;
      request_sync_2 <= 1'b0;
      acknowledge_toggle <= 1'b0;
      read_data <= '0;
      read_valid <= 1'b0;
      read_count <= '0;
    end else begin
      request_sync_1 <= request_toggle;
      request_sync_2 <= request_sync_1;

      if (!read_valid && (request_sync_2 != acknowledge_toggle)) begin
        read_data <= payload;
        read_valid <= 1'b1;
      end

      if (read_valid && read_ready) begin
        read_valid <= 1'b0;
        acknowledge_toggle <= request_sync_2;
        read_count <= read_count + 1'b1;
      end
    end
  end
endmodule
