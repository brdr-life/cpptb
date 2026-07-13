module stream_fifo (
    input  logic        clk,
    input  logic        rst_n,
    input  logic        in_valid,
    output logic        in_ready,
    input  logic [31:0] in_data,
    output logic        out_valid,
    input  logic        out_ready,
    output logic [31:0] out_data
);
  timeunit 1ns;
  timeprecision 1ps;

  logic [31:0] storage [0:3];
  logic [1:0] read_pointer;
  logic [1:0] write_pointer;
  logic [2:0] count;

  logic push;
  logic pop;

  always_comb begin
    in_ready = count != 3'd4;
    out_valid = count != 3'd0;
    out_data = storage[read_pointer];
    push = in_valid && in_ready;
    pop = out_valid && out_ready;
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      read_pointer <= 2'd0;
      write_pointer <= 2'd0;
      count <= 3'd0;
    end else begin
      if (push) begin
        storage[write_pointer] <= in_data;
        write_pointer <= write_pointer + 2'd1;
      end

      if (pop) begin
        read_pointer <= read_pointer + 2'd1;
      end

      case ({push, pop})
        2'b10: count <= count + 3'd1;
        2'b01: count <= count - 3'd1;
        default: count <= count;
      endcase
    end
  end
endmodule
