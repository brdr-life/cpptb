module apb_regfile (
    input  logic        clk,
    input  logic        rst_n,
    input  logic        apb_select,
    input  logic        apb_enable,
    input  logic        apb_write,
    input  logic [7:0]  apb_address,
    input  logic [31:0] apb_write_data,
    output logic [31:0] apb_read_data,
    output logic        apb_ready,
    output logic        apb_error
);
  timeunit 1ns;
  timeprecision 1ps;

  localparam logic [31:0] ID_VALUE = 32'h4350_5054;

  logic [31:0] registers [0:3];
  logic address_valid;

  always_comb begin
    apb_ready = 1'b1;
    address_valid = 1'b1;
    unique case (apb_address)
      8'h00: apb_read_data = registers[0];
      8'h04: apb_read_data = registers[1];
      8'h08: apb_read_data = registers[2];
      8'h0c: apb_read_data = registers[3];
      8'h10: apb_read_data = ID_VALUE;
      default: begin
        apb_read_data = 32'd0;
        address_valid = 1'b0;
      end
    endcase
    apb_error = apb_select && apb_enable && !address_valid;
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      registers[0] <= 32'd0;
      registers[1] <= 32'd0;
      registers[2] <= 32'd0;
      registers[3] <= 32'd0;
    end else if (apb_select && apb_enable && apb_write && apb_ready) begin
      unique case (apb_address)
        8'h00: registers[0] <= apb_write_data;
        8'h04: registers[1] <= apb_write_data;
        8'h08: registers[2] <= apb_write_data;
        8'h0c: registers[3] <= apb_write_data;
        default: begin
        end
      endcase
    end
  end
endmodule
