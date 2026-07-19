module aes_bus_observer (
  input logic        clk,
  input logic        reset_n,
  input logic        cs,
  input logic        we,
  input logic [7:0]  address,
  input logic [31:0] write_data,
  input logic [31:0] read_data
);
  longint unsigned cycle = 0;
  bit trace_enabled;

  initial trace_enabled = $test$plusargs("AES_BUS_TRACE");

  always @(posedge clk) begin
    cycle <= cycle + 1;
    if (trace_enabled && reset_n && cs) begin
      if (we) begin
        $display("AES_BUS cycle=%0d op=W address=%02x data=%08x",
                 cycle, address, write_data);
      end else begin
        $display("AES_BUS cycle=%0d op=R address=%02x data=%08x",
                 cycle, address, read_data);
      end
    end
  end
endmodule

bind aes aes_bus_observer i_aes_bus_observer (
  .clk(clk),
  .reset_n(reset_n),
  .cs(cs),
  .we(we),
  .address(address),
  .write_data(write_data),
  .read_data(read_data)
);
