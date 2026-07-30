// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Structural equivalent of dv/uvm/core_ibex/tb/core_ibex_tb_top.sv with the
// UVM removed.
//
// Upstream's top instantiates ibex_top_tracing and wires it to eight
// SystemVerilog interfaces that the UVM agents drive and monitor. This module
// instantiates the same core with the same parameters and lifts the same
// signals to its own boundary, so that testbench.cpp drives and samples them
// where the agents drive and sample the interfaces:
//
//   ibex_mem_intf instr_mem_vif    -> instr_*
//   ibex_mem_intf data_mem_vif     -> data_*
//   core_ibex_dut_probe_if dut_if  -> fetch_enable_i, debug_req_i,
//                                     double_fault_seen_o, the three alerts,
//                                     core_sleep_o, wb_exception_o
//   core_ibex_rvfi_if rvfi_if      -> rvfi_*
//   core_ibex_instr_monitor_if     -> instr_mon_*
//   core_ibex_csr_if csr_if        -> csr_*
//   core_ibex_ifetch_if            -> ifetch_*
//   core_ibex_ifetch_pmp_if        -> ifetch_pmp_*
//   push_pull_if scrambling_key_if -> scramble_*
//   irq_if irq_vif                 -> irq_*
//
// Deliberate differences from core_ibex_tb_top.sv, all of them consequences of
// the port's scope or of cpptb's transport:
//
//  * The memory integrity bits are computed here rather than driven. The UVM
//    response sequence writes
//
//        {req.intg, req.data} = prim_secded_pkg::prim_secded_inv_39_32_enc(req.data);
//
//    and then inverts req.intg when it wants a bad-integrity response. That is
//    exactly the two lines below per bus: the testbench drives the 32 data bits
//    and one `bad_intg` bit, and the encoder upstream uses computes the rest.
//    Reimplementing a SECDED parity matrix in C++ would be a second copy of
//    something the design already has.
//
//  * Ports wider than 32 bits are `bit` rather than `logic`. cpptb's generated
//    transport carries a port wider than 32 bits only when it is two-state and
//    not an inout. That covers the scrambling key and nonce, rvfi_order,
//    rvfi_ext_mcycle and the two performance-counter vectors, none of which
//    carries meaning beyond its value.
//
//  * rvfi_ext_mhpmcounters and rvfi_ext_mhpmcountersh are packed vectors here
//    and unpacked arrays in ibex_top_tracing. cpptb carries packed vectors.
//    Each counter is 32 bits and lands on a 32-bit boundary of the flat vector,
//    so testbench.cpp reads counter i as word i and no arithmetic is involved.
//
//  * `mcounteren_writable_i` is an input rather than an undriven interface
//    signal. Nothing in core_ibex_base_test drives dut_if.mcounteren_writable,
//    so on a 4-state simulator the core sees X and on Verilator it sees zero;
//    the baseline this port is compared against is the Verilator one, so the
//    testbench holds it at zero to match, and core_ibex_mcounteren_lock_test
//    drives it as that class does.

module core_ibex_cpptb_tb_top import ibex_pkg::*; #(
  // The parameter set core_ibex_tb_top.sv declares, in the same order and with
  // the same defaults. cpptb.toml overrides them from util/ibex_config.py, which
  // is the same tool ports/core_ibex_uvm's build_tb.py asks.
  parameter bit          PMPEnable        = 1'b0,
  parameter int unsigned PMPGranularity   = 0,
  parameter int unsigned PMPNumRegions    = 4,
  parameter int unsigned MHPMCounterNum   = 0,
  parameter int unsigned MHPMCounterWidth = 40,
  parameter bit          RV32E            = 1'b0,
  parameter bit          BranchTargetALU  = 1'b0,
  parameter bit          WritebackStage   = 1'b0,
  parameter bit          ICache           = 1'b0,
  parameter bit          ICacheECC        = 1'b0,
  parameter bit          ICacheTweakInfection = 1'b0,
  parameter bit          BranchPredictor  = 1'b0,
  parameter bit          SecureIbex       = 1'b0,
  parameter int unsigned LockstepOffset   = 1,
  parameter bit          ICacheScramble   = 1'b0,
  parameter bit          DbgTriggerEn     = 1'b0,
  // core_ibex_tb_top.sv takes these five from `define+ macros that
  // scripts/compile_tb.py supplies. They are parameters here, with the values
  // build_tb.py's ADDRESS_DEFINES gives, because the debug-module range has to
  // agree with the one Spike is told about and both are fixed rather than
  // chosen. ResetVec is BootAddr + 0x80.
  parameter int unsigned DmBaseAddr       = 32'h1A11_0000,
  parameter int unsigned DmAddrMask       = 32'h0000_0FFF,
  parameter int unsigned DmHaltAddr       = 32'h8000_0000,
  parameter int unsigned DmExceptionAddr  = 32'h8000_0008,
  parameter int unsigned BootAddr         = 32'h8000_0000,
  // The number of performance counters ibex_top_tracing exports on RVFI. It is
  // fixed at 10 in the RTL and in ibex_rvfi_monitor.sv's loop; naming it here
  // makes the two flat vectors below self-describing.
  localparam int unsigned NumPerfCounters = 10
) (
  input  logic        clk_i,
  input  logic        rst_ni,

  // Instruction memory, ibex_mem_intf instr_mem_vif
  output logic        instr_req_o,
  output logic [31:0] instr_addr_o,
  input  logic        instr_gnt_i,
  input  logic        instr_rvalid_i,
  input  logic [31:0] instr_rdata_i,
  input  logic        instr_bad_intg_i,
  input  logic        instr_err_i,

  // Functional coverage, core_ibex_fcov_if. That interface binds into the core
  // and reads these hierarchically; the port samples them at the boundary
  // instead, so the covergroup in fcov.hpp sees what uarch_cg sees.
  output logic [3:0]  fcov_controller_fsm_o,
  output logic [1:0]  fcov_priv_mode_id_o,
  output logic [1:0]  fcov_priv_mode_lsu_o,
  output logic        fcov_mprv_o,
  output logic        fcov_debug_mode_o,
  output logic        fcov_debug_req_o,
  output logic        fcov_data_ind_timing_o,
  output logic        fcov_dummy_instr_en_o,
  output logic        fcov_ls_error_exception_o,
  output logic        fcov_ls_pmp_exception_o,
  output logic        fcov_branch_taken_o,
  output logic        fcov_branch_not_taken_o,
  output logic        fcov_irq_pending_o,
  output logic        fcov_wb_reg_no_load_hz_o,
  output logic        fcov_dummy_instr_if_o,
  output logic        fcov_dummy_instr_id_o,
  output logic        fcov_dummy_instr_wb_o,

  // Interrupts, irq_if irq_vif. Nothing in core_ibex_base_test raises one, so
  // these are zero for the whole of every run this port covers, and cpptb's
  // default for an unwritten input is zero. They are lifted rather than tied
  // because the interrupt agent samples them every cycle, which is work the UVM
  // environment does on every test and a comparison has to include.
  input  logic        irq_software_i,
  input  logic        irq_timer_i,
  input  logic        irq_external_i,
  input  logic [14:0] irq_fast_i,
  input  logic        irq_nm_i,

  // Data memory, ibex_mem_intf data_mem_vif
  output logic        data_req_o,
  output logic [31:0] data_addr_o,
  output logic        data_we_o,
  output logic [3:0]  data_be_o,
  output logic [31:0] data_wdata_o,
  input  logic        data_gnt_i,
  input  logic        data_rvalid_i,
  input  logic [31:0] data_rdata_i,
  input  logic        data_bad_intg_i,
  input  logic        data_err_i,

  // The four dside fields ibex_mem_intf carries that are not bus pins:
  // core_ibex_tb_top.sv assigns them from inside the load/store unit and the
  // cosim scoreboard passes them to notify_dside_access.
  output logic        data_misaligned_first_o,
  output logic        data_misaligned_second_o,
  output logic        data_misaligned_first_saw_error_o,
  output logic        data_m_mode_access_o,

  // core_ibex_dut_probe_if
  input  logic [3:0]  fetch_enable_i,
  input  logic [3:0]  mcounteren_writable_i,
  input  logic        debug_req_i,
  output logic        double_fault_seen_o,
  output logic        alert_minor_o,
  output logic        alert_major_internal_o,
  output logic        alert_major_bus_o,
  output logic        core_sleep_o,
  output logic        wb_exception_o,

  // push_pull_if scrambling_key_if, in Pull/Device mode upstream
  output logic        scramble_req_o,
  input  logic        scramble_key_valid_i,
  input  bit [SCRAMBLE_KEY_W-1:0]   scramble_key_i,
  input  bit [SCRAMBLE_NONCE_W-1:0] scramble_nonce_i,

  // core_ibex_rvfi_if, restricted to the fields ibex_rvfi_monitor.sv reads
  output logic        rvfi_valid_o,
  output bit   [63:0] rvfi_order_o,
  output logic        rvfi_trap_o,
  output logic [4:0]  rvfi_rd_addr_o,
  output logic [31:0] rvfi_rd_wdata_o,
  output logic [31:0] rvfi_pc_rdata_o,
  output logic [31:0] rvfi_ext_pre_mip_o,
  output logic [31:0] rvfi_ext_post_mip_o,
  output logic        rvfi_ext_nmi_o,
  output logic        rvfi_ext_nmi_int_o,
  output logic        rvfi_ext_debug_req_o,
  output logic        rvfi_ext_rf_wr_suppress_o,
  output bit   [63:0] rvfi_ext_mcycle_o,
  output bit   [NumPerfCounters*32-1:0] rvfi_ext_mhpmcounters_o,
  output bit   [NumPerfCounters*32-1:0] rvfi_ext_mhpmcountersh_o,
  output logic        rvfi_ext_ic_scr_key_valid_o,
  output logic        rvfi_ext_irq_valid_o,

  // core_ibex_instr_monitor_if, restricted to what ibex_cosim_scoreboard reads
  output logic        instr_mon_rvfi_id_done_o,
  output bit   [63:0] instr_mon_rvfi_order_id_o,
  output logic [31:0] instr_mon_pc_id_o,
  output logic        instr_mon_is_compressed_id_o,

  // core_ibex_ifetch_if
  output logic        ifetch_ready_o,
  output logic        ifetch_valid_o,
  output logic [31:0] ifetch_rdata_o,
  output logic [31:0] ifetch_addr_o,
  output logic        ifetch_err_o,
  output logic        ifetch_err_plus2_o,

  // core_ibex_ifetch_pmp_if
  output logic        ifetch_pmp_valid_o,
  output logic [31:0] ifetch_pmp_addr_o,
  output logic        ifetch_pmp_err_o,

  // core_ibex_csr_if, which only core_ibex_mcounteren_lock_test reads
  output logic        csr_access_o,
  output logic [11:0] csr_addr_o,
  output logic [1:0]  csr_op_o
);

  // ---------------------------------------------------------------------
  // Bus integrity, in place of the response sequence's encoder call
  // ---------------------------------------------------------------------
  logic [38:0] instr_rdata_enc, data_rdata_enc;
  logic [6:0]  instr_rdata_intg, data_rdata_intg;

  assign instr_rdata_enc = prim_secded_pkg::prim_secded_inv_39_32_enc(instr_rdata_i);
  assign data_rdata_enc  = prim_secded_pkg::prim_secded_inv_39_32_enc(data_rdata_i);
  // "invert the correct ones, which we know will break things for the codes we
  // use" -- ibex_mem_intf_response_seq.
  assign instr_rdata_intg = instr_bad_intg_i ? ~instr_rdata_enc[38:32]
                                             :  instr_rdata_enc[38:32];
  assign data_rdata_intg  = data_bad_intg_i  ? ~data_rdata_enc[38:32]
                                             :  data_rdata_enc[38:32];

  ibex_top_tracing #(
    .PMPEnable            (PMPEnable           ),
    .PMPGranularity       (PMPGranularity      ),
    .PMPNumRegions        (PMPNumRegions       ),
    .MHPMCounterNum       (MHPMCounterNum      ),
    .MHPMCounterWidth     (MHPMCounterWidth    ),
    .RV32E                (RV32E               ),
    .RV32M                (`IBEX_CFG_RV32M     ),
    .RV32B                (`IBEX_CFG_RV32B     ),
    .RegFile              (`IBEX_CFG_RegFile   ),
    .BranchTargetALU      (BranchTargetALU     ),
    .WritebackStage       (WritebackStage      ),
    .ICache               (ICache              ),
    .ICacheECC            (ICacheECC           ),
    .ICacheTweakInfection (ICacheTweakInfection),
    .SecureIbex           (SecureIbex          ),
    .LockstepOffset       (LockstepOffset      ),
    .ICacheScramble       (ICacheScramble      ),
    .BranchPredictor      (BranchPredictor     ),
    .DbgTriggerEn         (DbgTriggerEn        ),
    .DmBaseAddr           (DmBaseAddr          ),
    .DmAddrMask           (DmAddrMask          ),
    .DmHaltAddr           (DmHaltAddr          ),
    .DmExceptionAddr      (DmExceptionAddr     )
  ) dut (
    .clk_i                     (clk_i                      ),
    .rst_ni                    (rst_ni                     ),

    .test_en_i                 (1'b0                       ),
    .scan_rst_ni               (1'b1                       ),
    .ram_cfg_icache_tag_i      ('{default: prim_ram_1p_pkg::RAM_1P_CFG_REQ_DEFAULT}),
    .ram_cfg_icache_tag_o      (                           ),
    .ram_cfg_icache_data_i     ('{default: prim_ram_1p_pkg::RAM_1P_CFG_REQ_DEFAULT}),
    .ram_cfg_icache_data_o     (                           ),

    .hart_id_i                 (32'b0                      ),
    .boot_addr_i               (BootAddr                   ),

    .instr_req_o               (instr_req_o                ),
    .instr_gnt_i               (instr_gnt_i                ),
    .instr_rvalid_i            (instr_rvalid_i             ),
    .instr_addr_o              (instr_addr_o               ),
    .instr_rdata_i             (instr_rdata_i              ),
    .instr_rdata_intg_i        (instr_rdata_intg           ),
    .instr_err_i               (instr_err_i                ),

    .data_req_o                (data_req_o                 ),
    .data_gnt_i                (data_gnt_i                 ),
    .data_rvalid_i             (data_rvalid_i              ),
    .data_addr_o               (data_addr_o                ),
    .data_we_o                 (data_we_o                  ),
    .data_be_o                 (data_be_o                  ),
    .data_rdata_i              (data_rdata_i               ),
    .data_rdata_intg_i         (data_rdata_intg            ),
    .data_wdata_o              (data_wdata_o               ),
    .data_wdata_intg_o         (                           ),
    .data_err_i                (data_err_i                 ),

    // Lifted to the boundary; zero for every run this port covers, because
    // core_ibex_base_test starts no interrupt sequence.
    .irq_software_i            (irq_software_i             ),
    .irq_timer_i               (irq_timer_i                ),
    .irq_external_i            (irq_external_i             ),
    .irq_fast_i                (irq_fast_i                 ),
    .irq_nm_i                  (irq_nm_i                   ),

    .scramble_key_valid_i      (scramble_key_valid_i       ),
    .scramble_key_i            (scramble_key_i             ),
    .scramble_nonce_i          (scramble_nonce_i           ),
    .scramble_req_o            (scramble_req_o             ),

    .debug_req_i               (debug_req_i                ),
    .crash_dump_o              (                           ),
    .double_fault_seen_o       (double_fault_seen_o        ),

    .fetch_enable_i            (fetch_enable_i             ),
    .mcounteren_writable_i     (mcounteren_writable_i      ),
    .alert_minor_o             (alert_minor_o              ),
    .alert_major_internal_o    (alert_major_internal_o     ),
    .alert_major_bus_o         (alert_major_bus_o          ),
    .core_sleep_o              (core_sleep_o               ),

    .lockstep_cmp_en_o         (                           ),
    .data_req_shadow_o         (                           ),
    .data_we_shadow_o          (                           ),
    .data_be_shadow_o          (                           ),
    .data_addr_shadow_o        (                           ),
    .data_wdata_shadow_o       (                           ),
    .data_wdata_intg_shadow_o  (                           ),

    .instr_req_shadow_o        (                           ),
    .instr_addr_shadow_o       (                           )
  );

  // ---------------------------------------------------------------------
  // The interface assigns from core_ibex_tb_top.sv, unchanged apart from
  // naming an output port where upstream names an interface field.
  //
  // The RVFI signals are not ports of ibex_top_tracing: they are internal
  // wires between the ibex_top instance and the tracer. core_ibex_tb_top.sv
  // reaches them the same way, with `assign rvfi_if.valid = dut.rvfi_valid;`.
  // ---------------------------------------------------------------------
  assign rvfi_valid_o              = dut.rvfi_valid;
  assign rvfi_order_o              = dut.rvfi_order;
  assign rvfi_trap_o               = dut.rvfi_trap;
  assign rvfi_rd_addr_o            = dut.rvfi_rd_addr;
  assign rvfi_rd_wdata_o           = dut.rvfi_rd_wdata;
  assign rvfi_pc_rdata_o           = dut.rvfi_pc_rdata;
  assign rvfi_ext_pre_mip_o        = dut.rvfi_ext_pre_mip;
  assign rvfi_ext_post_mip_o       = dut.rvfi_ext_post_mip;
  assign rvfi_ext_nmi_o            = dut.rvfi_ext_nmi;
  assign rvfi_ext_nmi_int_o        = dut.rvfi_ext_nmi_int;
  assign rvfi_ext_debug_req_o      = dut.rvfi_ext_debug_req;
  assign rvfi_ext_rf_wr_suppress_o = dut.rvfi_ext_rf_wr_suppress;
  assign rvfi_ext_mcycle_o         = dut.rvfi_ext_mcycle;
  assign rvfi_ext_ic_scr_key_valid_o = dut.rvfi_ext_ic_scr_key_valid;
  assign rvfi_ext_irq_valid_o      = dut.rvfi_ext_irq_valid;

  // The two performance-counter arrays are unpacked upstream and packed here,
  // because cpptb's transport carries packed vectors. Each counter is 32 bits
  // and lands on a 32-bit boundary, so testbench.cpp reads counter i as word i.
  for (genvar i = 0; i < NumPerfCounters; i++) begin : gen_perf_counter_flat
    assign rvfi_ext_mhpmcounters_o[i*32 +: 32]  = dut.rvfi_ext_mhpmcounters[i];
    assign rvfi_ext_mhpmcountersh_o[i*32 +: 32] = dut.rvfi_ext_mhpmcountersh[i];
  end

  assign wb_exception_o = dut.u_ibex_top.u_ibex_core.id_stage_i.wb_exception;

  assign instr_mon_rvfi_id_done_o = dut.u_ibex_top.u_ibex_core.rvfi_id_done;
  assign instr_mon_rvfi_order_id_o = dut.u_ibex_top.u_ibex_core.rvfi_stage_order_d;
  assign instr_mon_pc_id_o = dut.u_ibex_top.u_ibex_core.pc_id;
  assign instr_mon_is_compressed_id_o =
    dut.u_ibex_top.u_ibex_core.id_stage_i.instr_is_compressed_i;

  assign ifetch_ready_o     = dut.u_ibex_top.u_ibex_core.if_stage_i.fetch_ready;
  assign ifetch_valid_o     = dut.u_ibex_top.u_ibex_core.if_stage_i.fetch_valid;
  assign ifetch_rdata_o     = dut.u_ibex_top.u_ibex_core.if_stage_i.fetch_rdata;
  assign ifetch_addr_o      = dut.u_ibex_top.u_ibex_core.if_stage_i.fetch_addr;
  assign ifetch_err_o       = dut.u_ibex_top.u_ibex_core.if_stage_i.fetch_err;
  assign ifetch_err_plus2_o = dut.u_ibex_top.u_ibex_core.if_stage_i.fetch_err_plus2;

  assign ifetch_pmp_valid_o = dut.u_ibex_top.u_ibex_core.instr_req_o;
  assign ifetch_pmp_addr_o  = dut.u_ibex_top.u_ibex_core.instr_addr_o;
  assign ifetch_pmp_err_o   = dut.u_ibex_top.u_ibex_core.pmp_req_err[ibex_pkg::PMP_I];

  assign csr_access_o = dut.u_ibex_top.u_ibex_core.csr_access;
  assign csr_addr_o   = dut.u_ibex_top.u_ibex_core.csr_addr;
  assign csr_op_o     = dut.u_ibex_top.u_ibex_core.csr_op;

  // What core_ibex_fcov_if reads for cp_controller_fsm and cp_priv_mode_id.
  // The fcov interface binds into ibex_core and names these directly; the
  // paths here are the same signals from outside.
  // core_ibex_fcov_bind binds core_ibex_fcov_if into ibex_core with `.*`, so
  // everything the covergroup names is either a port of ibex_core or a
  // hierarchical reference relative to it. These are the same names from
  // outside.
  assign fcov_controller_fsm_o =
    dut.u_ibex_top.u_ibex_core.id_stage_i.controller_i.ctrl_fsm_cs;
  assign fcov_priv_mode_id_o   = dut.u_ibex_top.u_ibex_core.priv_mode_id;
  assign fcov_priv_mode_lsu_o  = dut.u_ibex_top.u_ibex_core.priv_mode_lsu;
  assign fcov_mprv_o           =
    dut.u_ibex_top.u_ibex_core.cs_registers_i.mstatus_q.mprv;
  assign fcov_debug_mode_o     = dut.u_ibex_top.u_ibex_core.debug_mode;
  assign fcov_debug_req_o      =
    dut.u_ibex_top.u_ibex_core.id_stage_i.controller_i.fcov_debug_req;
  assign fcov_data_ind_timing_o =
    dut.u_ibex_top.u_ibex_core.cs_registers_i.data_ind_timing_o;
  assign fcov_dummy_instr_en_o =
    dut.u_ibex_top.u_ibex_core.cs_registers_i.dummy_instr_en_o;
  assign fcov_ls_error_exception_o =
    dut.u_ibex_top.u_ibex_core.load_store_unit_i.fcov_ls_error_exception;
  assign fcov_ls_pmp_exception_o =
    dut.u_ibex_top.u_ibex_core.load_store_unit_i.fcov_ls_pmp_exception;
  assign fcov_branch_taken_o =
    dut.u_ibex_top.u_ibex_core.id_stage_i.fcov_branch_taken;
  assign fcov_branch_not_taken_o =
    dut.u_ibex_top.u_ibex_core.id_stage_i.fcov_branch_not_taken;
  assign fcov_irq_pending_o =
    dut.u_ibex_top.u_ibex_core.id_stage_i.irq_pending_i |
    dut.u_ibex_top.u_ibex_core.id_stage_i.irq_nm_i;
  assign fcov_wb_reg_no_load_hz_o =
    dut.u_ibex_top.u_ibex_core.id_stage_i.fcov_rf_rd_wb_hz &&
    !dut.u_ibex_top.u_ibex_core.wb_stage_i.outstanding_load_wb_o;
  assign fcov_dummy_instr_if_o =
    dut.u_ibex_top.u_ibex_core.if_stage_i.fcov_insert_dummy_instr;
  assign fcov_dummy_instr_id_o =
    dut.u_ibex_top.u_ibex_core.if_stage_i.dummy_instr_id_o;
  assign fcov_dummy_instr_wb_o =
    dut.u_ibex_top.u_ibex_core.wb_stage_i.dummy_instr_wb_o;

  assign data_misaligned_first_o =
    dut.u_ibex_top.u_ibex_core.load_store_unit_i.handle_misaligned_d |
    ((dut.u_ibex_top.u_ibex_core.load_store_unit_i.lsu_type_i == 2'b01) &
     (dut.u_ibex_top.u_ibex_core.load_store_unit_i.data_offset == 2'b01));

  assign data_misaligned_second_o =
    dut.u_ibex_top.u_ibex_core.load_store_unit_i.addr_incr_req_o;

  assign data_misaligned_first_saw_error_o =
    dut.u_ibex_top.u_ibex_core.load_store_unit_i.addr_incr_req_o &
    dut.u_ibex_top.u_ibex_core.load_store_unit_i.lsu_err_d;

  assign data_m_mode_access_o =
    dut.u_ibex_top.u_ibex_core.priv_mode_lsu == ibex_pkg::PRIV_LVL_M;

  // core_ibex_tb_top.sv drives this unconditionally, outside the `ifdef
  // INC_ASSERT that declares it, which is why ports/core_ibex_uvm has to
  // overlay prim_assert.sv. Guarding it costs nothing on a simulator that does
  // define INC_ASSERT, and is what upstream would have to write anyway.
`ifdef INC_ASSERT
  if (SecureIbex && LockstepOffset > 1) begin : gen_disable_count_check
    assign dut.u_ibex_top.gen_lockstep.u_ibex_lockstep.gen_reset_counter.u_rst_shadow_cnt.
          unused_assert_connected = 1;
  end
`endif

endmodule
