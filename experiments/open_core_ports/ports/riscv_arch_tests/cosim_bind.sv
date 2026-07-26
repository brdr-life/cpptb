// cosim_bind.sv -- binds Ibex's co-simulation checker into the design
//
// This replaces dv/verilator/simple_system_cosim/ibex_simple_system_cosim_checker_bind.sv
// and does exactly what it does. The checker itself is used unmodified; only
// the bind statement is rewritten, for one reason.
//
// Upstream writes the parameter overrides in the implicit shorthand:
//
//     bind ibex_simple_system ibex_simple_system_cosim_checker#(
//         .SecureIbex,
//         .ICache,
//         ...
//
// which means `.SecureIbex(SecureIbex)`. Verilator accepts that for parameters.
// cpptb parses the design with slang first, and slang does not:
//
//     ibex_simple_system_cosim_checker_bind.sv:7:18: error: expected '('
//
// Implicit `.name` connection is standard for ports; for parameter overrides it
// is a Verilator extension, so slang is within its rights. Writing them out is
// the whole fix, and it costs nothing but this file.
//
// SPDX-License-Identifier: Apache-2.0

module cosim_bind;
  bind ibex_simple_system ibex_simple_system_cosim_checker #(
      .SecureIbex     (SecureIbex),
      .ICache         (ICache),
      .PMPEnable      (PMPEnable),
      .PMPGranularity (PMPGranularity),
      .PMPNumRegions  (PMPNumRegions),
      .MHPMCounterNum (MHPMCounterNum)
    ) u_ibex_simple_system_cosim_checker_bind (
      .clk_i            (IO_CLK),
      .rst_ni           (IO_RST_N),

      // CoreD is a localparam of ibex_simple_system; a bind's expressions are
      // elaborated in the scope of the instance bound into, so it resolves.
      .host_dmem_req    (host_req[CoreD]),
      .host_dmem_gnt    (host_gnt[CoreD]),
      .host_dmem_we     (host_we[CoreD]),
      .host_dmem_addr   (host_addr[CoreD]),
      .host_dmem_be     (host_be[CoreD]),
      .host_dmem_wdata  (host_wdata[CoreD]),

      .host_dmem_rvalid (host_rvalid[CoreD]),
      .host_dmem_rdata  (host_rdata[CoreD]),
      .host_dmem_err    (host_err[CoreD])
    );
endmodule
