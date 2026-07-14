module heavy_benchmark_sv_tb;
  timeunit 1ns;
  timeprecision 1ps;

  logic clk;
  logic rst_n;
  logic fir_in_valid;
  logic fir_in_ready;
  logic signed [15:0] fir_in_sample;
  logic fir_out_valid;
  logic fir_out_ready;
  logic signed [31:0] fir_out_result;
  logic [31:0] fir_sample_count;
  logic crc_in_valid;
  logic crc_in_ready;
  logic [7:0] crc_in_data;
  logic crc_in_last;
  logic crc_out_valid;
  logic crc_out_ready;
  logic [31:0] crc_out_result;
  logic [31:0] crc_packet_count;
  logic mat_load_valid;
  logic mat_load_ready;
  logic mat_load_select;
  logic [3:0] mat_load_index;
  logic signed [15:0] mat_load_data;
  logic mat_start;
  logic mat_out_valid;
  logic mat_out_ready;
  logic [3:0] mat_out_index;
  logic signed [31:0] mat_out_data;
  logic [31:0] mat_block_count;

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

  function automatic integer signed fir_coefficient(input integer tap);
    return ((tap * 7) % 19) - 9;
  endfunction

  function automatic logic signed [15:0] fir_sample(input int unsigned index);
    return stimulus(index)[15:0];
  endfunction

  function automatic logic [7:0] packet_byte(
      input int unsigned packet,
      input int unsigned offset
  );
    return stimulus(packet * 96 + offset)[7:0];
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

  function automatic logic signed [15:0] matrix_value(
      input int unsigned block,
      input int unsigned matrix,
      input int unsigned index
  );
    int signed value;
    value = ((stimulus(block * 32 + matrix * 16 + index) >> 8) & 32'h7ff) -
            1024;
    return value[15:0];
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
      $display("HEAVY_BENCH_MISMATCH mode=pure_sv workload=%s label=%s actual=0x%08x expected=0x%08x",
               workload, label, actual, expected);
  endtask

  task automatic fold(input logic [31:0] value);
    checksum = (checksum ^ value) * 32'h0100_0193;
  endtask

  task automatic reset_dut();
    rst_n = 1'b0;
    fir_in_valid = 1'b0;
    fir_in_sample = '0;
    fir_out_ready = 1'b1;
    crc_in_valid = 1'b0;
    crc_in_data = '0;
    crc_in_last = 1'b0;
    crc_out_ready = 1'b1;
    mat_load_valid = 1'b0;
    mat_load_select = 1'b0;
    mat_load_index = '0;
    mat_load_data = '0;
    mat_start = 1'b0;
    mat_out_ready = 1'b1;
    repeat (4) @(posedge clk);
    rst_n = 1'b1;
  endtask

  task automatic run_fir();
    logic signed [15:0] history [0:31];
    logic signed [15:0] sample;
    longint signed expected;
    history = '{default: '0};
    for (int unsigned iteration = 0; iteration < iterations; iteration++) begin
      sample = fir_sample(iteration);
      expected = sample * fir_coefficient(0);
      for (int tap = 1; tap < 32; tap++)
        expected += history[tap - 1] * fir_coefficient(tap);
      for (int tap = 31; tap > 0; tap--) history[tap] = history[tap - 1];
      history[0] = sample;

      @(negedge clk);
      fir_in_sample = sample;
      fir_in_valid = 1'b1;
      @(posedge clk);
      #1ps;
      check32("FIR result", fir_out_result, expected[31:0]);
      fold(fir_out_result);
      transactions++;
    end
    fir_in_valid = 1'b0;
    check32("FIR accepted sample count", fir_sample_count, iterations);
  endtask

  task automatic run_crc();
    logic [31:0] expected;
    logic [7:0] data;
    int unsigned length;
    for (int unsigned packet = 0; packet < iterations; packet++) begin
      expected = 32'hffff_ffff;
      length = 32 + (packet & 63);
      for (int unsigned offset = 0; offset < length; offset++) begin
        data = packet_byte(packet, offset);
        expected = crc32_byte(expected, data);
        @(negedge clk);
        crc_in_data = data;
        crc_in_last = offset + 1 == length;
        crc_in_valid = 1'b1;
        @(posedge clk);
      end
      #1ps;
      check32("packet CRC32", crc_out_result, ~expected);
      fold(crc_out_result);
      transactions++;
    end
    crc_in_valid = 1'b0;
    crc_in_last = 1'b0;
    check32("CRC packet count", crc_packet_count, iterations);
  endtask

  task automatic load_matrix_value(
      input bit select,
      input int unsigned index,
      input logic signed [15:0] value
  );
    while (!mat_load_ready) begin
      @(posedge clk);
      #1ps;
    end
    @(negedge clk);
    mat_load_select = select;
    mat_load_index = index[3:0];
    mat_load_data = value;
    mat_load_valid = 1'b1;
    @(posedge clk);
  endtask

  task automatic run_matrix();
    logic signed [15:0] matrix_a [0:15];
    logic signed [15:0] matrix_b [0:15];
    longint signed expected;
    for (int unsigned block = 0; block < iterations; block++) begin
      for (int unsigned index = 0; index < 16; index++) begin
        matrix_a[index] = matrix_value(block, 0, index);
        load_matrix_value(1'b0, index, matrix_a[index]);
      end
      for (int unsigned index = 0; index < 16; index++) begin
        matrix_b[index] = matrix_value(block, 1, index);
        load_matrix_value(1'b1, index, matrix_b[index]);
      end

      @(negedge clk);
      mat_load_valid = 1'b0;
      mat_start = 1'b1;
      @(posedge clk);
      @(negedge clk);
      mat_start = 1'b0;

      for (int unsigned output_index = 0; output_index < 16;
           output_index++) begin
        @(posedge clk);
        #1ps;
        expected = 0;
        for (int element = 0; element < 4; element++)
          expected += matrix_a[(output_index / 4) * 4 + element] *
                      matrix_b[element * 4 + output_index % 4];
        check32("matrix output index", mat_out_index, output_index);
        check32("matrix output data", mat_out_data, expected[31:0]);
        fold(mat_out_data);
      end
      transactions++;
    end
    check32("matrix block count", mat_block_count, iterations);
  endtask

  initial begin
    clk = 1'b0;
    iterations = 1000;
    workload = "streaming_fir";
    transactions = 0;
    checks = 0;
    sim_cycles = 0;
    checksum = 32'h811c_9dc5;
    failures = 0;
    void'($value$plusargs("HEAVY_BENCH_ITERS=%d", iterations));
    void'($value$plusargs("HEAVY_BENCH_WORKLOAD=%s", workload));
    reset_dut();

    case (workload)
      "streaming_fir": run_fir();
      "packet_crc32": run_crc();
      "matrix4x4": run_matrix();
      default: $fatal(1, "unknown HEAVY_BENCH_WORKLOAD=%s", workload);
    endcase

    $display("HEAVY_BENCH_RESULT mode=pure_sv workload=%s iterations=%0d transactions=%0d checks=%0d sim_cycles=%0d checksum=%0d failures=%0d",
             workload, iterations, transactions, checks, sim_cycles, checksum,
             failures);
    if (failures != 0) $fatal(1, "heavy pure-SV benchmark failed");
    $finish;
  end

  initial begin
    #10s;
    $fatal(1, "heavy pure-SV benchmark timed out");
  end

  heavy_benchmark_dut dut (.*);
endmodule
