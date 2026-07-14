`ifndef OPEN_CORE_WORKLOAD
`define OPEN_CORE_WORKLOAD 0
`endif

module open_cores_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  logic        clk;
  logic        rst_n;
  logic        cpu_prog_we;
  logic [31:0] cpu_prog_addr;
  logic [31:0] cpu_prog_data;
  logic        cpu_done;
  logic        cpu_trap;
  logic [31:0] cpu_result;
  logic [31:0] cpu_instruction_count;
  logic        aes_cs;
  logic        aes_we;
  logic [7:0]  aes_address;
  logic [31:0] aes_write_data;
  logic [31:0] aes_read_data;
  logic [63:0] fcs_tdata;
  logic [7:0]  fcs_tkeep;
  logic        fcs_tvalid;
  logic        fcs_tready;
  logic        fcs_tlast;
  logic [31:0] fcs_result;
  logic        fcs_valid;

  logic [31:0] firmware [0:13];
  logic [31:0] aes_key [0:7];
  logic [31:0] aes_plaintext [0:3][0:3];
  logic [31:0] aes_ciphertext [0:3][0:3];
  int unsigned iterations;
  string workload;
  longint unsigned transactions;
  longint unsigned checks;
  longint unsigned sim_cycles;
  logic [31:0] checksum;
  int unsigned failures;

  always #1ns clk = ~clk;
  always @(posedge clk) sim_cycles++;

  function automatic logic [31:0] stimulus(input int unsigned ordinal);
    return ((ordinal + 1) * 32'h1f12_3bb5) ^ 32'hc001_d00d;
  endfunction

  function automatic logic [7:0] frame_byte(
      input int unsigned packet,
      input int unsigned offset
  );
    return stimulus(packet * 2048 + offset)[7:0];
  endfunction

  function automatic int unsigned frame_length(input int unsigned packet);
    return 64 + ((packet * 37) % 1455);
  endfunction

  function automatic logic [31:0] crc32_byte(
      input logic [31:0] current,
      input logic [7:0] data
  );
    logic [31:0] value;
    value = current ^ data;
    for (int bit_index = 0; bit_index < 8; bit_index++)
      value = value[0] ? ((value >> 1) ^ 32'hedb8_8320) : (value >> 1);
    return value;
  endfunction

  function automatic logic [31:0] xorshift32(input logic [31:0] value);
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return value;
  endfunction

  task automatic check32(
      input string label,
      input logic [31:0] actual,
      input logic [31:0] expected
  );
    checks++;
    if (actual === expected) return;
    failures++;
    if (failures <= 8)
      $display("OPEN_CORE_BENCH_MISMATCH mode=pure_sv workload=%s label=%s actual=0x%08x expected=0x%08x",
               workload, label, actual, expected);
  endtask

  task automatic fold(input logic [31:0] value);
    checksum = (checksum ^ value) * 32'h0100_0193;
  endtask

  task automatic initialize_inputs();
    rst_n = 1'b1;
    cpu_prog_we = 1'b0;
    cpu_prog_addr = '0;
    cpu_prog_data = '0;
    aes_cs = 1'b0;
    aes_we = 1'b0;
    aes_address = '0;
    aes_write_data = '0;
    fcs_tdata = '0;
    fcs_tkeep = '0;
    fcs_tvalid = 1'b0;
    fcs_tlast = 1'b0;
  endtask

  task automatic reset_dut();
    @(negedge clk);
    rst_n = 1'b0;
    repeat (4) @(posedge clk);
    @(negedge clk);
    rst_n = 1'b1;
  endtask

  task automatic program_word(
      input logic [31:0] address,
      input logic [31:0] data
  );
    @(negedge clk);
    cpu_prog_addr = address;
    cpu_prog_data = data;
    cpu_prog_we = 1'b1;
    @(posedge clk);
  endtask

  task automatic run_picorv32();
    logic [31:0] expected;
    @(negedge clk);
    rst_n = 1'b0;
    for (int unsigned index = 0; index < 14; index++)
      program_word(index * 4, firmware[index]);
    program_word(32'h100, iterations);
    @(negedge clk);
    cpu_prog_we = 1'b0;
    rst_n = 1'b1;
    @(posedge cpu_done);
    #1ps;
    expected = 32'h1234_5678;
    for (int unsigned iteration = 0; iteration < iterations; iteration++)
      expected = xorshift32(expected);
    check32("firmware result", cpu_result, expected);
    check32("CPU trap", cpu_trap, 0);
    fold(cpu_result);
    transactions = iterations;
  endtask

  task automatic aes_write(
      input logic [7:0] address,
      input logic [31:0] data
  );
    @(negedge clk);
    aes_cs = 1'b1;
    aes_we = 1'b1;
    aes_address = address;
    aes_write_data = data;
    @(posedge clk);
  endtask

  task automatic aes_select_read(input logic [7:0] address);
    @(negedge clk);
    aes_cs = 1'b1;
    aes_we = 1'b0;
    aes_address = address;
  endtask

  task automatic aes_wait_status(input logic [31:0] mask);
    int unsigned polls;
    bit saw_clear;
    polls = 0;
    saw_clear = 0;
    aes_select_read(8'h09);
    forever begin
      @(posedge clk);
      #1ps;
      if (!(aes_read_data & mask))
        saw_clear = 1;
      else if (saw_clear)
        return;
      polls++;
      if (polls == 256)
        $fatal(1, "AES status timeout mask=%08x status=%08x",
               mask, aes_read_data);
    end
  endtask

  task automatic aes_read(
      input logic [7:0] address,
      output logic [31:0] value
  );
    aes_select_read(address);
    #1ps;
    value = aes_read_data;
  endtask

  task automatic run_aes();
    logic [31:0] actual;
    logic [31:0] status;
    reset_dut();
    aes_write(8'h0a, 1);
    for (int unsigned index = 0; index < 8; index++)
      aes_write(8'h10 + index, aes_key[index]);
    aes_write(8'h08, 1);
    aes_wait_status(1);

    for (int unsigned block = 0; block < iterations; block++) begin
      for (int unsigned word = 0; word < 4; word++)
        aes_write(8'h20 + word, aes_plaintext[block & 3][word]);
      aes_write(8'h08, 2);
      aes_wait_status(2);
      for (int unsigned word = 0; word < 4; word++) begin
        aes_read(8'h30 + word, actual);
        check32("AES ciphertext word", actual,
                aes_ciphertext[block & 3][word]);
        fold(actual);
      end
      transactions++;
    end
    aes_read(8'h09, status);
    check32("AES ready", status & 1, 1);
  endtask

  task automatic run_fcs();
    logic [31:0] expected;
    logic [63:0] data;
    int unsigned length;
    int unsigned bytes;
    int unsigned beat;
    reset_dut();
    for (int unsigned packet = 0; packet < iterations; packet++) begin
      length = frame_length(packet);
      expected = 32'hffff_ffff;
      beat = 0;
      for (int unsigned offset = 0; offset < length; offset += 8) begin
        bytes = (length - offset < 8) ? length - offset : 8;
        data = '0;
        for (int unsigned lane = 0; lane < bytes; lane++) begin
          data[lane * 8 +: 8] = frame_byte(packet, offset + lane);
          expected = crc32_byte(expected, data[lane * 8 +: 8]);
        end
        if (((packet + beat) % 17) == 0) begin
          @(negedge clk);
          fcs_tvalid = 1'b0;
          @(posedge clk);
        end
        @(negedge clk);
        fcs_tdata = data;
        fcs_tkeep = (9'b1 << bytes) - 1;
        fcs_tlast = offset + bytes == length;
        fcs_tvalid = 1'b1;
        @(posedge clk);
        beat++;
      end
      #1ps;
      check32("Ethernet FCS", fcs_result, ~expected);
      fold(fcs_result);
      transactions++;
    end
    fcs_tvalid = 1'b0;
    fcs_tlast = 1'b0;
  endtask

  initial begin
    firmware = '{
      32'h1000_2083, 32'h1234_5137, 32'h6781_0113, 32'h00d1_1193,
      32'h0031_4133, 32'h0111_5193, 32'h0031_4133, 32'h0051_1193,
      32'h0031_4133, 32'hfff0_8093, 32'hfe00_92e3, 32'h1000_0237,
      32'h0022_2023, 32'h0000_006f
    };
    aes_key = '{
      32'h2b7e_1516, 32'h28ae_d2a6, 32'habf7_1588, 32'h09cf_4f3c,
      0, 0, 0, 0
    };
    aes_plaintext = '{
      '{32'h6bc1_bee2, 32'h2e40_9f96, 32'he93d_7e11, 32'h7393_172a},
      '{32'hae2d_8a57, 32'h1e03_ac9c, 32'h9eb7_6fac, 32'h45af_8e51},
      '{32'h30c8_1c46, 32'ha35c_e411, 32'he5fb_c119, 32'h1a0a_52ef},
      '{32'hf69f_2445, 32'hdf4f_9b17, 32'had2b_417b, 32'he66c_3710}
    };
    aes_ciphertext = '{
      '{32'h3ad7_7bb4, 32'h0d7a_3660, 32'ha89e_caf3, 32'h2466_ef97},
      '{32'hf5d3_d585, 32'h03b9_699d, 32'he785_895a, 32'h96fd_baaf},
      '{32'h43b1_cd7f, 32'h598e_ce23, 32'h881b_00e3, 32'hed03_0688},
      '{32'h7b0c_785e, 32'h27e8_ad3f, 32'h8223_2071, 32'h0472_5dd4}
    };

    clk = 1'b0;
    iterations = 100;
    transactions = 0;
    checks = 0;
    sim_cycles = 0;
    checksum = 32'h811c_9dc5;
    failures = 0;
    initialize_inputs();
    void'($value$plusargs("OPEN_CORE_BENCH_ITERS=%d", iterations));

    case (`OPEN_CORE_WORKLOAD)
      0: begin
        workload = "picorv32_firmware";
        run_picorv32();
      end
      1: begin
        workload = "secworks_aes128";
        run_aes();
      end
      2: begin
        workload = "ethernet_fcs64";
        run_fcs();
      end
      default: $fatal(1, "invalid OPEN_CORE_WORKLOAD");
    endcase

    $display("OPEN_CORE_BENCH_RESULT mode=pure_sv workload=%s iterations=%0d transactions=%0d checks=%0d sim_cycles=%0d checksum=%0d failures=%0d",
             workload, iterations, transactions, checks, sim_cycles, checksum,
             failures);
    if (failures != 0) $fatal(1, "open-core pure-SV benchmark failed");
    $finish;
  end

  initial begin
    #60s;
    $fatal(1, "open-core pure-SV benchmark timed out");
  end

  open_cores_benchmark_dut dut (.*);
endmodule
