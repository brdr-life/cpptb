/* link.ld -- riscv-dv generated programs on Ibex Simple System
 *
 * examples/simple_system/rtl/ibex_simple_system.sv ties boot_addr_i to
 * 32'h00100000 and instantiates the RAM with .Depth(1024*1024/4), so the core
 * starts at 0x00100000 and memory runs to 0x001FFFFF. Ibex takes its reset
 * vector 0x80 bytes into the vector table at boot_addr, which is why _start is
 * placed there and vectors.S fills the space below it.
 *
 * Ibex ships dv/uvm/core_ibex/riscv_dv_extension/ddm_link.ld for these
 * programs, but it targets core_ibex's map at 0x80000000. This is the same
 * shape against Simple System's.
 *
 * The one line that makes the whole adaptation work is `tohost`. A generated
 * program ends with
 *
 *     li gp, 1
 *     ecall
 *     write_tohost:
 *       sw gp, tohost, t5
 *
 * which is HTIF, the mechanism Spike stops on. Simple System's control register
 * halts the simulation when bit 0 of SIM_CTRL_CTRL is written, and Ibex's own
 * examples/sw/simple_system/common/link.ld already exploits that:
 *
 *     tohost   = 0x20008;
 *
 * so the generated exit sequence halts the simulator with no change to the
 * program and no trap handler of ours. fromhost is never read here and is
 * pointed at spare RAM so a stray reference links.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

OUTPUT_ARCH( "riscv" )
ENTRY(_start)

BOOT_ADDR = 0x00100000;
RESET_VEC = BOOT_ADDR + 0x80;
RAM_END   = 0x00200000;

/* simple_system_regs.h: SIM_CTRL_BASE + SIM_CTRL_CTRL. Must be 8-byte aligned. */
tohost   = 0x20008;

SECTIONS
{
  . = BOOT_ADDR;

  .text.vectors : { KEEP(*(.text.vectors)) }

  ASSERT(. == RESET_VEC,
         "vector table must be exactly 0x80 bytes, up to Ibex's reset vector")

  . = RESET_VEC;

  .text.reset : { KEEP(*(.text.reset)) }

  /* Ibex hardwires mtvec.MODE to vectored and masks BASE to a 256-byte
   * boundary, so the generated mtvec_handler must land on one. Its `.align`
   * is relative to the section, and .text starts at the reset vector, which is
   * not 256-aligned; aligning the output section is what makes the generator's
   * alignment mean anything. Without this, traps enter part-way through the
   * program and it wanders into unwritten memory after about a million
   * instructions. */
  . = ALIGN(256);
  .text : {
    *(.text.init)
    *(.text)
    *(.text.*)
    *(.text.earlytrap)
  }

  . = ALIGN(0x1000);
  .rodata : { *(.rodata) *(.rodata.*) *(.srodata) *(.srodata.*) }

  /* riscv-dv emits its data pages, stacks and PMP regions as named sections.
   * Listing them explicitly rather than globbing keeps an unexpected new one a
   * link error instead of silently landing wherever the linker chose. */
  . = ALIGN(0x1000);
  .data : {
    *(.data) *(.data.*) *(.sdata) *(.sdata.*)
    *(.user_stack)
    *(.kernel_stack)
    *(.region_0) *(.region_1)
    *(.s_region_0) *(.s_region_1)
  }

  . = ALIGN(16);
  .bss : {
    __bss_start = .;
    *(.sbss) *(.sbss.*) *(.bss) *(.bss.*) *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
  }

  . = ALIGN(16);
  _end = .;

  /* Never read; present so a reference links. */
  fromhost = RAM_END - 0x10;

  ASSERT(_end <= RAM_END,
         "image overflows Simple System RAM: reduce instr_cnt or data pages")
}
