module timer_peakrdl #(
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

    output logic                [1:0] irq_o
);

    import apb_timer_regs_core_pkg::*;

    localparam logic [1:0] RegTimer     = 2'b00;
    localparam logic [1:0] RegTimerCtrl = 2'b01;
    localparam logic [1:0] RegCmp       = 2'b10;

    localparam int PrescalerStartBit = 3;
    localparam int PrescalerStopBit  = 5;
    localparam int EnableBit         = 0;

    apb_timer_regs__in_t  hwif_in;
    apb_timer_regs__out_t hwif_out;

    logic [1:0]  register_adr;
    logic [3:0]  core_addr;
    logic        apb_write;
    logic        cmp_write;

    logic [31:0] cycle_counter_n;
    logic [31:0] cycle_counter_q;
    logic [31:0] timer_next;
    logic        timer_we;
    logic [2:0]  prescaler_int;

    logic        unused_core_req_stall_wr;
    logic        unused_core_req_stall_rd;
    logic        unused_core_rd_ack;
    logic        unused_core_rd_err;
    logic [31:0] unused_core_rd_data;
    logic        unused_core_wr_ack;
    logic        unused_core_wr_err;

    assign register_adr = PADDR[3:2];
    assign core_addr = {register_adr, 2'b00};
    assign apb_write = PSEL & PENABLE & PWRITE;
    assign cmp_write = apb_write & (register_adr == RegCmp);
    assign prescaler_int = hwif_out.timer_ctrl.value.value[PrescalerStopBit:PrescalerStartBit];

    assign PREADY  = 1'b1;
    assign PSLVERR = 1'b0;

    apb_timer_regs_core i_regs (
        .clk(HCLK),
        .arst_n(HRESETn),
        .s_cpuif_req(apb_write),
        .s_cpuif_req_is_wr(1'b1),
        .s_cpuif_addr(core_addr),
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

    always_comb begin
        irq_o = 2'b00;

        if ((hwif_out.timer.value.value == 32'hffff_ffff) && (prescaler_int == cycle_counter_q)) begin
            irq_o[0] = 1'b1;
        end

        if ((hwif_out.cmp.value.value != 32'h0000_0000)
            && (hwif_out.timer.value.value == hwif_out.cmp.value.value)
            && (prescaler_int == cycle_counter_q)) begin
            irq_o[1] = 1'b1;
        end
    end

    always_comb begin
        timer_next = hwif_out.timer.value.value;
        timer_we = 1'b0;
        cycle_counter_n = cycle_counter_q + 32'd1;

        if (irq_o[0] || irq_o[1]) begin
            timer_next = 32'b0;
            timer_we = 1'b1;
        end else if (hwif_out.timer_ctrl.value.value[EnableBit]
                     && (prescaler_int != 3'b000)
                     && (prescaler_int == cycle_counter_q[2:0])) begin
            timer_next = hwif_out.timer.value.value + 32'd1;
            timer_we = 1'b1;
        end else if (hwif_out.timer_ctrl.value.value[EnableBit]
                     && (prescaler_int == 3'b000)) begin
            timer_next = hwif_out.timer.value.value + 32'd1;
            timer_we = 1'b1;
        end

        if (cycle_counter_q >= {29'b0, prescaler_int}) begin
            cycle_counter_n = 32'b0;
        end

        if (cmp_write) begin
            timer_next = 32'b0;
            timer_we = 1'b1;
        end

        hwif_in.timer.value.next = timer_next;
        hwif_in.timer.value.we = timer_we;
    end

    always_comb begin
        PRDATA = 32'b0;

        if (PSEL && PENABLE && !PWRITE) begin
            unique case (register_adr)
                RegTimer: begin
                    PRDATA = hwif_out.timer.value.value;
                end
                RegTimerCtrl: begin
                    PRDATA = hwif_out.timer_ctrl.value.value;
                end
                RegCmp: begin
                    PRDATA = hwif_out.cmp.value.value;
                end
                default: begin
                    PRDATA = 32'b0;
                end
            endcase
        end
    end

    always_ff @(posedge HCLK or negedge HRESETn) begin
        if (!HRESETn) begin
            cycle_counter_q <= 32'b0;
        end else begin
            cycle_counter_q <= cycle_counter_n;
        end
    end

endmodule
