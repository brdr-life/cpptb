module ipxact_regfile (
    input  logic         clk,
    input  logic         rst_n,
    input  logic         apb_select,
    input  logic         apb_enable,
    input  logic         apb_write,
    input  logic [8:0]   apb_address,
    input  logic [31:0]  apb_write_data,
    input  logic [3:0]   apb_strobe,
    output logic [31:0]  apb_read_data,
    output logic         apb_ready,
    output logic         apb_error
);
  timeunit 1ns;
  timeprecision 1ps;

  logic [31:0] control;
  logic [31:0] thresholds [0:3];
  logic [31:0] scratchpad [0:15];
  logic address_valid;
  integer index;

  always_comb begin
    apb_ready = 1'b1;
    address_valid = 1'b1;
    apb_read_data = 32'd0;

    if (apb_address == 9'h000) begin
      apb_read_data = control;
    end else if (apb_address == 9'h004) begin
      apb_read_data = {29'd0, control[2:1], control[0]};
    end else if ((apb_address >= 9'h020) &&
                 (apb_address <= 9'h02c) &&
                 (apb_address[1:0] == 2'b00)) begin
      apb_read_data = thresholds[apb_address[3:2]];
    end else if ((apb_address >= 9'h100) &&
                 (apb_address <= 9'h13c) &&
                 (apb_address[1:0] == 2'b00)) begin
      apb_read_data = scratchpad[apb_address[5:2]];
    end else begin
      address_valid = 1'b0;
    end

    apb_error = apb_select && apb_enable && !address_valid;
  end

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      control <= 32'd0;
      for (index = 0; index < 4; index = index + 1) begin
        thresholds[index] <= 32'h0000_0010;
      end
      for (index = 0; index < 16; index = index + 1) begin
        scratchpad[index] <= 32'd0;
      end
    end else if (apb_select && apb_enable && apb_write && apb_ready) begin
      if (apb_address == 9'h000) begin
        if (apb_strobe[0]) control[7:0] <= apb_write_data[7:0] & 8'h07;
      end else if ((apb_address >= 9'h020) &&
                   (apb_address <= 9'h02c) &&
                   (apb_address[1:0] == 2'b00)) begin
        if (apb_strobe[0])
          thresholds[apb_address[3:2]][7:0] <=
              apb_write_data[7:0];
        if (apb_strobe[1])
          thresholds[apb_address[3:2]][15:8] <=
              apb_write_data[15:8];
      end else if ((apb_address >= 9'h100) &&
                   (apb_address <= 9'h13c) &&
                   (apb_address[1:0] == 2'b00)) begin
        for (index = 0; index < 4; index = index + 1) begin
          if (apb_strobe[index])
            scratchpad[apb_address[5:2]][index * 8 +: 8] <=
                apb_write_data[index * 8 +: 8];
        end
      end
    end
  end
endmodule
