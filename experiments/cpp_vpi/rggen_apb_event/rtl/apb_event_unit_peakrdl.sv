module generic_service_unit_peakrdl #(
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

    input  logic               [31:0] signal_i,
    output logic               [31:0] irq_o
);

  import apb_event_service_unit_regs_core_pkg::*;

  localparam logic [1:0] REG_ENABLE        = 2'b00;
  localparam logic [1:0] REG_PENDING       = 2'b01;
  localparam logic [1:0] REG_SET_PENDING   = 2'b10;
  localparam logic [1:0] REG_CLEAR_PENDING = 2'b11;

  apb_event_service_unit__in_t hwif_in;
  apb_event_service_unit__out_t hwif_out;

  logic [1:0] register_adr;
  logic apb_write;
  logic apb_read;
  logic [31:0] enable_q;
  logic [31:0] pending_q;
  logic [31:0] set_pending_q;
  logic [31:0] clear_pending_q;
  logic [31:0] pending_int;
  logic [31:0] pending_next;
  logic [31:0] enable_next;
  logic [31:0] set_pending_next;
  logic [31:0] clear_pending_next;
  logic [4:0] highest_pending_int;
  logic [31:0] irq_n;

  logic unused_core_req_stall_wr;
  logic unused_core_req_stall_rd;
  logic unused_core_rd_ack;
  logic unused_core_rd_err;
  logic [31:0] unused_core_rd_data;
  logic unused_core_wr_ack;
  logic unused_core_wr_err;

  assign register_adr = PADDR[3:2];
  assign apb_write = PSEL & PENABLE & PWRITE;
  assign apb_read = PSEL & PENABLE & ~PWRITE;
  assign enable_q = hwif_out.enable.value.value;
  assign pending_q = hwif_out.pending.value.value;
  assign set_pending_q = hwif_out.set_pending.value.value;
  assign clear_pending_q = hwif_out.clear_pending.value.value;
  assign PREADY = 1'b1;
  assign PSLVERR = 1'b0;

  always_comb begin
    highest_pending_int = '0;
    irq_n = '0;
    for (int unsigned i = 0; i < 32; i++) begin
      if (pending_q[i]) begin
        highest_pending_int = i[4:0];
        break;
      end
    end
    if (pending_q != '0) begin
      irq_n[highest_pending_int] = 1'b1;
    end
  end

  always_comb begin
    enable_next = enable_q;
    set_pending_next = '0;
    clear_pending_next = '0;
    pending_int = ((enable_q & signal_i) | pending_q);
    pending_int = pending_int | set_pending_q;

    for (int unsigned i = 0; i < 32; i++) begin
      if (clear_pending_q[i]) begin
        pending_int[i] = 1'b0;
      end
    end

    if (apb_write) begin
      unique case (register_adr)
        REG_ENABLE: enable_next = PWDATA;
        REG_PENDING: pending_int = PWDATA;
        REG_SET_PENDING: set_pending_next = PWDATA;
        REG_CLEAR_PENDING: clear_pending_next = PWDATA;
        default: ;
      endcase
    end

    pending_next = pending_int;
  end

  always_comb begin
    PRDATA = '0;
    if (apb_read) begin
      unique case (register_adr)
        REG_ENABLE: PRDATA = enable_q;
        REG_PENDING: PRDATA = pending_q;
        default: PRDATA = '0;
      endcase
    end
  end

  always_ff @(posedge HCLK or negedge HRESETn) begin
    if (!HRESETn) begin
      irq_o <= '0;
    end else begin
      irq_o <= irq_n;
    end
  end

  always_comb begin
    hwif_in.enable.value.next = enable_next;
    hwif_in.enable.value.we = apb_write && (register_adr == REG_ENABLE);
    hwif_in.pending.value.next = pending_next;
    hwif_in.pending.value.we = 1'b1;
    hwif_in.set_pending.value.next = set_pending_next;
    hwif_in.set_pending.value.we = 1'b1;
    hwif_in.clear_pending.value.next = clear_pending_next;
    hwif_in.clear_pending.value.we = 1'b1;
  end

  apb_event_service_unit_regs_core i_regs (
      .clk(HCLK),
      .arst_n(HRESETn),
      .s_cpuif_req(1'b0),
      .s_cpuif_req_is_wr(1'b1),
      .s_cpuif_addr(PADDR[3:0]),
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
    unused_core_req_stall_wr,
    unused_core_req_stall_rd,
    unused_core_rd_ack,
    unused_core_rd_err,
    unused_core_rd_data,
    unused_core_wr_ack,
    unused_core_wr_err,
    hwif_out.set_pending.value.wr_swacc,
    hwif_out.clear_pending.value.wr_swacc
  };

endmodule

module sleep_unit_peakrdl #(
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

    input  logic                      irq_i,
    input  logic                      event_i,
    input  logic                      core_busy_i,
    output logic                      fetch_en_o,
    output logic                      clk_gate_core_o
);

  import apb_event_sleep_unit_regs_core_pkg::*;

  typedef enum logic [1:0] {RUN, SHUTDOWN, SLEEP} sleep_state_e;

  localparam logic REG_SLEEP_CTRL = 1'b0;
  localparam logic REG_SLEEP_STATUS = 1'b1;

  apb_event_sleep_unit__in_t hwif_in;
  apb_event_sleep_unit__out_t hwif_out;

  sleep_state_e sleep_state_q;
  sleep_state_e sleep_state_n;
  logic register_adr;
  logic apb_write;
  logic apb_read;
  logic core_sleeping_int;
  logic [31:0] sleep_ctrl_q;
  logic [31:0] sleep_status_q;
  logic [31:0] sleep_ctrl_next;
  logic [31:0] sleep_status_next;

  logic unused_core_req_stall_wr;
  logic unused_core_req_stall_rd;
  logic unused_core_rd_ack;
  logic unused_core_rd_err;
  logic [31:0] unused_core_rd_data;
  logic unused_core_wr_ack;
  logic unused_core_wr_err;

  assign register_adr = PADDR[2];
  assign apb_write = PSEL & PENABLE & PWRITE;
  assign apb_read = PSEL & PENABLE & ~PWRITE;
  assign sleep_ctrl_q = hwif_out.sleep_ctrl.value.value;
  assign sleep_status_q = hwif_out.sleep_status.value.value;
  assign PREADY = 1'b1;
  assign PSLVERR = 1'b0;

  always_comb begin
    sleep_state_n = sleep_state_q;
    unique case (sleep_state_q)
      RUN: begin
        if (sleep_ctrl_q[0] && !event_i) begin
          sleep_state_n = SHUTDOWN;
        end
      end
      SHUTDOWN: begin
        if (event_i) begin
          sleep_state_n = RUN;
        end else if (!core_busy_i && !irq_i) begin
          sleep_state_n = SLEEP;
        end
      end
      SLEEP: begin
        if (event_i) begin
          sleep_state_n = RUN;
        end else if (irq_i) begin
          sleep_state_n = SHUTDOWN;
        end
      end
      default: sleep_state_n = RUN;
    endcase
  end

  always_comb begin
    fetch_en_o = 1'b1;
    clk_gate_core_o = 1'b1;
    core_sleeping_int = 1'b0;

    unique case (sleep_state_q)
      RUN: begin
        if (sleep_ctrl_q[0] && !event_i) begin
          fetch_en_o = 1'b0;
        end else begin
          fetch_en_o = 1'b1;
        end
      end
      SHUTDOWN: begin
        fetch_en_o = 1'b0;
      end
      SLEEP: begin
        clk_gate_core_o = event_i ? 1'b1 : 1'b0;
        core_sleeping_int = 1'b1;
        fetch_en_o = 1'b0;
      end
      default: begin
        fetch_en_o = 1'b1;
        clk_gate_core_o = 1'b1;
        core_sleeping_int = 1'b0;
      end
    endcase
  end

  always_comb begin
    sleep_ctrl_next = sleep_ctrl_q;
    sleep_status_next = sleep_status_q;
    sleep_status_next[0] = core_sleeping_int;

    if (core_sleeping_int || event_i) begin
      sleep_ctrl_next[0] = 1'b0;
    end

    if (apb_write && (register_adr == REG_SLEEP_CTRL)) begin
      sleep_ctrl_next = PWDATA;
    end
  end

  always_comb begin
    PRDATA = '0;
    if (apb_read) begin
      unique case (register_adr)
        REG_SLEEP_CTRL: PRDATA = sleep_ctrl_q;
        REG_SLEEP_STATUS: PRDATA = sleep_status_q;
        default: PRDATA = '0;
      endcase
    end
  end

  always_ff @(posedge HCLK or negedge HRESETn) begin
    if (!HRESETn) begin
      sleep_state_q <= RUN;
    end else begin
      sleep_state_q <= sleep_state_n;
    end
  end

  always_comb begin
    hwif_in.sleep_ctrl.value.next = sleep_ctrl_next;
    hwif_in.sleep_ctrl.value.we = 1'b1;
    hwif_in.sleep_status.value.next = sleep_status_next;
    hwif_in.sleep_status.value.we = 1'b1;
  end

  apb_event_sleep_unit_regs_core i_regs (
      .clk(HCLK),
      .arst_n(HRESETn),
      .s_cpuif_req(1'b0),
      .s_cpuif_req_is_wr(1'b1),
      .s_cpuif_addr({PADDR[2], 2'b00}),
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
    unused_core_req_stall_wr,
    unused_core_req_stall_rd,
    unused_core_rd_ack,
    unused_core_rd_err,
    unused_core_rd_data,
    unused_core_wr_ack,
    unused_core_wr_err
  };

endmodule

module apb_event_unit_peakrdl #(
    parameter APB_ADDR_WIDTH = 12
) (
    input  logic                      clk_i,
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

    input  logic               [31:0] irq_i,
    input  logic               [31:0] event_i,
    output logic               [31:0] irq_o,

    input  logic                      fetch_enable_i,
    output logic                      fetch_enable_o,
    output logic                      clk_gate_core_o,
    input  logic                      core_busy_i
);

  logic [31:0] events;
  logic [2:0] psel_int;
  logic [2:0] pready;
  logic [2:0] pslverr;
  logic [1:0] slave_address_int;
  logic [2:0][31:0] prdata;
  logic fetch_enable_ff1;
  logic fetch_enable_ff2;
  logic fetch_enable_int;

  assign fetch_enable_o = fetch_enable_ff2 & fetch_enable_int;
  assign slave_address_int = PADDR[5:4];

  always_comb begin
    psel_int = '0;
    if (slave_address_int < 2'd3) begin
      psel_int[slave_address_int] = PSEL;
    end
  end

  always_comb begin
    if (psel_int != 3'b000) begin
      PRDATA = prdata[slave_address_int];
      PREADY = pready[slave_address_int];
      PSLVERR = pslverr[slave_address_int];
    end else begin
      PRDATA = '0;
      PREADY = 1'b1;
      PSLVERR = 1'b0;
    end
  end

  generic_service_unit_peakrdl #(
      .APB_ADDR_WIDTH(APB_ADDR_WIDTH)
  ) i_interrupt_unit (
      .HCLK(HCLK),
      .HRESETn(HRESETn),
      .PADDR(PADDR),
      .PWDATA(PWDATA),
      .PWRITE(PWRITE),
      .PSEL(psel_int[0]),
      .PENABLE(PENABLE),
      .PRDATA(prdata[0]),
      .PREADY(pready[0]),
      .PSLVERR(pslverr[0]),
      .signal_i(irq_i),
      .irq_o(irq_o)
  );

  generic_service_unit_peakrdl #(
      .APB_ADDR_WIDTH(APB_ADDR_WIDTH)
  ) i_event_unit (
      .HCLK(HCLK),
      .HRESETn(HRESETn),
      .PADDR(PADDR),
      .PWDATA(PWDATA),
      .PWRITE(PWRITE),
      .PSEL(psel_int[1]),
      .PENABLE(PENABLE),
      .PRDATA(prdata[1]),
      .PREADY(pready[1]),
      .PSLVERR(pslverr[1]),
      .signal_i(event_i),
      .irq_o(events)
  );

  sleep_unit_peakrdl #(
      .APB_ADDR_WIDTH(APB_ADDR_WIDTH)
  ) i_sleep_unit (
      .HCLK(HCLK),
      .HRESETn(HRESETn),
      .PADDR(PADDR),
      .PWDATA(PWDATA),
      .PWRITE(PWRITE),
      .PSEL(psel_int[2]),
      .PENABLE(PENABLE),
      .PRDATA(prdata[2]),
      .PREADY(pready[2]),
      .PSLVERR(pslverr[2]),
      .irq_i(|irq_o),
      .event_i(|events),
      .core_busy_i(core_busy_i),
      .fetch_en_o(fetch_enable_int),
      .clk_gate_core_o(clk_gate_core_o)
  );

  always_ff @(posedge clk_i or negedge HRESETn) begin
    if (!HRESETn) begin
      fetch_enable_ff1 <= 1'b0;
      fetch_enable_ff2 <= 1'b0;
    end else begin
      fetch_enable_ff1 <= fetch_enable_i;
      fetch_enable_ff2 <= fetch_enable_ff1;
    end
  end

endmodule
