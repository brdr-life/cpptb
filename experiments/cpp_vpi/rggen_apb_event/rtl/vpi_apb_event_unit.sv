module vpi_apb_event_unit;
  localparam int APB_ADDR_WIDTH = 12;

  logic clk_i;
  logic HCLK;
  logic HRESETn;
  logic [APB_ADDR_WIDTH-1:0] PADDR;
  logic [31:0] PWDATA;
  logic PWRITE;
  logic PSEL;
  logic PENABLE;
  logic [31:0] PRDATA;
  logic PREADY;
  logic PSLVERR;
  logic [31:0] irq_i;
  logic [31:0] event_i;
  logic [31:0] irq_o;
  logic fetch_enable_i;
  logic fetch_enable_o;
  logic clk_gate_core_o;
  logic core_busy_i;

  apb_event_unit_peakrdl #(
      .APB_ADDR_WIDTH(APB_ADDR_WIDTH)
  ) dut (
      .clk_i,
      .HCLK,
      .HRESETn,
      .PADDR,
      .PWDATA,
      .PWRITE,
      .PSEL,
      .PENABLE,
      .PRDATA,
      .PREADY,
      .PSLVERR,
      .irq_i,
      .event_i,
      .irq_o,
      .fetch_enable_i,
      .fetch_enable_o,
      .clk_gate_core_o,
      .core_busy_i
  );

endmodule
