// vectors.S -- reset-vector table for Ibex Simple System
//
// Ibex initialises mtvec from boot_addr_i at reset, in vectored mode:
//
//     // mtvec.MODE set to vectored
//     // mtvec.BASE must be 256-byte aligned
//     mtvec_d = csr_mtvec_init_i ? {boot_addr_i[31:8], 6'b0, 2'b01} : ...
//
// so the 0x80 bytes from boot_addr up to the reset vector are a trap vector
// table, and the core begins executing at boot_addr + 0x80. The architectural
// tests install their own mtvec during boot, so this table is only live for the
// short window before that happens.
//
// It exists as real content rather than as a hole in the address space for two
// reasons.
//
// The first is that the upstream harness would otherwise load the image at the
// wrong address. Its ELF loader places segments relative to the lowest physical
// address in the file, not at their absolute addresses:
//
//     uint32_t off = phdr.p_paddr - low;
//         -- vendor/lowrisc_ip/dv/verilator/cpp/dpi_memutil.cc
//
// With the table absent the lowest address is the reset vector, so the whole
// image lands 0x80 bytes low, the core fetches the wrong instruction at reset,
// traps, and loops in the trap handler forever. That is the bug this file was
// written to fix, and it is invisible in the ELF: only a trace shows it.
//
// The second is that a trap taken before the tests install their handler would
// otherwise execute whatever the memory happened to contain. Making each entry
// jump somewhere deliberate turns that into a labelled halt, which the suite
// runner reports as a failed test in about a millisecond, rather than a run
// that wanders off and is eventually killed by the cycle limit.
//
// SPDX-License-Identifier: BSD-3-Clause

#include "rvmodel_macros.h"

  .section .text.vectors, "ax", @progbits
  .globl rvtest_vector_table
rvtest_vector_table:
  // 32 entries of four bytes each fills exactly the 0x80 bytes between
  // boot_addr and the reset vector. link.ld asserts the size.
  .rept 32
  j early_trap
  .endr

  .section .text.earlytrap, "ax", @progbits
early_trap:
  // Reached only if the core traps before the test's own handler is installed,
  // which no passing run does. Announce it and stop; the message is what
  // distinguishes this from a test that merely produced a wrong signature.
  la   a0, early_trap_message
  RVMODEL_IO_WRITE_STR(t1, t2, t3, a0)
  li   t0, IBEX_SIM_CTRL_BASE
  li   t1, 1
  sw   t1, IBEX_SIM_CTRL_CTRL(t0)
1:
  j    1b

  .section .rodata
  .balign 4
early_trap_message:
  .asciz "\nACT-RESULT EARLY-TRAP\n"
