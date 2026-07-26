/* link.ld -- riscv-arch-test layout for Ibex Simple System
 *
 * Derived from config/cores/cve2/cv32e20/link.ld in the pinned riscv-arch-test
 * checkout, with the memory map changed to Simple System's and one addition
 * noted below.
 *
 * examples/simple_system/rtl/ibex_simple_system.sv ties boot_addr_i to
 * 32'h00100000 and instantiates the RAM with .Depth(1024*1024/4), so the core
 * starts at 0x00100000 and memory runs to 0x001FFFFF.
 *
 * Ibex takes its reset vector 0x80 bytes into the vector table at boot_addr,
 * which is why the upstream software link script says
 *
 *     _entry_point = _vectors_start + 0x80;
 *
 * rvtest_entry_point is the first thing in .text.init, and TEST_BASE is where
 * .text.init lands, so TEST_BASE is the reset vector rather than boot_addr.
 *
 * The 0x80 bytes below it are the hardware vector table, supplied by vectors.S.
 * It has to be real content and not a hole: the upstream harness loads ELF
 * segments relative to the lowest address in the file, so a hole at the bottom
 * of the image would be collapsed and everything would land 0x80 bytes low.
 * See vectors.S for the whole story.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

BOOT_ADDR  = 0x00100000;
RESET_VEC  = BOOT_ADDR + 0x80;
RAM_END    = 0x00200000;

TEST_BASE  = RESET_VEC;
NUM_HARTS  = 1;
STACK_SIZE = 0x8000;

OUTPUT_ARCH( "riscv" )
ENTRY(rvtest_entry_point)

PROVIDE(__stack_size = STACK_SIZE);
PROVIDE(__num_harts = NUM_HARTS);

SECTIONS
{
  . = BOOT_ADDR;

  .text.vectors : { KEEP(*(.text.vectors)) }

  ASSERT(. == TEST_BASE,
         "vector table must be exactly 0x80 bytes, up to Ibex's reset vector")

  . = TEST_BASE;

  .text.init   : { *(.text.init) }
  .text.rvtest : { *(.text.rvtest) *(.text.rvtest.*) }

  . = ALIGN(0x4000);
  .rodata : { *(.rodata) *(.rodata.*) *(.srodata) *(.srodata.*) }
  .data   : { *(.data) *(.data.*) *(.sdata) *(.sdata.*) }

  . = ALIGN(16);
  .bss : {
    __bss_start = .;
    *(.sbss) *(.sbss.*) *(.bss) *(.bss.*) *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
  }

  . = ALIGN(16);
  __stack_bottom = .;
  . += __stack_size * __num_harts;
  __stack_top = .;

  . = ALIGN(0x1000);
  .text.rvmodel : {
    *(.text.rvmodel) *(.text.rvmodel.*)
    *(.text.earlytrap)
    *(.text) *(.text.*)
  }

  . = ALIGN(0x1000);
  _end = .;

  /* Not in the upstream script. The signature region is tens of kilobytes of
   * .fill, so overflowing the RAM is a real possibility and a link that
   * silently produced an image the memory cannot hold would show up as a
   * mystery hang rather than a build error. */
  ASSERT(_end <= RAM_END,
         "image overflows Simple System RAM: reduce TRAP_SIGUPD_COUNT")
}
