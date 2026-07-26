// rvmodel_macros.h -- riscv-arch-test target definitions for Ibex Simple System
//
// The suite's normal flow builds each test twice: once with -DSIGNATURE to run
// on a reference model and capture a golden signature, then again with
// -DRVTEST_SELFCHECK and that signature linked in, so the program checks itself
// on the DUT. That needs a reference model (upstream uses Sail).
//
// This port asks a different question. It runs the same test binary under two
// harnesses driving the same Ibex RTL -- the upstream Verilator harness and the
// cpptb port -- and requires them to agree. That needs no reference model, but
// it does need the signature to come back out of each simulation, and the
// upstream harness has no memory-dump option: MemArea::Read() exists in
// vendor/lowrisc_ip/dv/verilator/cpp/mem_area.h but nothing on the command line
// reaches it.
//
// So the program reports its own signature. RVMODEL_HALT_PASS and
// RVMODEL_HALT_FAIL are target-owned extension points, and Simple System has a
// character-output register, so the halt path digests the signature region and
// prints it. Both harnesses already forward that register to their log, so the
// same binary yields comparable evidence under either one with no modification
// to the suite, to Ibex, or to either harness.
//
// What this does and does not establish: agreement means the two harnesses
// drove the RTL identically -- same reset, same memory image, same clocking,
// same run length. It says nothing about whether Ibex is architecturally
// correct, which is what a reference model would tell you and what Ibex's own
// CI already checks.
//
// SPDX-License-Identifier: BSD-3-Clause

#ifndef _RVMODEL_MACROS_H
#define _RVMODEL_MACROS_H

// examples/sw/simple_system/common/simple_system_regs.h
#define IBEX_SIM_CTRL_BASE 0x20000
#define IBEX_SIM_CTRL_OUT  0x0  // byte written here appears on stdout and in the log
#define IBEX_SIM_CTRL_CTRL 0x8  // write 1 to bit 0 to halt the simulation
#define IBEX_TIMER_BASE    0x30000

// No tohost/fromhost pair. Those exist so Spike can be told to stop; this port
// halts through the Simple System control register instead.
#define RVMODEL_DATA_SECTION

// M-mode and the standard machine CSRs are implemented, so the default boot
// sequence in tests/env/rvtest_setup.h applies unchanged.
#define STANDARD_SM_SUPPORTED

// RVMODEL_BOOT is left undefined: the memory needs no bring-up, and firmware is
// already resident before the core leaves reset under both harnesses.

// RVMODEL_ACCESS_FAULT_ADDRESS is left undefined. Simple System's bus returns
// data for unmapped addresses rather than signalling an error, so there is no
// address that reliably faults and the access-fault tests are skipped.

///////////////////////////////////////////////////////////////////////////////
// Character output
///////////////////////////////////////////////////////////////////////////////

#define RVMODEL_IO_WRITE_STR(_R1, _R2, _R3, _STR_PTR) \
1:                                       ;\
  lbu  _R1, 0(_STR_PTR)                  ;\
  beqz _R1, 3f                           ;\
2:                                       ;\
  li   _R2, IBEX_SIM_CTRL_BASE           ;\
  sw   _R1, IBEX_SIM_CTRL_OUT(_R2)       ;\
  addi _STR_PTR, _STR_PTR, 1             ;\
  j    1b                                ;\
3:

///////////////////////////////////////////////////////////////////////////////
// Signature digest
///////////////////////////////////////////////////////////////////////////////
//
// Everything below is plain RV32I. The tests are built at the -march their own
// header declares, and most of the applicable subset is rv32i_zicsr_zifencei
// with no M extension, so the digest cannot use `mul`. The mixing step is a
// xor, a rotate and an add, which needs only shifts.
//
// The digest is not cryptographic and does not need to be. It exists to detect
// divergence between two runs of the same binary, where any difference at all
// is a failure, so the only property required is that unequal signature regions
// almost never collide. When one does mismatch, the cpptb side can read the
// whole region through the memory backdoor and diff it word by word; see
// testbench.cpp. The word count is printed alongside so that a region of the
// wrong length is distinguishable from one of the wrong content.
//
// Local labels are numeric so the macros can be used in both the pass and fail
// paths without duplicate symbols.

// Emit one character held in _CH.
#define RVMODEL_PUT_CHAR(_CH, _TMP)       \
  li   _TMP, IBEX_SIM_CTRL_BASE          ;\
  sw   _CH, IBEX_SIM_CTRL_OUT(_TMP)

// Emit _V as eight lowercase hex digits, most significant first.
#define RVMODEL_PUT_HEX32(_V, _NIB, _SHIFT, _TMP) \
  li   _SHIFT, 28                        ;\
90:                                      ;\
  srl  _NIB, _V, _SHIFT                  ;\
  andi _NIB, _NIB, 0xf                   ;\
  li   _TMP, 10                          ;\
  blt  _NIB, _TMP, 91f                   ;\
  addi _NIB, _NIB, 'a' - 10              ;\
  j    92f                               ;\
91:                                      ;\
  addi _NIB, _NIB, '0'                   ;\
92:                                      ;\
  RVMODEL_PUT_CHAR(_NIB, _TMP)           ;\
  addi _SHIFT, _SHIFT, -4                ;\
  bgez _SHIFT, 90b

// Digest rvtest_sig_begin..rvtest_sig_end into t2, counting words into t3.
//
//   h = 0x811c9dc5                       (FNV-1a's offset basis, reused here
//                                         only as a non-zero starting value)
//   for each word w:  h = rotl(h ^ w, 7) + w
#define RVMODEL_SIG_DIGEST                \
  la   t0, rvtest_sig_begin              ;\
  la   t1, rvtest_sig_end                ;\
  li   t2, 0x811c9dc5                    ;\
  li   t3, 0                             ;\
94:                                      ;\
  bgeu t0, t1, 95f                       ;\
  lw   t4, 0(t0)                         ;\
  xor  t2, t2, t4                        ;\
  slli t5, t2, 7                         ;\
  srli t6, t2, 25                        ;\
  or   t2, t5, t6                        ;\
  add  t2, t2, t4                        ;\
  addi t0, t0, 4                         ;\
  addi t3, t3, 1                         ;\
  j    94b                               ;\
95:

// Print "\nACT-SIG <digest> <words>\n". t2 and t3 hold the digest and count and
// must survive the hex printing, which is why it borrows a0-a2 instead.
#define RVMODEL_REPORT_SIG                \
  RVMODEL_SIG_DIGEST                     ;\
  .pushsection .rodata                   ;\
  .balign 4                              ;\
96:                                      ;\
  .asciz "\nACT-SIG "                    ;\
  .popsection                            ;\
  la   a0, 96b                           ;\
  RVMODEL_IO_WRITE_STR(a1, a2, s0, a0)   ;\
  RVMODEL_PUT_HEX32(t2, a0, a1, a2)      ;\
  li   a0, ' '                           ;\
  RVMODEL_PUT_CHAR(a0, a1)               ;\
  RVMODEL_PUT_HEX32(t3, a0, a1, a2)      ;\
  li   a0, '\n'                          ;\
  RVMODEL_PUT_CHAR(a0, a1)

///////////////////////////////////////////////////////////////////////////////
// Termination
///////////////////////////////////////////////////////////////////////////////
//
// The store to IBEX_SIM_CTRL_CTRL halts the simulation a few cycles later
// (shared/rtl/sim/simulator_ctrl.sv counts to two before stopping), so the
// trailing branch is what the core executes in the meantime. It is not dead
// code: without it the core would run off into whatever follows.

#define RVMODEL_HALT_PASS                 \
  RVMODEL_REPORT_SIG                     ;\
  .pushsection .rodata                   ;\
  .balign 4                              ;\
97:                                      ;\
  .asciz "ACT-RESULT PASS\n"             ;\
  .popsection                            ;\
  la   a0, 97b                           ;\
  RVMODEL_IO_WRITE_STR(a1, a2, s0, a0)   ;\
  li   t0, IBEX_SIM_CTRL_BASE            ;\
  li   t1, 1                             ;\
  sw   t1, IBEX_SIM_CTRL_CTRL(t0)        ;\
98:                                      ;\
  j    98b

#define RVMODEL_HALT_FAIL                 \
  RVMODEL_REPORT_SIG                     ;\
  .pushsection .rodata                   ;\
  .balign 4                              ;\
99:                                      ;\
  .asciz "ACT-RESULT FAIL\n"             ;\
  .popsection                            ;\
  la   a0, 99b                           ;\
  RVMODEL_IO_WRITE_STR(a1, a2, s0, a0)   ;\
  li   t0, IBEX_SIM_CTRL_BASE            ;\
  li   t1, 1                             ;\
  sw   t1, IBEX_SIM_CTRL_CTRL(t0)        ;\
89:                                      ;\
  j    89b

///////////////////////////////////////////////////////////////////////////////
// Machine timer
///////////////////////////////////////////////////////////////////////////////
//
// Simple System has a timer at 0x30000 with the mtime/mtimecmp layout the suite
// expects. Declaring it is honest -- the peripheral is real -- but no test in
// the applicable subset uses it.

#define RVMODEL_MTIME_ADDRESS    (IBEX_TIMER_BASE + 0x0)
#define RVMODEL_MTIMECMP_ADDRESS (IBEX_TIMER_BASE + 0x8)

#define RVMODEL_INTERRUPT_LATENCY 10
#define RVMODEL_TIMER_INT_SOON_DELAY 100
#define RVMODEL_MAX_CYCLES_PER_TIMER_TICK 1

///////////////////////////////////////////////////////////////////////////////
// Interrupts
///////////////////////////////////////////////////////////////////////////////
//
// Simple System drives the core's irq_external_i and irq_software_i pins from
// nothing: there is no interrupt-generator peripheral, unlike the cv32e20
// testbench the suite ships a config for. The macros must exist for
// check_defines.h to pass, but there is no way to implement them, so they are
// empty and any test that waits on an interrupt would hang.
//
// That is why build_tests.py excludes interrupt tests by name rather than
// letting them run into the cycle limit. Making these work would mean adding a
// peripheral to the RTL, which is a change to the design under test and out of
// scope for a port whose point is that both harnesses drive the same design.

#define RVMODEL_SET_MEXT_INT(_R1, _R2)
#define RVMODEL_CLR_MEXT_INT(_R1, _R2)
#define RVMODEL_SET_MSW_INT(_R1, _R2)
#define RVMODEL_CLR_MSW_INT(_R1, _R2)

// No supervisor mode, so these are unreachable as well as unimplementable.
#define RVMODEL_SET_SEXT_INT(_R1, _R2)
#define RVMODEL_CLR_SEXT_INT(_R1, _R2)
#define RVMODEL_SET_SSW_INT(_R1, _R2)
#define RVMODEL_CLR_SSW_INT(_R1, _R2)

#endif  // _RVMODEL_MACROS_H
