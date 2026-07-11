module peripheral_suite_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  localparam int APB_ADDR_WIDTH = 12;
  localparam int TIMER_CNT = 2;
  localparam int SPI_BUFFER_DEPTH = 10;
  localparam int SPI_LOG_BUFFER_DEPTH = 4;

  localparam int TIMER_REG_TIMER = 0;
  localparam int TIMER_REG_CTRL = 1;
  localparam int TIMER_REG_CMP = 2;

  localparam int SPI_STATUS = 0;
  localparam int SPI_CLKDIV = 1;
  localparam int SPI_CMD = 2;
  localparam int SPI_ADDR = 3;
  localparam int SPI_LEN = 4;
  localparam int SPI_DUMMY = 5;
  localparam int SPI_TXFIFO = 6;
  localparam int SPI_RXFIFO = 8;
  localparam int SPI_INTCFG = 9;

  localparam int I2C_PRESCALER = 0;
  localparam int I2C_CTRL = 1;
  localparam int I2C_STATUS = 3;
  localparam int I2C_TX = 4;
  localparam int I2C_CMD = 5;

  typedef enum int {
    BUS_TIMER,
    BUS_SPI,
    BUS_I2C
  } apb_bus_t;

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

  int unsigned iterations;
  longint unsigned checks;
  longint unsigned sim_cycles;
  int unsigned failures;
  bit timer_done;
  bit spi_done;
  bit i2c_done;

  always #1ns HCLK = ~HCLK;
  always @(posedge HCLK) sim_cycles++;

  function automatic int unsigned timer_addr(input int unsigned timer_index,
                                             input int unsigned reg_index);
    return ((timer_index & 1) << 4) | ((reg_index & 3) << 2);
  endfunction

  function automatic int unsigned word_addr(input int unsigned word_index);
    return word_index << 2;
  endfunction

  function automatic bit sequences_done();
    return timer_done && spi_done && i2c_done;
  endfunction

  task automatic sample_outputs();
    #1ps;
  endtask

  task automatic check32(input longint unsigned actual,
                         input longint unsigned expected,
                         input string label);
    checks++;
    if (actual == expected) begin
      return;
    end

    failures++;
    if (failures <= 8) begin
      $display("SV_PERIPHERAL_MISMATCH %s actual=0x%08x expected=0x%08x",
               label, actual[31:0], expected[31:0]);
    end
  endtask

  task automatic wait_cycles(input int unsigned cycles);
    repeat (cycles) @(posedge HCLK);
  endtask

  task automatic wait_reset_released();
    while (HRESETn == 1'b0) begin
      @(posedge HCLK);
    end
  endtask

  task automatic drive_apb(input apb_bus_t bus,
                           input int unsigned byte_addr,
                           input logic [31:0] data,
                           input bit write,
                           input bit psel,
                           input bit penable);
    case (bus)
      BUS_TIMER: begin
        timer_PADDR = byte_addr[APB_ADDR_WIDTH-1:0];
        timer_PWDATA = data;
        timer_PWRITE = write;
        timer_PSEL = psel;
        timer_PENABLE = penable;
      end
      BUS_SPI: begin
        spi_PADDR = byte_addr[APB_ADDR_WIDTH-1:0];
        spi_PWDATA = data;
        spi_PWRITE = write;
        spi_PSEL = psel;
        spi_PENABLE = penable;
      end
      BUS_I2C: begin
        i2c_PADDR = byte_addr[APB_ADDR_WIDTH-1:0];
        i2c_PWDATA = data;
        i2c_PWRITE = write;
        i2c_PSEL = psel;
        i2c_PENABLE = penable;
      end
    endcase
  endtask

  task automatic drive_apb_idle(input apb_bus_t bus);
    drive_apb(bus, 0, 32'h0, 1'b0, 1'b0, 1'b0);
  endtask

  function automatic logic [31:0] apb_prdata(input apb_bus_t bus);
    case (bus)
      BUS_TIMER: return timer_PRDATA;
      BUS_SPI: return spi_PRDATA;
      BUS_I2C: return i2c_PRDATA;
      default: return 32'h0;
    endcase
  endfunction

  function automatic logic apb_pready(input apb_bus_t bus);
    case (bus)
      BUS_TIMER: return timer_PREADY;
      BUS_SPI: return spi_PREADY;
      BUS_I2C: return i2c_PREADY;
      default: return 1'b0;
    endcase
  endfunction

  function automatic logic apb_pslverr(input apb_bus_t bus);
    case (bus)
      BUS_TIMER: return timer_PSLVERR;
      BUS_SPI: return spi_PSLVERR;
      BUS_I2C: return i2c_PSLVERR;
      default: return 1'b0;
    endcase
  endfunction

  task automatic apb_write(input apb_bus_t bus,
                           input int unsigned byte_addr,
                           input logic [31:0] data);
    @(negedge HCLK);
    drive_apb(bus, byte_addr, data, 1'b1, 1'b1, 1'b0);

    @(negedge HCLK);
    drive_apb(bus, byte_addr, data, 1'b1, 1'b1, 1'b1);

    @(posedge HCLK);
    sample_outputs();
    check32(apb_pready(bus), 1, $sformatf("write PREADY addr=0x%03x", byte_addr));
    check32(apb_pslverr(bus), 0, $sformatf("write PSLVERR addr=0x%03x", byte_addr));

    @(negedge HCLK);
    drive_apb_idle(bus);
  endtask

  task automatic apb_read_expect(input apb_bus_t bus,
                                 input int unsigned byte_addr,
                                 input logic [31:0] expected);
    @(negedge HCLK);
    drive_apb(bus, byte_addr, 32'h0, 1'b0, 1'b1, 1'b0);

    @(negedge HCLK);
    drive_apb(bus, byte_addr, 32'h0, 1'b0, 1'b1, 1'b1);

    @(posedge HCLK);
    sample_outputs();
    check32(apb_prdata(bus), expected, $sformatf("read PRDATA addr=0x%03x", byte_addr));
    check32(apb_pready(bus), 1, $sformatf("read PREADY addr=0x%03x", byte_addr));
    check32(apb_pslverr(bus), 0, $sformatf("read PSLVERR addr=0x%03x", byte_addr));

    @(negedge HCLK);
    drive_apb_idle(bus);
  endtask

  task automatic apb_read_ok(input apb_bus_t bus,
                             input int unsigned byte_addr);
    @(negedge HCLK);
    drive_apb(bus, byte_addr, 32'h0, 1'b0, 1'b1, 1'b0);

    @(negedge HCLK);
    drive_apb(bus, byte_addr, 32'h0, 1'b0, 1'b1, 1'b1);

    @(posedge HCLK);
    sample_outputs();
    check32(apb_pready(bus), 1, $sformatf("read-ok PREADY addr=0x%03x", byte_addr));
    check32(apb_pslverr(bus), 0, $sformatf("read-ok PSLVERR addr=0x%03x", byte_addr));

    @(negedge HCLK);
    drive_apb_idle(bus);
  endtask

  task automatic reset_driver();
    HRESETn = 1'b0;
    drive_apb_idle(BUS_TIMER);
    drive_apb_idle(BUS_SPI);
    drive_apb_idle(BUS_I2C);

    spi_status = 32'h0;
    spi_data_tx_ready = 1'b1;
    spi_data_rx = 32'h0;
    spi_data_rx_valid = 1'b1;
    i2c_scl_pad_i = 1'b1;
    i2c_sda_pad_i = 1'b1;

    wait_cycles(8);
    HRESETn = 1'b1;
    wait_cycles(8);
  endtask

  task automatic wait_timer_irq(input int unsigned bit_index,
                                input int unsigned max_cycles);
    bit seen;
    seen = 1'b0;
    for (int unsigned i = 0; i < max_cycles; i++) begin
      @(posedge HCLK);
      sample_outputs();
      if (((timer_irq >> bit_index) & 1) != 0) begin
        seen = 1'b1;
        break;
      end
    end
    check32(seen ? 1 : 0, 1, "timer irq wait");
  endtask

  task automatic timer_sequence();
    wait_reset_released();
    wait_cycles(2);

    for (int unsigned i = 0; i < iterations; i++) begin
      int unsigned timer_index;
      int unsigned cmp_value;
      int unsigned prescale;
      int unsigned ctrl;

      timer_index = i & 1;
      cmp_value = 3 + (i & 7);
      prescale = (i >> 2) & 3;
      ctrl = 1 | (prescale << 3);

      apb_write(BUS_TIMER, timer_addr(timer_index, TIMER_REG_TIMER), 32'h0);
      apb_write(BUS_TIMER, timer_addr(timer_index, TIMER_REG_CMP), cmp_value);
      apb_write(BUS_TIMER, timer_addr(timer_index, TIMER_REG_CTRL), ctrl);
      wait_timer_irq(timer_index * 2 + 1, 48 + prescale * cmp_value * 4);
      apb_write(BUS_TIMER, timer_addr(timer_index, TIMER_REG_CTRL), 32'h0);

      if ((i % 8) == 0) begin
        apb_write(BUS_TIMER, timer_addr(timer_index, TIMER_REG_CMP), 32'h0);
        apb_write(BUS_TIMER, timer_addr(timer_index, TIMER_REG_TIMER), 32'hffff_fffe);
        apb_write(BUS_TIMER, timer_addr(timer_index, TIMER_REG_CTRL), 32'h1);
        wait_timer_irq(timer_index * 2, 8);
        apb_write(BUS_TIMER, timer_addr(timer_index, TIMER_REG_CTRL), 32'h0);
      end
    end

    timer_done = 1'b1;
  endtask

  task automatic spi_sequence();
    wait_reset_released();
    wait_cycles(4);

    for (int unsigned i = 0; i < iterations; i++) begin
      logic [31:0] div;
      logic [31:0] cmd;
      logic [31:0] addr;
      logic [31:0] length;
      logic [31:0] dummy;
      logic [31:0] intcfg;
      logic [31:0] status;
      logic [31:0] rx;

      div = (i * 13 + 7) & 32'hff;
      cmd = 32'h1100_0000 ^ (i * 32'h0101_0101);
      addr = 32'h5500_0000 ^ (i * 32'h0011_0021);
      length = ((i & 32'hff) << 24)
             | (((i + 3) & 32'hff) << 16)
             | (((i + 5) & 32'h3f) << 8)
             | ((i + 7) & 32'h3f);
      dummy = ((32'hab00 | (i & 32'hff)) << 16) | (32'h1200 | (i & 32'hff));
      intcfg = 32'h8000_0000
             | ((i & 32'h1f) << 24)
             | (((i + 1) & 32'h1f) << 16)
             | (((i + 2) & 32'h1f) << 8)
             | ((i + 3) & 32'h1f);
      status = 32'ha500_0000 | ((i & 32'hf) << 8) | (i & 32'hff);
      rx = 32'hcafe_0000 ^ (i * 32'h1021);

      spi_status = status;
      spi_data_rx = rx;
      spi_data_rx_valid = 1'b1;
      spi_data_tx_ready = ((i & 3) == 0) ? 1'b0 : 1'b1;

      apb_write(BUS_SPI, word_addr(SPI_STATUS), ((i & 32'hf) << 8) | (i & 32'hf));
      sample_outputs();
      check32(spi_csreg, i & 32'hf, "spi csreg");

      apb_write(BUS_SPI, word_addr(SPI_CLKDIV), div);
      sample_outputs();
      check32(spi_clk_div, div, "spi clk_div");

      apb_write(BUS_SPI, word_addr(SPI_CMD), cmd);
      apb_write(BUS_SPI, word_addr(SPI_ADDR), addr);
      apb_write(BUS_SPI, word_addr(SPI_LEN), length);
      apb_write(BUS_SPI, word_addr(SPI_DUMMY), dummy);
      apb_write(BUS_SPI, word_addr(SPI_INTCFG), intcfg);
      sample_outputs();

      check32(spi_cmd, cmd, "spi cmd");
      check32(spi_addr, addr, "spi addr");
      check32(spi_cmd_len, length & 32'h3f, "spi cmd_len");
      check32(spi_addr_len, (length >> 8) & 32'h3f, "spi addr_len");
      check32(spi_data_len, (length >> 16) & 32'hffff, "spi data_len");
      check32(spi_dummy_rd, dummy & 32'hffff, "spi dummy_rd");
      check32(spi_dummy_wr, (dummy >> 16) & 32'hffff, "spi dummy_wr");

      apb_write(BUS_SPI, word_addr(SPI_TXFIFO), 32'h1357_0000 | i);
      apb_read_expect(BUS_SPI, word_addr(SPI_RXFIFO), rx);
      apb_read_expect(BUS_SPI, word_addr(SPI_STATUS), status);
      apb_read_expect(BUS_SPI, word_addr(SPI_CMD), cmd);
      apb_read_expect(BUS_SPI, word_addr(SPI_ADDR), addr);
      apb_read_expect(BUS_SPI, word_addr(SPI_LEN), length & 32'hffff_3f3f);
      apb_read_expect(BUS_SPI, word_addr(SPI_DUMMY), dummy);
    end

    spi_done = 1'b1;
  endtask

  task automatic i2c_sequence();
    wait_reset_released();
    wait_cycles(6);

    for (int unsigned i = 0; i < iterations; i++) begin
      logic [31:0] prescaler;
      logic [31:0] tx;

      prescaler = 4 + (i & 7);
      tx = 32'h40 | (i & 32'h3f);

      i2c_scl_pad_i = 1'b1;
      i2c_sda_pad_i = 1'b1;
      apb_write(BUS_I2C, word_addr(I2C_PRESCALER), prescaler);
      apb_write(BUS_I2C, word_addr(I2C_CTRL), 32'hc0);
      apb_write(BUS_I2C, word_addr(I2C_TX), tx);
      apb_read_expect(BUS_I2C, word_addr(I2C_PRESCALER), prescaler);
      apb_read_expect(BUS_I2C, word_addr(I2C_CTRL), 32'hc0);
      apb_read_expect(BUS_I2C, word_addr(I2C_TX), tx);

      apb_write(BUS_I2C, word_addr(I2C_CMD), 32'h90);
      wait_cycles(20 + (i & 32'hf));
      apb_read_ok(BUS_I2C, word_addr(I2C_STATUS));
      apb_write(BUS_I2C, word_addr(I2C_CMD), 32'h01);

      if ((i & 3) == 0) begin
        i2c_sda_pad_i = 1'b0;
        wait_cycles(4);
        i2c_sda_pad_i = 1'b1;
        wait_cycles(4);
      end
    end

    i2c_done = 1'b1;
  endtask

  task automatic timer_monitor();
    wait_reset_released();
    while (!sequences_done()) begin
      @(posedge HCLK);
      sample_outputs();
      check32(timer_irq & ~32'hf, 0, "timer irq high bits");
    end
  endtask

  task automatic spi_monitor();
    wait_reset_released();
    while (!sequences_done()) begin
      @(posedge HCLK);
      sample_outputs();
      check32((spi_clk_div_valid <= 1) ? 1 : 0, 1, "spi clk_div_valid");
      check32((spi_data_tx_valid <= 1) ? 1 : 0, 1, "spi data_tx_valid");
      check32((spi_data_rx_ready <= 1) ? 1 : 0, 1, "spi data_rx_ready");
    end
  endtask

  task automatic i2c_monitor();
    wait_reset_released();
    while (!sequences_done()) begin
      @(posedge HCLK);
      sample_outputs();
      check32((i2c_interrupt <= 1) ? 1 : 0, 1, "i2c interrupt");
      check32((i2c_scl_padoen_o <= 1) ? 1 : 0, 1, "i2c scl_padoen");
      check32((i2c_sda_padoen_o <= 1) ? 1 : 0, 1, "i2c sda_padoen");
    end
  endtask

  initial begin
    HCLK = 1'b0;
    HRESETn = 1'b0;
    checks = 0;
    failures = 0;
    sim_cycles = 0;
    timer_done = 1'b0;
    spi_done = 1'b0;
    i2c_done = 1'b0;
    iterations = 1000;
    void'($value$plusargs("PERIPHERAL_SUITE_ITERS=%d", iterations));

    fork
      reset_driver();
      timer_sequence();
      spi_sequence();
      i2c_sequence();
      timer_monitor();
      spi_monitor();
      i2c_monitor();
    join

    $display("PURE_SV_PERIPHERAL_RESULT iterations=%0d checks=%0d sim_cycles=%0d failures=%0d",
             iterations, checks, sim_cycles, failures);
    if (failures != 0) begin
      $fatal(1, "SV_PERIPHERAL_FAIL failures=%0d checks=%0d", failures, checks);
    end
    $finish;
  end

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
