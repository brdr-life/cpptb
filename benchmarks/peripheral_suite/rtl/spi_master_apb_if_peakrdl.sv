`define log2_peakrdl_spi(VALUE) ((VALUE) < ( 1 ) ? 0 : (VALUE) < ( 2 ) ? 1 : (VALUE) < ( 4 ) ? 2 : (VALUE) < ( 8 ) ? 3 : (VALUE) < ( 16 )  ? 4 : (VALUE) < ( 32 )  ? 5 : (VALUE) < ( 64 )  ? 6 : (VALUE) < ( 128 ) ? 7 : (VALUE) < ( 256 ) ? 8 : (VALUE) < ( 512 ) ? 9 : (VALUE) < ( 1024 ) ? 10 : (VALUE) < ( 2048 ) ? 11 : (VALUE) < ( 4096 ) ? 12 : (VALUE) < ( 8192 ) ? 13 : (VALUE) < ( 16384 ) ? 14 : (VALUE) < ( 32768 ) ? 15 : (VALUE) < ( 65536 ) ? 16 : (VALUE) < ( 131072 ) ? 17 : (VALUE) < ( 262144 ) ? 18 : (VALUE) < ( 524288 ) ? 19 : (VALUE) < ( 1048576 ) ? 20 : (VALUE) < ( 1048576 * 2 ) ? 21 : (VALUE) < ( 1048576 * 4 ) ? 22 : (VALUE) < ( 1048576 * 8 ) ? 23 : (VALUE) < ( 1048576 * 16 ) ? 24 : 25)

module spi_master_apb_if_peakrdl #(
    parameter BUFFER_DEPTH = 10,
    parameter APB_ADDR_WIDTH = 12,
    parameter LOG_BUFFER_DEPTH = `log2_peakrdl_spi(BUFFER_DEPTH)
) (
    input  logic                      HCLK,
    input  logic                      HRESETn,
    input  logic [APB_ADDR_WIDTH-1:0] PADDR,
    input  logic               [31:0] PWDATA,
    input  logic                      PWRITE,
    input  logic                      PSEL,
    input  logic                      PENABLE,
    output logic               [31:0] PRDATA,
    output logic                      PREADY,
    output logic                      PSLVERR,

    output logic                [7:0] spi_clk_div,
    output logic                      spi_clk_div_valid,
    input  logic               [31:0] spi_status,
    output logic               [31:0] spi_addr,
    output logic                [5:0] spi_addr_len,
    output logic               [31:0] spi_cmd,
    output logic                [5:0] spi_cmd_len,
    output logic                [3:0] spi_csreg,
    output logic               [15:0] spi_data_len,
    output logic               [15:0] spi_dummy_rd,
    output logic               [15:0] spi_dummy_wr,
    output logic [LOG_BUFFER_DEPTH:0] spi_int_th_tx,
    output logic [LOG_BUFFER_DEPTH:0] spi_int_th_rx,
    output logic [LOG_BUFFER_DEPTH:0] spi_int_cnt_tx,
    output logic [LOG_BUFFER_DEPTH:0] spi_int_cnt_rx,
    output logic                      spi_int_en,
    output logic                      spi_int_cnt_en,
    output logic                      spi_int_rd_sta,
    output logic                      spi_swrst,
    output logic                      spi_rd,
    output logic                      spi_wr,
    output logic                      spi_qrd,
    output logic                      spi_qwr,
    output logic               [31:0] spi_data_tx,
    output logic                      spi_data_tx_valid,
    input  logic                      spi_data_tx_ready,
    input  logic               [31:0] spi_data_rx,
    input  logic                      spi_data_rx_valid,
    output logic                      spi_data_rx_ready
);

  import apb_spi_master_regs_core_pkg::*;

  localparam logic [3:0] REG_STATUS = 4'b0000;
  localparam logic [3:0] REG_CLKDIV = 4'b0001;
  localparam logic [3:0] REG_SPICMD = 4'b0010;
  localparam logic [3:0] REG_SPIADR = 4'b0011;
  localparam logic [3:0] REG_SPILEN = 4'b0100;
  localparam logic [3:0] REG_SPIDUM = 4'b0101;
  localparam logic [3:0] REG_TXFIFO = 4'b0110;
  localparam logic [3:0] REG_RXFIFO = 4'b1000;
  localparam logic [3:0] REG_INTCFG = 4'b1001;
  localparam logic [3:0] REG_INTSTA = 4'b1010;

  apb_spi_master__in_t hwif_in;
  apb_spi_master__out_t hwif_out;

  logic [3:0] apb_addr;
  logic apb_read;
  logic apb_write;
  logic [31:0] status_next;
  logic [31:0] clkdiv_next;
  logic [31:0] spicmd_next;
  logic [31:0] spiadr_next;
  logic [31:0] spilen_next;
  logic [31:0] spidum_next;
  logic [31:0] intcfg_next;

  logic unused_core_req_stall_wr;
  logic unused_core_req_stall_rd;
  logic unused_core_rd_ack;
  logic unused_core_rd_err;
  logic [31:0] unused_core_rd_data;
  logic unused_core_wr_ack;
  logic unused_core_wr_err;

  assign apb_addr = PADDR[5:2];
  assign apb_read = PSEL & PENABLE & ~PWRITE;
  assign apb_write = PSEL & PENABLE & PWRITE;

  assign PREADY = 1'b1;
  assign PSLVERR = 1'b0;

  assign spi_clk_div = hwif_out.clkdiv.value.value[7:0];
  assign spi_cmd = hwif_out.spicmd.value.value;
  assign spi_addr = hwif_out.spiadr.value.value;
  assign spi_cmd_len = hwif_out.spilen.value.value[5:0];
  assign spi_addr_len = hwif_out.spilen.value.value[13:8];
  assign spi_data_len = {hwif_out.spilen.value.value[31:24], hwif_out.spilen.value.value[23:16]};
  assign spi_dummy_rd = hwif_out.spidum.value.value[15:0];
  assign spi_dummy_wr = hwif_out.spidum.value.value[31:16];
  assign spi_csreg = hwif_out.status.value.value[11:8];
  assign spi_int_th_tx = hwif_out.intcfg.value.value[LOG_BUFFER_DEPTH:0];
  assign spi_int_th_rx = hwif_out.intcfg.value.value[8 + LOG_BUFFER_DEPTH:8];
  assign spi_int_cnt_tx = hwif_out.intcfg.value.value[16 + LOG_BUFFER_DEPTH:16];
  assign spi_int_cnt_rx = hwif_out.intcfg.value.value[24 + LOG_BUFFER_DEPTH:24];
  assign spi_int_cnt_en = hwif_out.intcfg.value.value[30];
  assign spi_int_en = hwif_out.intcfg.value.value[31];

  assign spi_int_rd_sta = apb_read && (apb_addr == REG_INTSTA);
  assign spi_data_tx = PWDATA;
  assign spi_data_tx_valid = apb_write && (apb_addr == REG_TXFIFO);
  assign spi_data_rx_ready = apb_read && (apb_addr == REG_RXFIFO);

  always_ff @(posedge HCLK or negedge HRESETn) begin
    if (!HRESETn) begin
      spi_swrst <= 1'b0;
      spi_rd <= 1'b0;
      spi_wr <= 1'b0;
      spi_qrd <= 1'b0;
      spi_qwr <= 1'b0;
      spi_clk_div_valid <= 1'b0;
    end else if (apb_write) begin
      spi_swrst <= 1'b0;
      spi_rd <= 1'b0;
      spi_wr <= 1'b0;
      spi_qrd <= 1'b0;
      spi_qwr <= 1'b0;
      spi_clk_div_valid <= 1'b0;
      if (apb_addr == REG_STATUS) begin
        spi_rd <= PWDATA[0];
        spi_wr <= PWDATA[1];
        spi_qrd <= PWDATA[2];
        spi_qwr <= PWDATA[3];
        spi_swrst <= PWDATA[4];
      end else if (apb_addr == REG_CLKDIV) begin
        spi_clk_div_valid <= 1'b1;
      end
    end else begin
      spi_swrst <= 1'b0;
      spi_rd <= 1'b0;
      spi_wr <= 1'b0;
      spi_qrd <= 1'b0;
      spi_qwr <= 1'b0;
      spi_clk_div_valid <= 1'b0;
    end
  end

  always_comb begin
    status_next = hwif_out.status.value.value;
    clkdiv_next = hwif_out.clkdiv.value.value;
    spicmd_next = hwif_out.spicmd.value.value;
    spiadr_next = hwif_out.spiadr.value.value;
    spilen_next = hwif_out.spilen.value.value;
    spidum_next = hwif_out.spidum.value.value;
    intcfg_next = hwif_out.intcfg.value.value;

    if (apb_write) begin
      unique case (apb_addr)
        REG_STATUS: status_next[11:8] = PWDATA[11:8];
        REG_CLKDIV: clkdiv_next = {24'h00_0000, PWDATA[7:0]};
        REG_SPICMD: spicmd_next = PWDATA;
        REG_SPIADR: spiadr_next = PWDATA;
        REG_SPILEN: begin
          spilen_next = '0;
          spilen_next[5:0] = PWDATA[5:0];
          spilen_next[13:8] = PWDATA[13:8];
          spilen_next[23:16] = PWDATA[23:16];
          spilen_next[31:24] = PWDATA[31:24];
        end
        REG_SPIDUM: spidum_next = PWDATA;
        REG_INTCFG: begin
          intcfg_next = '0;
          intcfg_next[LOG_BUFFER_DEPTH:0] = PWDATA[LOG_BUFFER_DEPTH:0];
          intcfg_next[8 + LOG_BUFFER_DEPTH:8] = PWDATA[8 + LOG_BUFFER_DEPTH:8];
          intcfg_next[16 + LOG_BUFFER_DEPTH:16] = PWDATA[16 + LOG_BUFFER_DEPTH:16];
          intcfg_next[24 + LOG_BUFFER_DEPTH:24] = PWDATA[24 + LOG_BUFFER_DEPTH:24];
          intcfg_next[30] = PWDATA[30];
          intcfg_next[31] = PWDATA[31];
        end
        default: ;
      endcase
    end
  end

  always_comb begin
    hwif_in.status.value.next = status_next;
    hwif_in.status.value.we = apb_write && (apb_addr == REG_STATUS);
    hwif_in.clkdiv.value.next = clkdiv_next;
    hwif_in.clkdiv.value.we = apb_write && (apb_addr == REG_CLKDIV);
    hwif_in.spicmd.value.next = spicmd_next;
    hwif_in.spicmd.value.we = apb_write && (apb_addr == REG_SPICMD);
    hwif_in.spiadr.value.next = spiadr_next;
    hwif_in.spiadr.value.we = apb_write && (apb_addr == REG_SPIADR);
    hwif_in.spilen.value.next = spilen_next;
    hwif_in.spilen.value.we = apb_write && (apb_addr == REG_SPILEN);
    hwif_in.spidum.value.next = spidum_next;
    hwif_in.spidum.value.we = apb_write && (apb_addr == REG_SPIDUM);
    hwif_in.rxfifo.value.next = spi_data_rx;
    hwif_in.rxfifo.value.we = 1'b1;
    hwif_in.intcfg.value.next = intcfg_next;
    hwif_in.intcfg.value.we = apb_write && (apb_addr == REG_INTCFG);
    hwif_in.intsta.value.next = '0;
    hwif_in.intsta.value.we = 1'b1;
  end

  always_comb begin
    unique case (apb_addr)
      REG_STATUS: PRDATA = spi_status;
      REG_CLKDIV: PRDATA = {24'h00_0000, hwif_out.clkdiv.value.value[7:0]};
      REG_SPICMD: PRDATA = hwif_out.spicmd.value.value;
      REG_SPIADR: PRDATA = hwif_out.spiadr.value.value;
      REG_SPILEN: PRDATA = {spi_data_len, 2'b00, spi_addr_len, 2'b00, spi_cmd_len};
      REG_SPIDUM: PRDATA = {spi_dummy_wr, spi_dummy_rd};
      REG_RXFIFO: PRDATA = spi_data_rx;
      REG_INTCFG: begin
        PRDATA = '0;
        PRDATA[LOG_BUFFER_DEPTH:0] = spi_int_th_tx;
        PRDATA[8 + LOG_BUFFER_DEPTH:8] = spi_int_th_rx;
        PRDATA[16 + LOG_BUFFER_DEPTH:16] = spi_int_cnt_tx;
        PRDATA[24 + LOG_BUFFER_DEPTH:24] = spi_int_cnt_rx;
        PRDATA[30] = spi_int_cnt_en;
        PRDATA[31] = spi_int_en;
      end
      default: PRDATA = '0;
    endcase
  end

  apb_spi_master_regs_core i_regs (
      .clk(HCLK),
      .arst_n(HRESETn),
      .s_cpuif_req(1'b0),
      .s_cpuif_req_is_wr(1'b1),
      .s_cpuif_addr(PADDR[5:0]),
      .s_cpuif_wr_data(PWDATA),
      .s_cpuif_wr_biten(32'hffff_ffff),
      .s_cpuif_req_stall_wr(unused_core_req_stall_wr),
      .s_cpuif_req_stall_rd(unused_core_req_stall_rd),
      .s_cpuif_rd_ack(unused_core_rd_ack),
      .s_cpuif_rd_err(unused_core_rd_err),
      .s_cpuif_rd_data(unused_core_rd_data),
      .s_cpuif_wr_ack(unused_core_wr_ack),
      .s_cpuif_wr_err(unused_core_wr_err),
      .hwif_in(hwif_in),
      .hwif_out(hwif_out)
  );

  logic unused_signals;
  assign unused_signals = &{
    spi_data_tx_ready,
    spi_data_rx_valid,
    unused_core_req_stall_wr,
    unused_core_req_stall_rd,
    unused_core_rd_ack,
    unused_core_rd_err,
    unused_core_rd_data,
    unused_core_wr_ack,
    unused_core_wr_err,
    hwif_out.txfifo.value.value,
    hwif_out.txfifo.value.wr_swacc,
    hwif_out.rxfifo.value.value,
    hwif_out.intsta.value.value
  };

endmodule
