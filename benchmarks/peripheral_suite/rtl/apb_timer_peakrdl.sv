module apb_timer_peakrdl #(
    parameter APB_ADDR_WIDTH = 12,
    parameter TIMER_CNT = 2
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

    output logic [(TIMER_CNT * 2) - 1:0] irq_o
);

    localparam int RegsMaxAdr = 2;

    logic [TIMER_CNT-1:0] psel_int;
    logic [TIMER_CNT-1:0] pready;
    logic [TIMER_CNT-1:0] pslverr;
    logic [$clog2(TIMER_CNT)-1:0] slave_address_int;
    logic [TIMER_CNT-1:0] [31:0] prdata;

    assign slave_address_int = PADDR[$clog2(TIMER_CNT)+RegsMaxAdr+1:RegsMaxAdr+2];

    always_comb begin
        psel_int = '0;
        psel_int[slave_address_int] = PSEL;
    end

    always_comb begin
        if (psel_int != '0) begin
            PRDATA = prdata[slave_address_int];
            PREADY = pready[slave_address_int];
            PSLVERR = pslverr[slave_address_int];
        end else begin
            PRDATA = '0;
            PREADY = 1'b1;
            PSLVERR = 1'b0;
        end
    end

    genvar k;
    generate
        for (k = 0; k < TIMER_CNT; k++) begin : TIMER_GEN
            timer_peakrdl #(
                .APB_ADDR_WIDTH(APB_ADDR_WIDTH)
            ) timer_i (
                .HCLK(HCLK),
                .HRESETn(HRESETn),
                .PADDR(PADDR),
                .PWDATA(PWDATA),
                .PWRITE(PWRITE),
                .PSEL(psel_int[k]),
                .PENABLE(PENABLE),
                .PRDATA(prdata[k]),
                .PREADY(pready[k]),
                .PSLVERR(pslverr[k]),
                .irq_o(irq_o[2*k+1 : 2*k])
            );
        end
    endgenerate

endmodule
