// Reduced case: a forced signal is read unforced when it is used as an index
// into an unpacked array.
//
// core_ibex_rf_addr_intg_test forces raddr_a_i / raddr_b_i on Ibex's register
// file, which reads `rf_reg[raddr_a_i]`. The force is accepted and reads back,
// but the register file keeps returning the entry the unforced address selects,
// so the test's ECC glitch never happens. This is the same module reduced to
// two reads of one forced signal: one arithmetic, one an array index.
//
//   $ verilator --binary --timing -o vfai --Mdir obj \
//     shims/verilator_force_array_index.sv && ./obj/vfai
//
// On Verilator 5.050 this prints
//
//   base            rdata=13 plus=04
//   forced raddr=7  rdata=13 plus=08     <-- rdata should be 17
//
// The generated C++ shows why: the arithmetic reads raddr_i__VforceRd and the
// array select reads raddr_i. The VPI path (vpi_put_value with vpiForceFlag on
// a forceable signal) behaves the same way, so this is not specific to how UVM
// reaches the signal.
//
// SPDX-License-Identifier: Apache-2.0

module rf (input logic [3:0] raddr_i,
           output logic [7:0] rdata_o,
           output logic [7:0] plus_o);
  logic [7:0] mem [16];
  initial for (int i = 0; i < 16; i++) mem[i] = 8'h10 + i[7:0];
  assign rdata_o = mem[raddr_i];
  assign plus_o  = {4'h0, raddr_i} + 8'h1;
endmodule

module verilator_force_array_index;
  logic [3:0] addr = 4'h3;
  logic [7:0] rdata, plus;

  rf u_rf (.raddr_i(addr), .rdata_o(rdata), .plus_o(plus));

  initial begin
    #10;
    $display("base            rdata=%h plus=%h", rdata, plus);
    force u_rf.raddr_i = 4'h7;
    #10;
    $display("forced raddr=7  rdata=%h plus=%h     (expect rdata=17 plus=08)",
             rdata, plus);
    release u_rf.raddr_i;
    $finish;
  end
endmodule
