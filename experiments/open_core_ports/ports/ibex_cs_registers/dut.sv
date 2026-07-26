// dut.sv -- ibex_cs_registers with an interface cpptb can drive
//
// This replaces dv/cs_registers/tb/tb_cs_registers.sv, and the difference is
// the whole point of the port. Upstream's wrapper instantiates the same design
// and then carries the testbench inside itself: four DPI calls on every clock
// edge, one each for the environment, the reset driver, the monitor and the
// driver, plus an `always_ff` that assigns the driver's outputs through
// non-blocking assignments to avoid a race with the design's own always_ff
// blocks.
//
// None of that is needed when the testbench can drive ports directly. What is
// left is a wrapper that exists only to name the interface, because
// ibex_cs_registers has far more ports than this testbench uses -- upstream
// waives PINMISSING for the same reason -- and cpptb generates its interface
// from the top module's ports.
//
// The enum-typed ports are exposed as plain vectors and cast here. The C++ side
// deals in uint32_t for the operation and address, exactly as upstream's DPI
// does, so widening them at the boundary would add a conversion nobody wants.
//
// SPDX-License-Identifier: Apache-2.0

module cs_registers_dut #(
    parameter bit               DbgTriggerEn     = 1'b0,
    parameter bit               ICache           = 1'b0,
    parameter int unsigned      MHPMCounterNum   = 8,
    parameter int unsigned      MHPMCounterWidth = 40,
    parameter bit               PMPEnable        = 1'b0,
    parameter int unsigned      PMPGranularity   = 0,
    parameter int unsigned      PMPNumRegions    = 4,
    parameter bit               RV32E            = 1'b0
) (
    input  logic        clk_i,
    input  logic        rst_ni,

    // The register interface, SRAM-like. Driven a transaction at a time.
    input  logic        csr_access_i,
    input  logic [11:0] csr_addr_i,
    input  logic [31:0] csr_wdata_i,
    input  logic [1:0]  csr_op_i,
    input  logic        csr_op_en_i,

    output logic [31:0] csr_rdata_o,
    output logic        illegal_csr_insn_o
);

  // RV32M and RV32B are enum-valued parameters, so they arrive as defines
  // rather than as -G, matching how every other port here passes them.
  /* verilator lint_off PINMISSING */
  ibex_cs_registers #(
    .DbgTriggerEn     (DbgTriggerEn),
    .ICache           (ICache),
    .MHPMCounterNum   (MHPMCounterNum),
    .MHPMCounterWidth (MHPMCounterWidth),
    .PMPEnable        (PMPEnable),
    .PMPGranularity   (PMPGranularity),
    .PMPNumRegions    (PMPNumRegions),
    .RV32E            (RV32E),
    .RV32M            (`RV32M),
    .RV32B            (`RV32B)
  ) i_cs_regs (
    .clk_i              (clk_i),
    .rst_ni             (rst_ni),
    .csr_access_i       (csr_access_i),
    .csr_addr_i         (ibex_pkg::csr_num_e'(csr_addr_i)),
    .csr_wdata_i        (csr_wdata_i),
    .csr_op_i           (ibex_pkg::csr_op_e'(csr_op_i)),
    .csr_op_en_i        (csr_op_en_i),
    .csr_rdata_o        (csr_rdata_o),
    .illegal_csr_insn_o (illegal_csr_insn_o)
  );
  /* verilator lint_on PINMISSING */

endmodule
