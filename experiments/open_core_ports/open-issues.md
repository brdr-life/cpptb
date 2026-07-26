# Open issues from the porting exercise

Things found while porting Ibex's verification flows that are not fixed, with
enough detail to pick up cold. Numbering matches the findings list in
[ibex-coverage.md](ibex-coverage.md).

Nothing here blocks the ports: each has a workaround in use, which is why none
of them stopped the work. That is also why they are easy to forget.

## In cpptb

### 5. The discovery pass runs the testbench

`cpptb build` compiles the testbench a second time with
`-DCPPTB_HIERARCHY_DISCOVERY` and executes it to learn which signals are clocks
and which hierarchy paths need a transport. A testbench that returns early when
its environment is empty, which it always is at build time, is discovered as one
that never starts a clock. The generated SystemVerilog then has
`CALENDAR_CLOCK_COUNT = 0`, nothing toggles the clock, and every run dies at
time zero with `scheduler starvation`.

A build-time problem reported as a scheduler problem, at run time, with no
mention of discovery.

**Repro:** put an early `co_return` before `start_clock` on a condition that is
false at build time.
**Workaround:** no early returns before the clock and hierarchy accesses; see
the header comment in `ports/riscv_arch_tests/testbench.cpp`.
**Fix direction:** discovery could warn when a testbench it ran started no
clock, or the starvation message could say that no clock driver was generated.

### 6. Rebuilds do not track sources added through `verilator_args`

Only `[testbench] sources` and the RTL list are fingerprinted. A `.cc` file
added through `build.verilator_args` is compiled but not tracked, so editing it
and rebuilding is a no-op and the fix appears not to work.

Cost more than the other findings put together, because it makes every other
bug look intermittent.

**Repro:** add a `.cc` to `verilator_args`, edit it, run `cpptb build`, observe
`up to date`.
**Workaround:** `--rebuild`, or put the file in `[testbench] sources`.
**Fix direction:** fingerprint file arguments in `verilator_args`, or reject
them with a message pointing at `[testbench] sources`.

### 7. `.svh` is rejected in the source list

`cpptb: RTL source file has unsupported suffix ... (expected .sv, .v)`.
Defensible, since `.svh` is conventionally an include, but upstream fusesoc
cores do compile `.svh` files directly when they hold DPI import declarations,
as `dv/cosim/cosim_dpi.svh` does.

**Workaround:** `ports/riscv_arch_tests/configure.py` copies it in as `.sv`.
**Fix direction:** accept `.svh` in the source list, or say in the error that
copying is the intended workaround.

### 8. Edge semantics and the missing timing backend

`co_await RisingEdge` resumes before the design evaluates that edge, so it is
right for sampling and wrong for driving: `set()` is immediate and a value
written there is captured by the edge just awaited. `ReadOnly` and `ReadWrite`
would express this directly but cannot be selected from a project.

Written up in [Scheduling](../../docs/scheduling.md#coming-from-cocotb) and on
the roadmap as
[aligning the scheduling semantics with cocotb](../../docs/future-directions.md#align-the-scheduling-semantics-with-cocotb),
with three options and a recommendation. This entry exists so it is not lost
among the porting findings.

## In upstream, not filed

### 14. `tb_cs_registers` is vacuous on Verilator 5.050

It reports `TEST PASSED` having driven zero transactions, and without `-c` never
terminates. The driver object is constructed, since its `OnFinal` prints, but
`driver_tick`'s lookup in `reg_dpi.cc` never finds it.

The most serious thing found here: a green result that checked nothing.

**Repro:**
```sh
fusesoc --cores-root=. run --target=sim --tool=verilator \
  --build-root=build-csr lowrisc:ibex:tb_cs_registers
./build-csr/lowrisc_ibex_tb_cs_registers_0/sim-verilator/Vtb_cs_registers -c 500000
```
**Not root-caused.** Ibex pins `VERILATOR_VERSION=v4.210` in `ci/vars.env`; this
was seen on 5.050 and has not been confirmed against 4.210. Confirming it
against the pinned version is the first step before filing, since the report
should say whether it is a regression or has never worked on Verilator 5.

### 15. The pinned Spike never registers `CSR_MENVCFGH`

`riscv/processor.cc` on lowRISC's `ibex_cosim` branch adds `CSR_MENVCFG` to
`csrmap` and has no entry for the high half, so on RV32 a write to `menvcfgh`
traps where Ibex correctly implements it as read-only zero. A model with the low
half and not the high half is not a coherent RV32 configuration.

Belongs on `lowRISC/riscv-isa-sim`, not `ibex`.

**Repro:** build the architectural tests without `-DIBEX_NO_U_MODE` and run any
of them under co-simulation; both harnesses fail identically at the first
`csrw menvcfgh` in the suite's boot code.

### 16, 17. riscv-arch-test gaps

Two limitations in the suite for cores like Ibex, both worked around in
`ports/riscv_arch_tests/build_tests.py` with the reasons recorded there:
`Zicsr` tests cannot be built for a core with U-mode but no F, no V and no
`time` CSR, and the PMP tests assume a granularity of 2 or more. Lower value to
file than the two above, since they affect a narrower set of cores.

## Filed

### 13. Upstream's CSR model does not implement MML write suppression

Independently reported in
[lowRISC/ibex#2242](https://github.com/lowRISC/ibex/issues/2242), open since
January 2025. The code-level cause and a suggested fix are
[a comment there](https://github.com/lowRISC/ibex/issues/2242#issuecomment-5084571451).

This is what stops `ports/ibex_cs_registers` at transaction 1,119. If it is
fixed upstream, that port should reach 10,000 and the stopping point in its
README can go.
