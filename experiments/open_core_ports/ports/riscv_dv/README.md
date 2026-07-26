# riscv-dv random programs on cpptb

Flow 5's stimulus without UVM: riscv-dv generates random RISC-V programs, and
they run through the co-simulation harness this repository already has, with
Spike checking every retired instruction on both harnesses.

**Status: generating, building and running. Termination is not reliable yet.**

Generated programs link against Simple System and run under co-simulation with
Spike checking every retired instruction. Across three programs, about 4.5
million instructions matched with **no mismatches**, which is the result E1
exists for. What does not work yet is programs ending: one of the three halts
cleanly at 585 cycles, the other two run to the testbench cycle limit. See
[what is left](#what-is-left).

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

Ibex hardwires `mtvec.MODE` to vectored and masks `BASE` to a 256-byte boundary.
The generated program sets its own vector with `la x27, mtvec_handler; ori x27,
x27, 1; csrw mtvec, x27`, so if the handler is not on such a boundary Ibex
silently drops the low bits and traps enter part-way through the program.

The symptom is a program that executes correctly for about a million
instructions, matching Spike the whole way, and then wanders into unwritten
memory and traps on `c.unimp` forever.

Two things are needed together. `generate.py` passes `--tvec_alignment 8`, and
`target/link.ld` aligns the program body to 256 bytes, because the generator's
`.align` is relative to its section and `.text` would otherwise start at the
reset vector, which is not 256-aligned. `target/vectors.S` supplies a
`.text.reset` trampoline so the body can be aligned while Ibex still starts at
`boot_addr + 0x80`.

## What is left

**Reliable termination.** With the alignment fixed, one program of three halts
cleanly; the other two run to the cycle limit having matched Spike the whole
way. The one that halts does so after 273 instructions, which is short enough
to suspect it took an early trap into `vectors.S` rather than reaching
`test_done`, so that number should not be trusted as a pass either.

The next thing to look at is what the generated `mtvec_handler` does with the
traps these programs take, and whether it reaches `test_done` at all. A run with
the tracer enabled and the cosim log alongside should settle it quickly, since
Spike is agreeing with the core throughout and therefore is not the problem.

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
