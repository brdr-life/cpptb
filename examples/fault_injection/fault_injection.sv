module fault_injection (
    input  logic        clk,
    input  logic        rst_n,
    input  logic [7:0]  source_i,
    input  logic [1:0]  memory_address,
    input  logic        memory_write,
    input  logic [15:0] memory_write_data,
    output logic [7:0]  resolved_o,
    output logic [7:0]  counter_o,
    output logic [15:0] memory_read_data
);
  logic [7:0] counter;
  logic [15:0] memory [0:3];
  wire [7:0] resolved_value = source_i ^ 8'h5a;

  assign resolved_o = resolved_value;
  assign counter_o = counter;
  always_comb begin
    case (memory_address)
      2'd0: memory_read_data = memory[0];
      2'd1: memory_read_data = memory[1];
      2'd2: memory_read_data = memory[2];
      default: memory_read_data = memory[3];
    endcase
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      counter <= '0;
      for (int index = 0; index < 4; index++) begin
        memory[index] <= '0;
      end
    end else begin
      counter <= counter + 1'b1;
      if (memory_write) begin
        memory[memory_address] <= memory_write_data;
      end
    end
  end
endmodule
