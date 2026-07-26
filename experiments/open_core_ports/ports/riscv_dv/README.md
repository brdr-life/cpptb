# riscv-dv random programs on cpptb

Flow 5's stimulus without UVM: riscv-dv generates random RISC-V programs, and
they run through the co-simulation harness this repository already has, with
Spike checking every retired instruction on both harnesses.

**Status: generation works, the target adaptation is not written.** Programs are
produced reproducibly; nothing runs them yet.

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

## What is left

The generated programs are not yet runnable against Simple System. What a
program needs, and what has to be written:

1. **Target glue.** riscv-dv programs open with
   `.include "user_define.h"` and `.include "user_init.s"`. Defaults are in
   `vendor/google_riscv-dv/user_extension/`; Simple System needs its own.
2. **A linker script.** Programs end at `test_done` with an `ecall` and then
   spin in `write_tohost`, the HTIF mechanism Spike uses. Ibex has
   `dv/uvm/core_ibex/riscv_dv_extension/ddm_link.ld`, but it places code at
   `0x80000000` for core_ibex's map, not Simple System's `0x100000`. The one in
   `ports/riscv_arch_tests/target/link.ld` is the right shape and already
   handles Ibex's reset vector.
3. **Termination.** `write_tohost` has to become a write to the Simple System
   control register, the same adaptation
   `ports/riscv_arch_tests/target/rvmodel_macros.h` makes for the architectural
   suite.
4. **A corpus runner.** `ports/riscv_arch_tests/run_cosim_programs.py` runs a
   fixed list of programs across configurations on both harnesses; it needs to
   take a generated corpus instead.

Steps 2 and 3 are close to work already done for the architectural tests, which
is most of the reason this is tractable.

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
