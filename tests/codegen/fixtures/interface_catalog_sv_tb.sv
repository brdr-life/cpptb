module interface_catalog_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  logic link_clk [3];
  stream_if #(.WIDTH(8)) links [3] (.clk(link_clk));
  logic gpio_drive;
  tri [3:0] gpio;
  logic [3:0] gpio_seen;
  tri [64:0] wide_gpio;
  logic [64:0] wide_gpio_seen;
  logic [3:0] gpio_tb_drive;
  logic gpio_tb_oe;
  logic [64:0] wide_gpio_tb_drive;
  logic wide_gpio_tb_oe;
  logic sideband_tb_drive [3];
  logic sideband_tb_oe [3];
  int checks;
  int failures;

  assign gpio = gpio_tb_oe ? gpio_tb_drive : 4'hz;
  assign wide_gpio = wide_gpio_tb_oe ? wide_gpio_tb_drive : 65'bz;
  for (genvar index = 0; index < 3; ++index) begin
    assign links[index].sideband =
        sideband_tb_oe[index] ? sideband_tb_drive[index] : 1'bz;
  end

  task automatic expect_eq(string label, int actual, int expected);
    checks++;
    if (actual != expected) begin
      failures++;
      $error("%s: actual=%0h expected=%0h", label, actual, expected);
    end
  endtask

  initial forever #5ns link_clk[0] = ~link_clk[0];
  initial forever #7ns link_clk[1] = ~link_clk[1];

  initial begin
    link_clk = '{default: 0};
    links[0].reset_n = 0;
    links[1].reset_n = 0;
    links[2].reset_n = 0;
    links[0].valid = 0;
    links[1].valid = 0;
    links[2].valid = 0;
    links[0].data = 0;
    links[1].data = 0;
    links[2].data = 0;
    gpio_drive = 0;
    gpio_tb_drive = 0;
    gpio_tb_oe = 0;
    wide_gpio_tb_drive = 0;
    wide_gpio_tb_oe = 0;
    sideband_tb_drive = '{default: 0};
    sideband_tb_oe = '{default: 0};
    checks = 0;
    failures = 0;

    @(posedge link_clk[0]);
    expect_eq("link zero reset ready", links[0].ready, 0);
    expect_eq("link one reset ready", links[1].ready, 0);

    link_clk[2] = 1;
    #1ns;
    expect_eq("unscheduled interface clock remains directly driven",
              links[2].clock_seen, 1);

    links[0].reset_n = 1;
    links[1].reset_n = 1;
    links[0].data = 8'h24;
    links[1].data = 8'h35;
    #1ns;
    expect_eq("link zero ready", links[0].ready, 1);
    expect_eq("link one ready", links[1].ready, 1);
    expect_eq("link zero observed", links[0].observed, 8'h24);
    expect_eq("link one observed", links[1].observed, 8'h36);

    sideband_tb_drive[0] = 1;
    sideband_tb_oe[0] = 1;
    #1ns;
    expect_eq("testbench drives interface inout", links[0].sideband, 1);
    sideband_tb_oe[0] = 0;
    links[0].valid = 1;
    links[0].data = 8'h22;
    #1ns;
    expect_eq("DUT drives released interface inout", links[0].sideband, 0);

    gpio_tb_drive = 4'h5;
    gpio_tb_oe = 1;
    #1ns;
    expect_eq("testbench drives top inout", gpio_seen, 4'h5);
    gpio_tb_oe = 0;
    gpio_drive = 1;
    #1ns;
    expect_eq("DUT drives released top inout", gpio, 4'ha);

    wide_gpio_tb_drive = 65'h1_01234567_89abcdef;
    wide_gpio_tb_oe = 1;
    #1ns;
    checks++;
    if (wide_gpio_seen != wide_gpio_tb_drive) begin
      failures++;
      $error("testbench drives wide top inout");
    end
    wide_gpio_tb_oe = 0;

    $display("PURE_SV_INTERFACE_CATALOG_RESULT checks=%0d failures=%0d",
             checks, failures);
    if (failures != 0) $fatal(1, "interface catalog test failed");
    $finish;
  end

  interface_catalog i_dut (.*);
endmodule
