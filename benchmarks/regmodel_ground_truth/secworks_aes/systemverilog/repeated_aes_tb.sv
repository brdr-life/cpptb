`timescale 1ns/1ps
`default_nettype none

module secworks_aes_repeated_tb;
  localparam time CLK_PERIOD = 2ns;

  localparam logic [7:0] ADDR_CTRL    = 8'h08;
  localparam logic [7:0] ADDR_CONFIG  = 8'h0a;
  localparam logic [7:0] ADDR_KEY0    = 8'h10;
  localparam logic [7:0] ADDR_BLOCK0  = 8'h20;
  localparam logic [7:0] ADDR_RESULT0 = 8'h30;

  logic        clk = 0;
  logic        reset_n = 1;
  logic        cs = 0;
  logic        we = 0;
  logic [7:0]  address = 0;
  logic [31:0] write_data = 0;
  logic [31:0] read_data;

  integer unsigned repeats;
  integer unsigned cases;
  integer unsigned checks;
  integer unsigned failures;
  logic [31:0] checksum;

  aes dut (
    .clk,
    .reset_n,
    .cs,
    .we,
    .address,
    .write_data,
    .read_data
  );

  always #(CLK_PERIOD / 2) clk = !clk;

  task automatic write_word(
    input logic [7:0] register_address,
    input logic [31:0] value
  );
    address = register_address;
    write_data = value;
    cs = 1;
    we = 1;
    #(2 * CLK_PERIOD);
    cs = 0;
    we = 0;
  endtask

  task automatic read_word(
    input logic [7:0] register_address,
    output logic [31:0] value
  );
    address = register_address;
    cs = 1;
    we = 0;
    #(CLK_PERIOD);
    value = read_data;
    cs = 0;
  endtask

  task automatic initialize_key(
    input logic [255:0] key,
    input logic aes256
  );
    for (integer word = 0; word < 8; ++word)
      write_word(ADDR_KEY0 + word[7:0], key[255 - word * 32 -: 32]);
    write_word(ADDR_CONFIG, aes256 ? 32'h2 : 32'h0);
    write_word(ADDR_CTRL, 32'h1);
    #(100 * CLK_PERIOD);
  endtask

  task automatic process_block(
    input logic [127:0] input_block,
    input logic aes256,
    input logic encipher,
    output logic [127:0] output_block
  );
    logic [31:0] value;
    for (integer word = 0; word < 4; ++word)
      write_word(ADDR_BLOCK0 + word[7:0],
                 input_block[127 - word * 32 -: 32]);
    write_word(ADDR_CONFIG, {30'b0, aes256, encipher});
    write_word(ADDR_CTRL, 32'h2);
    #(100 * CLK_PERIOD);
    for (integer word = 0; word < 4; ++word) begin
      read_word(ADDR_RESULT0 + word[7:0], value);
      output_block[127 - word * 32 -: 32] = value;
    end
  endtask

  task automatic fold_result(input logic [127:0] result);
    for (integer word = 0; word < 4; ++word)
      checksum = (checksum ^ result[127 - word * 32 -: 32]) * 32'h01000193;
  endtask

  task automatic run_case(
    input integer unsigned suite,
    input integer unsigned case_id,
    input logic [255:0] key,
    input logic aes256,
    input logic encipher,
    input logic [127:0] input_block,
    input logic [127:0] expected
  );
    logic [127:0] actual;
    initialize_key(key, aes256);
    process_block(input_block, aes256, encipher, actual);
    ++cases;
    checks += 4;
    fold_result(actual);
    if (actual !== expected) begin
      ++failures;
      $error("AES case %0d mismatch: actual=%032x expected=%032x",
             case_id, actual, expected);
    end
    if (repeats == 1)
      $display("AES_CASE suite=%0d id=%0d result=%032x",
               suite, case_id, actual);
  endtask

  task automatic run_suite(input integer unsigned suite);
    localparam logic [255:0] AES128_KEY1 =
        256'h2b7e151628aed2a6abf7158809cf4f3c00000000000000000000000000000000;
    localparam logic [255:0] AES128_KEY2 =
        256'h000102030405060708090a0b0c0d0e0f00000000000000000000000000000000;
    localparam logic [255:0] AES256_KEY1 =
        256'h603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4;
    localparam logic [255:0] AES256_KEY2 =
        256'h000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f;

    run_case(suite, 1, AES128_KEY1, 0, 1,
             128'h6bc1bee22e409f96e93d7e117393172a,
             128'h3ad77bb40d7a3660a89ecaf32466ef97);
    run_case(suite, 2, AES128_KEY1, 0, 1,
             128'hae2d8a571e03ac9c9eb76fac45af8e51,
             128'hf5d3d58503b9699de785895a96fdbaaf);
    run_case(suite, 3, AES128_KEY1, 0, 1,
             128'h30c81c46a35ce411e5fbc1191a0a52ef,
             128'h43b1cd7f598ece23881b00e3ed030688);
    run_case(suite, 4, AES128_KEY1, 0, 1,
             128'hf69f2445df4f9b17ad2b417be66c3710,
             128'h7b0c785e27e8ad3f8223207104725dd4);
    run_case(suite, 5, AES128_KEY1, 0, 0,
             128'h3ad77bb40d7a3660a89ecaf32466ef97,
             128'h6bc1bee22e409f96e93d7e117393172a);
    run_case(suite, 6, AES128_KEY1, 0, 0,
             128'hf5d3d58503b9699de785895a96fdbaaf,
             128'hae2d8a571e03ac9c9eb76fac45af8e51);
    run_case(suite, 7, AES128_KEY1, 0, 0,
             128'h43b1cd7f598ece23881b00e3ed030688,
             128'h30c81c46a35ce411e5fbc1191a0a52ef);
    run_case(suite, 8, AES128_KEY1, 0, 0,
             128'h7b0c785e27e8ad3f8223207104725dd4,
             128'hf69f2445df4f9b17ad2b417be66c3710);
    run_case(suite, 9, AES128_KEY2, 0, 1,
             128'h00112233445566778899aabbccddeeff,
             128'h69c4e0d86a7b0430d8cdb78070b4c55a);
    run_case(suite, 10, AES128_KEY2, 0, 0,
             128'h69c4e0d86a7b0430d8cdb78070b4c55a,
             128'h00112233445566778899aabbccddeeff);

    run_case(suite, 16, AES256_KEY1, 1, 1,
             128'h6bc1bee22e409f96e93d7e117393172a,
             128'hf3eed1bdb5d2a03c064b5a7e3db181f8);
    run_case(suite, 17, AES256_KEY1, 1, 1,
             128'hae2d8a571e03ac9c9eb76fac45af8e51,
             128'h591ccb10d410ed26dc5ba74a31362870);
    run_case(suite, 18, AES256_KEY1, 1, 1,
             128'h30c81c46a35ce411e5fbc1191a0a52ef,
             128'hb6ed21b99ca6f4f9f153e7b1beafed1d);
    run_case(suite, 19, AES256_KEY1, 1, 1,
             128'hf69f2445df4f9b17ad2b417be66c3710,
             128'h23304b7a39f9f3ff067d8d8f9e24ecc7);
    run_case(suite, 20, AES256_KEY1, 1, 0,
             128'hf3eed1bdb5d2a03c064b5a7e3db181f8,
             128'h6bc1bee22e409f96e93d7e117393172a);
    run_case(suite, 21, AES256_KEY1, 1, 0,
             128'h591ccb10d410ed26dc5ba74a31362870,
             128'hae2d8a571e03ac9c9eb76fac45af8e51);
    run_case(suite, 22, AES256_KEY1, 1, 0,
             128'hb6ed21b99ca6f4f9f153e7b1beafed1d,
             128'h30c81c46a35ce411e5fbc1191a0a52ef);
    run_case(suite, 23, AES256_KEY1, 1, 0,
             128'h23304b7a39f9f3ff067d8d8f9e24ecc7,
             128'hf69f2445df4f9b17ad2b417be66c3710);
    run_case(suite, 24, AES256_KEY2, 1, 1,
             128'h00112233445566778899aabbccddeeff,
             128'h8ea2b7ca516745bfeafc49904b496089);
    run_case(suite, 25, AES256_KEY2, 1, 0,
             128'h8ea2b7ca516745bfeafc49904b496089,
             128'h00112233445566778899aabbccddeeff);
  endtask

  initial begin
    repeats = 1;
    void'($value$plusargs("AES_REGMODEL_REPEATS=%d", repeats));
    if (repeats == 0)
      $fatal(1, "AES_REGMODEL_REPEATS must be at least one");

    cases = 0;
    checks = 0;
    failures = 0;
    checksum = 32'h811c9dc5;
    reset_n = 0;
    #(2 * CLK_PERIOD);
    reset_n = 1;

    for (integer unsigned suite = 0; suite < repeats; ++suite)
      run_suite(suite);

    $display(
      "SV_AES_REGMODEL_RESULT suites=%0d cases=%0d checks=%0d checksum=%08x failures=%0d",
      repeats, cases, checks, checksum, failures
    );
    if (failures != 0)
      $fatal(1, "secworks AES repeated test failed");
    $finish;
  end
endmodule

`default_nettype wire
