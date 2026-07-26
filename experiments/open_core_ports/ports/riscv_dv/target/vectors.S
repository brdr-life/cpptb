// vectors.S -- reset-vector table for riscv-dv programs on Simple System
//
// Same job as ports/riscv_arch_tests/target/vectors.S and for the same two
// reasons, kept separate because that one pulls in the architectural suite's
// model macros.
//
// Ibex takes its reset vector 0x80 bytes into the vector table at boot_addr, so
// the space below _start has to be real content rather than a hole: the
// upstream harness places ELF segments relative to the file's lowest address,
// and a hole at the bottom would load the whole image 0x80 bytes low.
//
// A trap taken before the generated program installs its own mtvec is
// unrecoverable, so each entry halts through the Simple System control register
// rather than running into whatever follows.
//
// SPDX-License-Identifier: Apache-2.0

  .section .text.vectors, "ax", @progbits
  .globl rvdv_vector_table
rvdv_vector_table:
  .rept 32
  j early_trap
  .endr

  # Ibex starts here, 0x80 into the vector table. The generated program's body
  # has to begin 256-byte aligned so its mtvec handler can be too, and it cannot
  # be both that and at the reset vector, so this jumps across the gap.
  .section .text.reset, "ax", @progbits
  .globl rvdv_reset
rvdv_reset:
  j _start

  .section .text.earlytrap, "ax", @progbits
early_trap:
  li   t0, 0x20000          // SIM_CTRL_BASE
  li   t1, 1
  sw   t1, 8(t0)            // SIM_CTRL_CTRL: write 1 to bit 0 to halt
1:
  j    1b
