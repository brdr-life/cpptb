# riscv-dv random programs on cpptb

Flow 5's stimulus without UVM: riscv-dv generates random RISC-V programs, and
they run through the co-simulation harness this repository already has, with
Spike checking every retired instruction on both harnesses.

**Status: working end to end.** Random programs generate, build, run and
terminate, under both harnesses, with Spike checking every retired instruction
and no mismatches.

| | upstream | cpptb |
| --- | ---: | ---: |
| `gen_0` | 590 cycles | 273 instructions matched |
| `gen_1` | 630 cycles | 239 instructions matched |
| `gen_2` | 531 cycles | 219 instructions matched |

The instruction counts are well below the 300 requested per program because
riscv-dv's random control flow reaches `test_done` early; that is the
generator's behaviour, not a truncated run.

## Why this exists

`dv/uvm/core_ibex` is Ibex's real verification depth, and porting it whole means
porting a UVM environment. The local matrix in `experiments/uvm_comparison`,
measured on Verilator 5.050, found that UVM builds and executes there --
`data1_test` passes 3/3 and `data0_test` 2/3 -- but that the two random tests
report scoreboard errors. Constrained-random is the part that does not work yet,
and it is the part riscv-dv depends on.

riscv-dv ships `pyflow`, a pure-Python generator with a `pyvsc` constraint
solver, which sidesteps that entirely: no SystemVerilog simulator is involved in
generation. The generated programs can then be run by
`ports/riscv_arch_tests`'s co-simulation harness, which already loads an
arbitrary program through the memory backdoor and runs it with Spike in
lockstep on both harnesses.

So the split is:

| | | |
| --- | --- | --- |
| **E1**, here | generate with pyflow, run through the existing harness | no UVM |
| **E2** | port the `dv/uvm/core_ibex` environment: interrupt and debug stimulus, the RVFI scoreboard, functional coverage | needs UVM |

## Generating

```sh
python3 generate.py --count 10 --instructions 400
```

Three things were needed to make the vendored generator run, and
`generate.py` encodes all of them:

- **Python 3.11.** `pygen_src/isa/riscv_instr.py` does `from imp import reload`
  and `imp` was removed in 3.12. uv provisions 3.11 rather than this repository
  changing its interpreter or the fetched tree being patched.
- **`--num_of_sub_program 0`.** Anything else reaches `gen_callstack`, which
  fails on current pyvsc with `'riscv_asm_program_gen' object has no attribute
  'callstack_gen'`; the vendored generator expects an older solver. Sub-programs
  are call-stack stress rather than instruction coverage, so this costs less
  than pinning an old pyvsc would.
- **`PYTHONPATH` at `pygen/`**, not the riscv-dv root, since the package is
  `pygen_src`.

The generator's dependencies are supplied by uv at run time and are not
installed into this project.

## Building and running

```sh
python3 generate.py --count 3 --instructions 300
python3 build_programs.py
```

Then any program through the co-simulation harness:

```sh
ACT_FIRMWARE=build/elf/gen_0.vmem ACT_CYCLE_LIMIT=5000000 \
  ../../work/riscv_arch_tests_cosim/cpptb/riscv_arch_tests_cosim/obj/Vdpi_riscv_arch_tests_cosim
```

Termination needed no adaptation, which was the pleasant surprise. Programs end
with

    li gp, 1
    ecall
    write_tohost:
      sw gp, tohost, t5

which is HTIF, and Simple System's control register *is* the tohost address, as
Ibex's own `examples/sw/simple_system/common/link.ld` already exploits. So
`tohost = 0x20008;` in `target/link.ld` is the whole of it.

### The trap vector has to be 256-byte aligned

This is the whole of the target adaptation, and it took two goes.

Ibex hardwires `mtvec.MODE` to vectored and masks `BASE` to a 256-byte boundary.
The generated program sets its own vector with `la x27, mtvec_handler; ori x27,
x27, 1; csrw mtvec, x27`, so if the handler is not on such a boundary Ibex
silently drops the low bits and traps enter part-way through the program.

The symptom is a program that executes correctly for about a million
instructions, matching Spike the whole way, and then wanders into unwritten
memory and traps on `c.unimp` forever.

`generate.py` passes `--tvec_alignment 8`, but that alone is not enough and is
not even reliable: in `riscv_instr_gen_config.py` the constraint is
`vsc.soft(self.tvec_alignment == ...)`, so the solver may ignore it, and it
does. Some programs came out aligned by luck and others emitted `.align 2`,
which is why one of three terminated and two did not.

So `build_programs.py` rewrites that directive to `.align 8` before assembling,
which is reliable where the flag is not, and edits a generated file under
`build/` rather than the fetched tree. `target/link.ld` then aligns the program
body to 256 bytes, because the generator's `.align` is relative to its section
and `.text` would otherwise start at the reset vector, which is not aligned.
`target/vectors.S` supplies a `.text.reset` trampoline so the body can be
aligned while Ibex still starts at `boot_addr + 0x80`.

All three are needed together.

## E2, started: asynchronous interrupt stimulus

The capability E1 cannot have, because the stimulus comes from outside the
program. `ports/riscv_arch_tests/testbench.cpp` gained a spawned coroutine that
forces `irq_external_i`, off unless `ACT_IRQ_PERIOD` is set:

```sh
ACT_IRQ_PERIOD=150 ACT_IRQ_COUNT=20 ACT_FIRMWARE=build/elf/gen_0.vmem \
  ../../work/riscv_arch_tests_cosim_small/.../Vdpi_riscv_arch_tests_cosim_small
```

`ibex_simple_system.sv` ties the interrupt pins to constants, so they are forced
rather than driven. Nothing has to tell Spike about it: Ibex exports the pin
state on `rvfi_ext_pre_mip`/`rvfi_ext_post_mip` and the checker already forwards
those with `riscv_cosim_set_mip`.

It demonstrably works. The same program, generated with `--interrupts`:

| | instructions matched | mismatches |
| --- | ---: | ---: |
| no stimulus | 274 | 0 |
| 20 interrupts, every 150 cycles | 1,046,965 | 0 |

The interrupts are being taken, the program services them, and Spike stays in
lockstep across a million instructions. **That is the thing E1 could not do.**

### Why it does not terminate, and why that is not a small fix

The run reaches the cycle limit rather than the program's exit sequence. The
cause is now known and it is not a tuning problem.

The core loops inside `mmode_intr_handler`, ending at its `mret` and
immediately re-entering. The obvious explanation is the pin still being
asserted at `mret`, and that is wrong: shortening the assertion to one cycle
changes nothing, and the instruction count is identical at `ACT_IRQ_HOLD=1` and
`2`. The handler cannot complete on its own.

riscv-dv's external-interrupt handler expects a way to identify and clear the
interrupt source. Ibex's own flow provides that from the UVM environment's
interrupt agent. Simple System has no interrupt controller at all -- it ties
`irq_external_i` to a constant -- so there is nothing for the handler to talk
to, and it spins.

So external interrupt stimulus needs more than a forced pin, in the same way
debug stimulus needs more than a forced `debug_req_i`: both want platform
support Simple System does not have. That is a property of the platform this
port targets, not of cpptb, and it is the real content of E2.

Two ways forward, neither a tweak:

- **Use the timer interrupt.** Simple System *does* have a timer with
  mtime/mtimecmp, and `irq_timer_i` is already driven by it rather than tied
  off. riscv-dv's timer handler clears the interrupt by writing `mtimecmp`,
  which needs no new device. This is the cheap experiment and should be tried
  first.
- **Model an interrupt controller.** Either as RTL beside the design or in the
  testbench, answering the claim and complete accesses the handler makes. More
  faithful to core_ibex, and considerably more work.

What is established either way is that the stimulus reaches the core and the
reference model follows it: 20 interrupts taken, over a million instructions
matched, zero mismatches. The gap is the platform, not the mechanism.

## The upstream side now exists

This port was written without an upstream side to compare against, because
upstream's flow 5 is UVM and nothing local could run it. `ports/core_ibex_uvm`
now does: Ibex's own `dv/uvm/core_ibex` testbench builds and runs on Verilator
5.050, executes riscv-dv programs, and co-simulates against Spike.

Both harnesses currently run riscv-dv stimulus with Spike checking every
retired instruction, and both report no mismatches:

| harness | program | result |
| --- | --- | --- |
| cpptb, this port | `gen_0.vmem`, Simple System link | 586 cycles, 0 failures |
| UVM, `ports/core_ibex_uvm` | `riscv_arithmetic_basic_test_0.bin` | 6,074 instructions, passed |

That is mutual corroboration, not yet the like-for-like comparison the rest of
this directory insists on. The two harnesses are running *different* programs,
because the two platforms end a test differently: Simple System watches the HTIF
`tohost` address, core_ibex watches a signature address, and riscv-dv bakes its
base address into the generated code, so the same `.S` cannot simply be relinked
for both.

Closing that gap is a bounded piece of work and it is what makes the comparison
mean what it means elsewhere here: same stimulus, two harnesses, a disagreement
being a port defect and a shared failure being a core property. The options are
to teach the generator to emit both endings, or to give the cpptb port a
signature-address device so it can run core_ibex's programs unmodified. The
second looks smaller.

## What is left

**A corpus runner.** `ports/riscv_arch_tests/run_cosim_programs.py` runs a fixed
list across configurations on both harnesses. It should take a generated corpus
instead, so a riscv-dv run reports the same way every other port here does.

## A limitation to state up front

Ibex's own riscv-dv customisation is SystemVerilog:
`riscv_dv_extension/ibex_asm_program_gen.sv`, `ibex_directed_instr_lib.sv` and
`riscv_core_setting.tpl.sv`. pyflow is a separate Python implementation and does
not read any of it, so this generates **generic RV32IMC riscv-dv programs, not
Ibex-tuned ones**. The core settings can be reproduced in pyflow's
configuration; the directed instruction library cannot, short of reimplementing
it.

That is a real gap against upstream's flow 5 and should not be glossed over when
the results are reported.
