module dual_clock_mailbox_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  logic rst_n;

  logic write_clk;
  logic [7:0] write_data;
  logic write_valid;
  logic write_ready;
  logic [31:0] write_count;

  logic read_clk;
  logic [7:0] read_data;
  logic read_valid;
  logic read_ready;
  logic [31:0] read_count;

  logic [7:0] probe_in;
  logic [7:0] probe_echo;
  logic output_clk;

  localparam int unsigned kTransferCount = 16;

  longint unsigned checks;
  int unsigned failures;
  longint unsigned sim_cycles;

  task automatic expect_eq(
      input string label,
      input logic [63:0] actual,
      input logic [63:0] expected
  );
    checks++;
    if (actual !== expected) begin
      failures++;
      $error("%s: expected 0x%0h, got 0x%0h", label, expected, actual);
    end
  endtask

  task automatic reset_dut();
    rst_n = 1'b0;
    write_valid = 1'b0;
    write_data = '0;
    read_ready = 1'b0;
    probe_in = '0;

    #20ns;
    expect_eq("reset timer deadline", $time, 20);
    rst_n = 1'b1;
  endtask

  task automatic wait_reset_write();
    while (rst_n == 1'b0) begin
      @(posedge write_clk);
    end
  endtask

  task automatic wait_reset_read();
    while (rst_n == 1'b0) begin
      @(posedge read_clk);
    end
  endtask

  task automatic producer();
    wait_reset_write();

    for (int unsigned value = 0; value < kTransferCount; value++) begin
      forever begin
        @(posedge write_clk);
        // The edge sample point is already safe for a read-only observation.
        #1ps;
        if (write_ready != 1'b0) break;
      end

      write_data = (8'h40 + value[7:0]);
      write_valid = 1'b1;

      @(posedge write_clk);
      // Let the DUT consume valid before changing the next-cycle drive.
      #1ps;
      write_valid = 1'b0;
    end
  endtask

  task automatic consumer();
    logic [7:0] expected_data;

    wait_reset_read();

    for (int unsigned expected = 0; expected < kTransferCount; expected++) begin
      forever begin
        @(posedge read_clk);
        #1ps;
        if (read_valid != 1'b0) break;
      end

      expected_data = 8'h40 + expected[7:0];
      expect_eq("mailbox payload", read_data, expected_data);
      read_ready = 1'b1;

      @(posedge read_clk);
      #1ps;
      read_ready = 1'b0;
    end
  endtask

  task automatic traffic();
    fork
      producer();
      consumer();
    join

    expect_eq("write count", write_count, kTransferCount);
    expect_eq("read count", read_count, kTransferCount);
  endtask

  task automatic trigger_and_phase_probe();
    int unsigned winner;

    #7ns;
    expect_eq("independent timer", $time, 7);

    winner = 1;
    fork : first_trigger
      begin
        @(posedge read_clk);
        winner = 0;
      end
      begin
        #100ns;
        winner = 1;
      end
    join_any
    disable first_trigger;
    expect_eq("First chose read clock", winner, 0);

    @(write_clk);
    #1ps;
    probe_in = 8'ha5;
    #1ps;
    expect_eq("delay settles combinational output", probe_echo,
              8'ha5);

    probe_in = 8'h3c;
    #1ps;
    expect_eq("successive delay settles next drive", probe_echo,
              8'h3c);
  endtask

  task automatic output_clock_probe();
    @(posedge output_clk);
    expect_eq("DUT output clock edge", $time, 22);
  endtask

  initial begin
    write_clk = 1'b0;
    sim_cycles = 0;
    forever begin
      #2ns write_clk = ~write_clk;
      if (write_clk) sim_cycles++;
    end
  end

  initial begin
    read_clk = 1'b0;
    #1ns;
    forever begin
      #3ns read_clk = ~read_clk;
    end
  end

  initial begin
    rst_n = 1'b0;
    write_data = '0;
    write_valid = 1'b0;
    read_ready = 1'b0;
    probe_in = '0;
    checks = 0;
    failures = 0;
    fork
      reset_dut();
      traffic();
      trigger_and_phase_probe();
      output_clock_probe();
    join

    $display(
        "PURE_SV_MULTICLOCK_RESULT iterations=%0d checks=%0d sim_cycles=%0d failures=%0d",
        1, checks, sim_cycles, failures);
    if (failures != 0) begin
      $fatal(1, "dual_clock_mailbox pure-SV testbench failed");
    end
    $finish;
  end

  initial begin
    #1ms;
    $fatal(1, "dual_clock_mailbox pure-SV testbench timed out");
  end

  dual_clock_mailbox i_dut (
      .rst_n(rst_n),
      .write_clk(write_clk),
      .write_data(write_data),
      .write_valid(write_valid),
      .write_ready(write_ready),
      .write_count(write_count),
      .read_clk(read_clk),
      .read_data(read_data),
      .read_valid(read_valid),
      .read_ready(read_ready),
      .read_count(read_count),
      .probe_in(probe_in),
      .probe_echo(probe_echo),
      .output_clk(output_clk)
  );
endmodule : dual_clock_mailbox_sv_tb
