// rvtest_config.h -- Ibex Simple System, small configuration
//
// Upstream generates this file from a UDB configuration with a Ruby toolchain
// (see framework/src/act/parse_udb_config.py, which shells out to `bundle exec
// udb`). Nothing else in this repository needs Ruby, and the file is small and
// stable, so it is written by hand here and committed. That keeps the port
// buildable with nothing but a RISC-V GCC.
//
// The consequence is that it can drift from what UDB would emit. The macros
// below are the complete set the test environment reads -- `grep -ohE
// '\bUDB_[A-Z0-9_]+|[A-Z0-9_]+_SUPPORTED' tests/env/*.h` in the pinned
// riscv-arch-test checkout returns nothing else -- so drift would show up as a
// missing-macro error rather than a silently wrong configuration.
//
// Every value below is justified against the Ibex RTL at the commit pinned in
// sources.toml, with the parameters ports/ibex_simple_system/cpptb.toml sets.
//
// SPDX-License-Identifier: BSD-3-Clause

#ifndef RVTEST_CONFIG_H
#define RVTEST_CONFIG_H

// rtl/ibex_cs_registers.sv MISA_VALUE, with RV32E=0: I, M, C and U are set;
// A, B, D, F, N and S are not.
#define UDB_MXLEN 32

// U-mode is implemented (MISA bit 20 is hardwired to 1).
#define U_SUPPORTED

// Zifencei: Ibex implements fence.i as a pipeline flush.
#define ZIFENCEI_SUPPORTED

// PMPEnable=0 in the small configuration, so there are no PMP entries and the
// PMP tests do not apply. check_defines.h reads the misspelled name as well as
// the correct one; both are defined so neither spelling picks up a stray zero
// from an undefined macro.
#define UDB_NUM_PMP_ENTRIES 0
#define UDB_NUM_PMP_ENTIRES 0
#define UDB_NUM_USABLE_PMP_ENTRIES 0

// rtl/ibex_cs_registers.sv hardwires mtvec.MODE to 2'b01 and requires BASE to
// be 256-byte aligned:
//
//     // mtvec.MODE set to vectored
//     // mtvec.BASE must be 256-byte aligned
//     mtvec_d = csr_mtvec_init_i ? {boot_addr_i[31:8], 6'b0, 2'b01} : ...
//
// Direct mode is therefore not available, and UDB_MTVEC_MODES_0 is deliberately
// left undefined. The trap handler in tests/env/rvtest_trap_handler.h builds a
// vectored trampoline when only mode 1 is offered.
#define UDB_MTVEC_MODES_1
#define UDB_MTVEC_BASE_ALIGNMENT_VECTORED 256

// Not defined, each for a reason:
//
//   E_SUPPORTED         RV32E=0.
//   F_SUPPORTED         No FPU. Same for D, Q and Zfinx.
//   S_SUPPORTED         MISA bit 18 is 0, so no supervisor mode, no Sv32/Sv39.
//   H_SUPPORTED         No hypervisor extension.
//   ZKR_SUPPORTED       No entropy source, so mseccfg does not exist.
//   SMEPMP_SUPPORTED    Follows from PMPEnable=0. Same for Smmpm.
//   SMRNMI_SUPPORTED    Ibex's NMI is not Smrnmi.
//   SSCOFPMF_SUPPORTED  MHPMCounterNum=0.
//   SMSTATEEN_SUPPORTED No mstateen CSRs.
//   ZICFILP_SUPPORTED   No landing pads.
//   ZVL32B_SUPPORTED    No vector unit, so the ELEN/VLEN/SEW macros and the
//                       indexed load/store width macros are all absent too.

#endif  // RVTEST_CONFIG_H
