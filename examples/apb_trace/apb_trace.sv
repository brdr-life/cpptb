module apb_trace (
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

  logic [31:0] memory [0:63];
  logic address_valid;
  logic [1:0] wait_phase;
  integer index;

  always_comb begin
    address_valid = apb_address[1:0] == 2'b00;
    apb_read_data = address_valid ? memory[apb_address[7:2]] : 32'd0;
    apb_ready = !(apb_select && apb_enable && apb_address[2]) ||
                wait_phase == 2;
    apb_error = apb_select && apb_enable && !address_valid;
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      wait_phase <= 0;
      for (index = 0; index < 64; index++) memory[index] <= 32'd0;
    end else begin
      if (!apb_select || !apb_enable)
        wait_phase <= 0;
      else if (apb_address[2] && wait_phase != 2)
        wait_phase <= wait_phase + 1'b1;

      if (apb_select && apb_enable && apb_write && address_valid &&
          (apb_ready || (apb_address[2] && wait_phase == 1)))
        memory[apb_address[7:2]] <= apb_write_data;
    end
  end
endmodule
