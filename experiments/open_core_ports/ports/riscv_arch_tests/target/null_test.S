// null_test.S -- a program that does nothing, to measure what a run costs
//
// The 96 architectural tests are all close to the same length: between 182,000
// and 205,000 cycles, a spread of 1.13x. That is far too narrow to separate
// fixed per-run cost from per-cycle cost by fitting a line through them --
// extrapolating back to zero from a 13% spread produces a negative intercept,
// which is a statement about the conditioning of the fit and not about the
// simulator.
//
// So this measures the intercept directly. It halts immediately, so essentially
// all of its wall time is what a run costs before any simulation happens:
// starting the process, constructing the model, loading the program, and
// releasing reset. Subtracting it from a real test's time leaves the part that
// scales with cycles.
//
// It deliberately goes through the same pipeline as every other test -- same
// linker script, same vector table, same halt path, same ACT-SIG line, same
// comparison in run_suite.py -- so it measures this port's per-run cost rather
// than the cost of some other, simpler thing. Its signature region is empty, so
// the digest is the initial value over zero words, which both harnesses and the
// backdoor read agree on for the same reason they agree everywhere else.
//
// SPDX-License-Identifier: BSD-3-Clause

#include "rvmodel_macros.h"

  .section .text.init
  .globl rvtest_entry_point
rvtest_entry_point:
  j halt

  .section .text.rvmodel
halt:
  RVMODEL_HALT_PASS

  // An empty signature region. RVMODEL_SIG_DIGEST walks from rvtest_sig_begin
  // to rvtest_sig_end and stops immediately, reporting the digest's starting
  // value and a count of zero.
  .section .data
  .balign 16
  .globl rvtest_sig_begin
rvtest_sig_begin:
  .globl rvtest_sig_end
rvtest_sig_end:

#ifdef NULL_TEST_PAD
  // Built a second time with this set to the size of a real test's image.
  //
  // Halting immediately measures process start, model construction and reset,
  // but not the cost of getting the program into memory, and that part is not
  // negligible or equal between the harnesses: a real test is about 165kB,
  // which cpptb writes through the memory backdoor a word at a time while
  // upstream parses an ELF through libelf and the simutil_memload DPI export.
  //
  // The difference between this variant and the bare one is that loading cost,
  // measured rather than assumed. The padding is a distinctive constant so a
  // hexdump of the memory shows plainly that it arrived.
  .balign 16
null_test_padding:
  .fill NULL_TEST_PAD, 1, 0xa5
#endif
