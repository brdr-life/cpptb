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
//
// The co-simulation configuration turns this off, and not because Ibex lacks
// U-mode. The suite reads U_SUPPORTED as "menvcfg exists" -- see rvtest_setup.h,
// "menvcfg only exists if U-mode is supported" -- and has the boot code write
// menvcfg and, on RV32, menvcfgh. Ibex implements both as read-only zero, which
// is a legal WARL choice, and accepts the writes:
//
//     // menvcfg: machine environment configuration, all zeros for Ibex
//     CSR_MENVCFG, CSR_MENVCFGH: csr_rdata_int = '0;
//
// The Spike on lowRISC's ibex_cosim branch never registers CSR_MENVCFGH at all
// -- riscv/processor.cc adds CSR_MENVCFG to csrmap and has no entry for the
// high half -- so it raises an illegal instruction where Ibex does not, and
// every test dies in common boot code before reaching anything it tests. The
// reference model is the one behind the specification here, not the DUT.
//
// Nothing in the applicable set for that configuration declares U as a required
// extension, so the cost is only that this setup is skipped. It is a limitation
// of co-simulating against this Spike, and it is why the plain and
// co-simulation runs are reported separately rather than as one number.
#ifndef IBEX_NO_U_MODE
#  define U_SUPPORTED
#endif

// Zifencei: Ibex implements fence.i as a pipeline flush.
#define ZIFENCEI_SUPPORTED

// PMP depends on which Ibex configuration is being built, so build_tests.py
// passes it in: 0 for `small` (PMPEnable=0) and 16 for `bmfull`
// (PMPEnable=1, PMPNumRegions=16). The default matches `small` so the header is
// still correct if compiled by hand.
//
// check_defines.h reads the misspelled name as well as the correct one, and
// both must be defined: an undefined macro is zero in `#if`, so leaving the
// misspelled one out would silently disable the check that reads it.
#ifndef IBEX_PMP_ENTRIES
#  define IBEX_PMP_ENTRIES 0
#endif

#define UDB_NUM_PMP_ENTRIES IBEX_PMP_ENTRIES
#define UDB_NUM_PMP_ENTIRES IBEX_PMP_ENTRIES
// Ibex reserves none of its regions, so every implemented entry is usable. The
// suite refuses to run with fewer than 8, which is why `bmfull` uses the
// 16-region configuration rather than the 4-region default.
#define UDB_NUM_USABLE_PMP_ENTRIES IBEX_PMP_ENTRIES

#if IBEX_PMP_ENTRIES > 0
// rtl/ibex_pmp.sv implements all three addressing modes -- PMP_MODE_TOR,
// PMP_MODE_NA4 and PMP_MODE_NAPOT -- and PMPGranularity=0 places no lower bound
// on region size, so NA4 is genuinely available.
#  define UDB_PMP_TOR_SUPPORTED
#  define UDB_PMP_NAPOT_SUPPORTED
#  define UDB_PMP_GRANULARITY 0
#endif

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
