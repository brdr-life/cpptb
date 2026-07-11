module apb_i2c_peakrdl #(
    parameter APB_ADDR_WIDTH = 12
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
    output logic                      interrupt_o,
    input  logic                      scl_pad_i,
    output logic                      scl_pad_o,
    output logic                      scl_padoen_o,
    input  logic                      sda_pad_i,
    output logic                      sda_pad_o,
    output logic                      sda_padoen_o
);

  import apb_i2c_regs_core_pkg::*;

  localparam logic [3:0] REG_CLK_PRESCALER = 4'b0000;
  localparam logic [3:0] REG_CTRL          = 4'b0001;
  localparam logic [3:0] REG_RX            = 4'b0010;
  localparam logic [3:0] REG_STATUS        = 4'b0011;
  localparam logic [3:0] REG_TX            = 4'b0100;
  localparam logic [3:0] REG_CMD           = 4'b0101;

  apb_i2c__in_t hwif_in;
  apb_i2c__out_t hwif_out;

  logic [3:0] apb_addr;
  logic apb_write;
  logic [31:0] pre_next;
  logic [31:0] ctrl_next;
  logic [31:0] tx_next;
  logic [31:0] cmd_next;

  logic [15:0] pre_q;
  logic [7:0] ctrl_q;
  logic [7:0] tx_q;
  logic [7:0] cmd_q;
  logic [7:0] rx_data;
  logic [7:0] status_data;

  logic done;
  logic core_en;
  logic ien;
  logic irxack;
  logic rxack;
  logic tip;
  logic irq_flag;
  logic i2c_busy;
  logic i2c_al;
  logic al;

  logic sta;
  logic sto;
  logic rd;
  logic wr;
  logic ack;
  logic iack;

  logic unused_core_req_stall_wr;
  logic unused_core_req_stall_rd;
  logic unused_core_rd_ack;
  logic unused_core_rd_err;
  logic [31:0] unused_core_rd_data;
  logic unused_core_wr_ack;
  logic unused_core_wr_err;

  assign apb_addr = PADDR[5:2];
  assign apb_write = PSEL & PENABLE & PWRITE;

  assign pre_q = hwif_out.clk_prescaler.value.value[15:0];
  assign ctrl_q = hwif_out.ctrl.value.value[7:0];
  assign tx_q = hwif_out.tx.value.value[7:0];
  assign cmd_q = hwif_out.cmd.value.value[7:0];

  assign sta = cmd_q[7];
  assign sto = cmd_q[6];
  assign rd = cmd_q[5];
  assign wr = cmd_q[4];
  assign ack = cmd_q[3];
  assign iack = cmd_q[0];
  assign core_en = ctrl_q[7];
  assign ien = ctrl_q[6];

  assign status_data[7] = rxack;
  assign status_data[6] = i2c_busy;
  assign status_data[5] = al;
  assign status_data[4:2] = 3'h0;
  assign status_data[1] = tip;
  assign status_data[0] = irq_flag;

  assign PREADY = 1'b1;
  assign PSLVERR = 1'b0;

  i2c_master_byte_ctrl byte_controller (
      .clk(HCLK),
      .nReset(HRESETn),
      .ena(core_en),
      .clk_cnt(pre_q),
      .start(sta),
      .stop(sto),
      .read(rd),
      .write(wr),
      .ack_in(ack),
      .din(tx_q),
      .cmd_ack(done),
      .ack_out(irxack),
      .dout(rx_data),
      .i2c_busy(i2c_busy),
      .i2c_al(i2c_al),
      .scl_i(scl_pad_i),
      .scl_o(scl_pad_o),
      .scl_oen(scl_padoen_o),
      .sda_i(sda_pad_i),
      .sda_o(sda_pad_o),
      .sda_oen(sda_padoen_o)
  );

  always_comb begin
    pre_next = hwif_out.clk_prescaler.value.value;
    ctrl_next = hwif_out.ctrl.value.value;
    tx_next = hwif_out.tx.value.value;
    cmd_next = hwif_out.cmd.value.value;

    if (apb_write) begin
      if (done | i2c_al) begin
        cmd_next[7:4] = 4'h0;
      end
      cmd_next[2:1] = 2'b0;
      cmd_next[0] = 1'b0;

      unique case (apb_addr)
        REG_CLK_PRESCALER: pre_next = {16'h0000, PWDATA[15:0]};
        REG_CTRL: ctrl_next = {24'h00_0000, PWDATA[7:0]};
        REG_TX: tx_next = {24'h00_0000, PWDATA[7:0]};
        REG_CMD: begin
          if (core_en) begin
            cmd_next = {24'h00_0000, PWDATA[7:0]};
          end
        end
        default: ;
      endcase
    end else begin
      if (done | i2c_al) begin
        cmd_next[7:4] = 4'h0;
      end
      cmd_next[2:1] = 2'b0;
      cmd_next[0] = 1'b0;
    end
  end

  always_ff @(posedge HCLK or negedge HRESETn) begin
    if (!HRESETn) begin
      al <= 1'b0;
      rxack <= 1'b0;
      tip <= 1'b0;
      irq_flag <= 1'b0;
    end else begin
      al <= i2c_al | (al & ~sta);
      rxack <= irxack;
      tip <= (rd | wr);
      irq_flag <= (done | i2c_al | irq_flag) & ~iack;
    end
  end

  always_ff @(posedge HCLK or negedge HRESETn) begin
    if (!HRESETn) begin
      interrupt_o <= 1'b0;
    end else begin
      interrupt_o <= irq_flag && ien;
    end
  end

  always_comb begin
    hwif_in.clk_prescaler.value.next = pre_next;
    hwif_in.clk_prescaler.value.we = apb_write && (apb_addr == REG_CLK_PRESCALER);
    hwif_in.ctrl.value.next = ctrl_next;
    hwif_in.ctrl.value.we = apb_write && (apb_addr == REG_CTRL);
    hwif_in.rx.value.next = {24'h00_0000, rx_data};
    hwif_in.rx.value.we = 1'b1;
    hwif_in.status.value.next = {24'h00_0000, status_data};
    hwif_in.status.value.we = 1'b1;
    hwif_in.tx.value.next = tx_next;
    hwif_in.tx.value.we = apb_write && (apb_addr == REG_TX);
    hwif_in.cmd.value.next = cmd_next;
    hwif_in.cmd.value.we = 1'b1;
  end

  always_comb begin
    unique case (apb_addr)
      REG_CLK_PRESCALER: PRDATA = {16'h0000, pre_q};
      REG_CTRL:          PRDATA = {24'h00_0000, ctrl_q};
      REG_RX:            PRDATA = {24'h00_0000, rx_data};
      REG_STATUS:        PRDATA = {24'h00_0000, status_data};
      REG_TX:            PRDATA = {24'h00_0000, tx_q};
      REG_CMD:           PRDATA = {24'h00_0000, cmd_q};
      default:           PRDATA = '0;
    endcase
  end

  apb_i2c_regs_core i_regs (
      .clk(HCLK),
      .arst_n(HRESETn),
      .s_cpuif_req(1'b0),
      .s_cpuif_req_is_wr(1'b1),
      .s_cpuif_addr(PADDR[4:0]),
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
    sto,
    unused_core_req_stall_wr,
    unused_core_req_stall_rd,
    unused_core_rd_ack,
    unused_core_rd_err,
    unused_core_rd_data,
    unused_core_wr_ack,
    unused_core_wr_err,
    hwif_out.rx.value.value,
    hwif_out.status.value.value
  };

endmodule
