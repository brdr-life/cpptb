module vpi_peripheral_suite;
  localparam int APB_ADDR_WIDTH = 12;
  localparam int TIMER_CNT = 2;
  localparam int SPI_BUFFER_DEPTH = 10;
  localparam int SPI_LOG_BUFFER_DEPTH = 4;

  logic HCLK;
  logic HRESETn;

  logic [APB_ADDR_WIDTH-1:0] timer_PADDR;
  logic [31:0] timer_PWDATA;
  logic timer_PWRITE;
  logic timer_PSEL;
  logic timer_PENABLE;
  logic [31:0] timer_PRDATA;
  logic timer_PREADY;
  logic timer_PSLVERR;
  logic [(TIMER_CNT * 2)-1:0] timer_irq;

  logic [APB_ADDR_WIDTH-1:0] spi_PADDR;
  logic [31:0] spi_PWDATA;
  logic spi_PWRITE;
  logic spi_PSEL;
  logic spi_PENABLE;
  logic [31:0] spi_PRDATA;
  logic spi_PREADY;
  logic spi_PSLVERR;
  logic [7:0] spi_clk_div;
  logic spi_clk_div_valid;
  logic [31:0] spi_status;
  logic [31:0] spi_addr;
  logic [5:0] spi_addr_len;
  logic [31:0] spi_cmd;
  logic [5:0] spi_cmd_len;
  logic [3:0] spi_csreg;
  logic [15:0] spi_data_len;
  logic [15:0] spi_dummy_rd;
  logic [15:0] spi_dummy_wr;
  logic [SPI_LOG_BUFFER_DEPTH:0] spi_int_th_tx;
  logic [SPI_LOG_BUFFER_DEPTH:0] spi_int_th_rx;
  logic [SPI_LOG_BUFFER_DEPTH:0] spi_int_cnt_tx;
  logic [SPI_LOG_BUFFER_DEPTH:0] spi_int_cnt_rx;
  logic spi_int_en;
  logic spi_int_cnt_en;
  logic spi_int_rd_sta;
  logic spi_swrst;
  logic spi_rd;
  logic spi_wr;
  logic spi_qrd;
  logic spi_qwr;
  logic [31:0] spi_data_tx;
  logic spi_data_tx_valid;
  logic spi_data_tx_ready;
  logic [31:0] spi_data_rx;
  logic spi_data_rx_valid;
  logic spi_data_rx_ready;

  logic [APB_ADDR_WIDTH-1:0] i2c_PADDR;
  logic [31:0] i2c_PWDATA;
  logic i2c_PWRITE;
  logic i2c_PSEL;
  logic i2c_PENABLE;
  logic [31:0] i2c_PRDATA;
  logic i2c_PREADY;
  logic i2c_PSLVERR;
  logic i2c_interrupt;
  logic i2c_scl_pad_i;
  logic i2c_scl_pad_o;
  logic i2c_scl_padoen_o;
  logic i2c_sda_pad_i;
  logic i2c_sda_pad_o;
  logic i2c_sda_padoen_o;

  peripheral_suite_dut #(
      .APB_ADDR_WIDTH(APB_ADDR_WIDTH),
      .TIMER_CNT(TIMER_CNT),
      .SPI_BUFFER_DEPTH(SPI_BUFFER_DEPTH),
      .SPI_LOG_BUFFER_DEPTH(SPI_LOG_BUFFER_DEPTH)
  ) i_dut (
      .HCLK(HCLK),
      .HRESETn(HRESETn),
      .timer_PADDR(timer_PADDR),
      .timer_PWDATA(timer_PWDATA),
      .timer_PWRITE(timer_PWRITE),
      .timer_PSEL(timer_PSEL),
      .timer_PENABLE(timer_PENABLE),
      .timer_PRDATA(timer_PRDATA),
      .timer_PREADY(timer_PREADY),
      .timer_PSLVERR(timer_PSLVERR),
      .timer_irq(timer_irq),
      .spi_PADDR(spi_PADDR),
      .spi_PWDATA(spi_PWDATA),
      .spi_PWRITE(spi_PWRITE),
      .spi_PSEL(spi_PSEL),
      .spi_PENABLE(spi_PENABLE),
      .spi_PRDATA(spi_PRDATA),
      .spi_PREADY(spi_PREADY),
      .spi_PSLVERR(spi_PSLVERR),
      .spi_clk_div(spi_clk_div),
      .spi_clk_div_valid(spi_clk_div_valid),
      .spi_status(spi_status),
      .spi_addr(spi_addr),
      .spi_addr_len(spi_addr_len),
      .spi_cmd(spi_cmd),
      .spi_cmd_len(spi_cmd_len),
      .spi_csreg(spi_csreg),
      .spi_data_len(spi_data_len),
      .spi_dummy_rd(spi_dummy_rd),
      .spi_dummy_wr(spi_dummy_wr),
      .spi_int_th_tx(spi_int_th_tx),
      .spi_int_th_rx(spi_int_th_rx),
      .spi_int_cnt_tx(spi_int_cnt_tx),
      .spi_int_cnt_rx(spi_int_cnt_rx),
      .spi_int_en(spi_int_en),
      .spi_int_cnt_en(spi_int_cnt_en),
      .spi_int_rd_sta(spi_int_rd_sta),
      .spi_swrst(spi_swrst),
      .spi_rd(spi_rd),
      .spi_wr(spi_wr),
      .spi_qrd(spi_qrd),
      .spi_qwr(spi_qwr),
      .spi_data_tx(spi_data_tx),
      .spi_data_tx_valid(spi_data_tx_valid),
      .spi_data_tx_ready(spi_data_tx_ready),
      .spi_data_rx(spi_data_rx),
      .spi_data_rx_valid(spi_data_rx_valid),
      .spi_data_rx_ready(spi_data_rx_ready),
      .i2c_PADDR(i2c_PADDR),
      .i2c_PWDATA(i2c_PWDATA),
      .i2c_PWRITE(i2c_PWRITE),
      .i2c_PSEL(i2c_PSEL),
      .i2c_PENABLE(i2c_PENABLE),
      .i2c_PRDATA(i2c_PRDATA),
      .i2c_PREADY(i2c_PREADY),
      .i2c_PSLVERR(i2c_PSLVERR),
      .i2c_interrupt(i2c_interrupt),
      .i2c_scl_pad_i(i2c_scl_pad_i),
      .i2c_scl_pad_o(i2c_scl_pad_o),
      .i2c_scl_padoen_o(i2c_scl_padoen_o),
      .i2c_sda_pad_i(i2c_sda_pad_i),
      .i2c_sda_pad_o(i2c_sda_pad_o),
      .i2c_sda_padoen_o(i2c_sda_padoen_o)
  );

endmodule
