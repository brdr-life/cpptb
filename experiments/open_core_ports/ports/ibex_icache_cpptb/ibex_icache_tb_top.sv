// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Structural equivalent of dv/uvm/icache/dv/tb/tb.sv with the UVM removed.
//
// ibex_icache cannot be elaborated on its own: it has tag and data RAM ports
// that something has to answer, and a scrambling key port that something has
// to service. Upstream's tb.sv wires those to two prim_ram_1p_scr instances
// per way and to a push-pull agent. This module does the same, and lifts the
// core-side and memory-side pins plus the key handshake to the boundary so
// that the cpptb testbench drives them the way the UVM agents drive the
// interfaces.
//
// Differences from tb.sv, all of them deliberate:
//
//  * the parameter override is spelled TweakInfection. tb.sv writes
//    .ICacheTweakInfection, which ibex_icache does not have, so tb.sv cannot
//    elaborate as vendored on any simulator. ports/ibex_icache_uvm patches it.
//  * ibex_icache_ram_if is not instantiated. It exists to corrupt RAM read
//    data for ibex_icache_ecc, which this port does not run, and its
//    monitor-side clocking block and SVA belong to the UVM environment.
//  * the key/nonce/valid triple is an input rather than the d_data field of a
//    push_pull_if. The key device is a coroutine in testbench.cpp.
//
// The scramble key latch, the request/valid handshake around it and both RAM
// instantiations are copied from tb.sv unchanged apart from the signal names.

module ibex_icache_tb_top import ibex_pkg::*; #(
  parameter bit ICacheECC      = 1'b1,
  parameter bit TweakInfection = 1'b1
) (
  input  logic                           clk_i,
  input  logic                           rst_ni,

  // Core side (ibex_icache_core_if)
  input  logic                           req_i,
  input  logic                           branch_i,
  input  logic [31:0]                    branch_addr_i,
  input  logic                           ready_i,
  output logic                           valid_o,
  output logic [31:0]                    rdata_o,
  output logic [31:0]                    addr_o,
  output logic                           err_o,
  output logic                           err_plus2_o,
  input  logic                           enable_i,
  input  logic                           invalidate_i,
  output logic                           busy_o,

  // Memory side (ibex_icache_mem_if)
  output logic                           instr_req_o,
  output logic [31:0]                    instr_addr_o,
  input  logic                           instr_gnt_i,
  input  logic                           instr_rvalid_i,
  input  logic [31:0]                    instr_rdata_i,
  input  logic                           instr_err_i,

  // Scrambling key device (push_pull_if in Pull/Device mode upstream)
  output logic                           scr_key_req_o,
  input  logic                           scr_key_valid_i,
  // Two-state: cpptb's wide-signal transport carries bit vectors, and these
  // two carry no meaning beyond being random bits.
  input  bit   [SCRAMBLE_KEY_W-1:0]      scr_key_i,
  input  bit   [SCRAMBLE_NONCE_W-1:0]    scr_nonce_i,

  output logic                           ecc_error_o
);

  localparam int unsigned BusSizeECC       = ICacheECC ? (BUS_SIZE + 7) : BUS_SIZE;
  localparam int unsigned LineSizeECC      = BusSizeECC * IC_LINE_BEATS;
  localparam int unsigned TagSizeECC       = ICacheECC ? (IC_TAG_SIZE + 6) : IC_TAG_SIZE;
  localparam int unsigned NumAddrScrRounds = 2;

  // RAM wiring, in place of ibex_icache_ram_if
  logic [IC_INDEX_W-1:0]  ic_tag_addr;
  logic                   ic_tag_write;
  logic [IC_NUM_WAYS-1:0] ic_tag_req;
  logic [TagSizeECC-1:0]  ic_tag_wdata;
  logic [TagSizeECC-1:0]  ic_tag_rdata [IC_NUM_WAYS];
  logic [IC_NUM_WAYS-1:0] ic_data_req;
  logic                   ic_data_write;
  logic [IC_INDEX_W-1:0]  ic_data_addr;
  logic [LineSizeECC-1:0] ic_data_wdata;
  logic [LineSizeECC-1:0] ic_data_rdata [IC_NUM_WAYS];

  // Scramble key state, as in tb.sv
  logic [SCRAMBLE_KEY_W-1:0]   scramble_key_q, scramble_key_d;
  logic [SCRAMBLE_NONCE_W-1:0] scramble_nonce_q, scramble_nonce_d;
  logic                        scramble_key_valid_d, scramble_key_valid_q;
  logic                        scramble_req_d, scramble_req_q;

  ibex_icache #(
      .ICacheECC      (ICacheECC),
      .TweakInfection (TweakInfection),
      .BusSizeECC     (BusSizeECC),
      .TagSizeECC     (TagSizeECC),
      .LineSizeECC    (LineSizeECC)
  ) dut (
      .clk_i               ( clk_i                ),
      .rst_ni              ( rst_ni               ),

      .req_i               ( req_i                ),

      .branch_i            ( branch_i             ),
      .addr_i              ( branch_addr_i        ),

      .ready_i             ( ready_i              ),
      .valid_o             ( valid_o              ),
      .rdata_o             ( rdata_o              ),
      .addr_o              ( addr_o               ),
      .err_o               ( err_o                ),
      .err_plus2_o         ( err_plus2_o          ),
      .icache_enable_i     ( enable_i             ),
      .icache_inval_i      ( invalidate_i         ),
      .busy_o              ( busy_o               ),

      .instr_req_o         ( instr_req_o          ),
      .instr_addr_o        ( instr_addr_o         ),
      .instr_gnt_i         ( instr_gnt_i          ),
      .instr_rvalid_i      ( instr_rvalid_i       ),
      .instr_rdata_i       ( instr_rdata_i        ),
      .instr_err_i         ( instr_err_i          ),

      .ic_tag_req_o        ( ic_tag_req           ),
      .ic_tag_write_o      ( ic_tag_write         ),
      .ic_tag_addr_o       ( ic_tag_addr          ),
      .ic_tag_wdata_o      ( ic_tag_wdata         ),
      .ic_tag_rdata_i      ( ic_tag_rdata         ),
      .ic_data_req_o       ( ic_data_req          ),
      .ic_data_write_o     ( ic_data_write        ),
      .ic_data_addr_o      ( ic_data_addr         ),
      .ic_data_wdata_o     ( ic_data_wdata        ),
      .ic_data_rdata_i     ( ic_data_rdata        ),
      .ic_scr_key_valid_i  ( scramble_key_valid_q ),
      .ic_scr_key_req_o    (                      ),

      .ecc_error_o         ( ecc_error_o          )
  );

  // Scramble key valid starts with OTP returning new valid key and stays high
  // until we request a new valid key.
  assign scramble_key_valid_d = scramble_req_q  ? scr_key_valid_i :
                                invalidate_i    ? 1'b0            :
                                                  scramble_key_valid_q;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      scramble_key_q   <= 128'hDDDDDDDDEEEEEEEEAAAAAAAADDDDDDDD;
      scramble_nonce_q <= 64'hBBBBEEEEEEEEFFFF;
    end else if (scr_key_valid_i && scramble_req_q) begin
      scramble_key_q   <= scr_key_i;
      scramble_nonce_q <= scr_nonce_i;
    end
  end

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      scramble_key_valid_q <= 1'b1;
      scramble_req_q       <= 1'b0;
    end else begin
      scramble_key_valid_q <= scramble_key_valid_d;
      scramble_req_q       <= scramble_req_d;
    end
  end

  // Scramble key request starts with the invalidate signal and stays high
  // until we get a valid key.
  assign scramble_req_d = scramble_req_q ? ~scr_key_valid_i : invalidate_i;
  assign scr_key_req_o  = scramble_req_q;

  assign scramble_key_d   = scramble_key_q;
  assign scramble_nonce_d = scramble_nonce_q;

  for (genvar way = 0; way < IC_NUM_WAYS; way++) begin : gen_rams
    prim_ram_1p_scr #(
      .Width            (TagSizeECC),
      .Depth            (IC_NUM_LINES),
      .DataBitsPerMask  (TagSizeECC),
      .EnableParity     (0),
      .NumAddrScrRounds (NumAddrScrRounds)
    ) tag_bank (
      .clk_i            (clk_i),
      .rst_ni           (rst_ni),

      .key_valid_i      (scramble_key_valid_q),
      .key_i            (scramble_key_q),
      .nonce_i          (scramble_nonce_q),

      .req_i            (ic_tag_req[way]),

      .gnt_o            (),
      .write_i          (ic_tag_write),
      .addr_i           (ic_tag_addr),
      .wdata_i          (ic_tag_wdata),
      .wmask_i          ({TagSizeECC{1'b1}}),
      .intg_error_i     (1'b0),

      .rdata_o          (ic_tag_rdata[way]),
      .rvalid_o         (),
      .raddr_o          (),
      .rerror_o         (),
      .cfg_i            ('{default: prim_ram_1p_pkg::RAM_1P_CFG_REQ_DEFAULT}),
      .cfg_o            (),
      .wr_collision_o   (),
      .write_pending_o  (),
      .alert_o          ()
    );

    prim_ram_1p_scr #(
      .Width              (LineSizeECC),
      .Depth              (IC_NUM_LINES),
      .DataBitsPerMask    (LineSizeECC),
      .EnableParity       (0),
      .ReplicateKeyStream (1),
      .NumAddrScrRounds   (NumAddrScrRounds)
    ) data_bank (
      .clk_i            (clk_i),
      .rst_ni           (rst_ni),

      .key_valid_i      (scramble_key_valid_q),
      .key_i            (scramble_key_q),
      .nonce_i          (scramble_nonce_q),

      .req_i            (ic_data_req[way]),

      .gnt_o            (),
      .write_i          (ic_data_write),
      .addr_i           (ic_data_addr),
      .wdata_i          (ic_data_wdata),
      .wmask_i          ({LineSizeECC{1'b1}}),
      .intg_error_i     (1'b0),

      .rdata_o          (ic_data_rdata[way]),
      .rvalid_o         (),
      .raddr_o          (),
      .rerror_o         (),
      .cfg_i            ('{default: prim_ram_1p_pkg::RAM_1P_CFG_REQ_DEFAULT}),
      .cfg_o            (),
      .wr_collision_o   (),
      .write_pending_o  (),
      .alert_o          ()
    );
  end

endmodule
