# Ibex verification: what upstream runs, what is ported, what is left

A review document for the Ibex porting exercise. It answers three questions:
what does Ibex actually verify itself with, how much of that runs under cpptb,
and what would it take to close the rest.

Measurements live in each port's `RESULTS.md`; this is the map.

## What Ibex verifies itself with

From `.github/workflows/ci.yml` and `.github/actions/ibex-rtl-ci-steps`, which
is the authoritative list rather than an inferred one.

Run for **each of six configurations** (`small`, `opentitan`, `maxperf`,
`maxperf-pmp-bmbalanced`, `maxperf-pmp-bmfull`, `experimental-branch-predictor`):

| | flow | reference |
| --- | --- | --- |
| 1 | Verilator lint, Verible lint | — |
| 2 | RISC-V Compliance suite, `ibex_riscv_compliance` | committed signatures |
| 3 | Verilator co-simulation of CoreMark, `pmp_smoke_test`, `dit_test`, `dummy_instr_test` | Spike, per instruction |

Run once, configuration-independent:

| | flow | reference |
| --- | --- | --- |
| 4 | `tb_cs_registers` | a C++ model of the CSR block |

Elsewhere:

| | flow | where |
| --- | --- | --- |
| 5 | riscv-dv random programs under UVM, `dv/uvm/core_ibex` | `private-ci.yml`, internal infrastructure |
| 6 | instruction-cache block testbench, `dv/uvm/icache` | UVM |
| 7 | formal, `dv/formal` | `ci-formal.yml` |

Two things are worth noticing about this list.

**Ibex has both an architectural test suite and a reference model, but never
uses them together.** Flow 2 runs `riscv/riscv-compliance` -- the *archived*
predecessor of `riscv-arch-test` -- and compares signatures with no reference
model. Flow 3 uses a reference model but runs four fixed programs. Nothing
upstream runs a current architectural suite against Spike.

**The parameter sets come from `util/ibex_config.py`.** `python3
util/ibex_config.py <config> fusesoc_opts` emits the exact flags CI uses, so
anything here that needs per-configuration parameters should ask that tool
rather than transcribe them.

## What is ported

Three ports, in `ports/`. Each drives the same design as the upstream harness it
replaces, from the same sources at the same pinned commit, under the same
Verilator.

| port | workload | configurations | reference |
| --- | --- | --- | --- |
| [`ibex_simple_system`](ports/ibex_simple_system/RESULTS.md) | CoreMark, 40.7M cycles | `small` | the upstream harness |
| [`riscv_arch_tests`](ports/riscv_arch_tests/RESULTS.md) | 98 / 193 architectural tests | `small`, `maxperf-pmp-bmfull` | the upstream harness, and Spike per instruction |
| [`riscv_arch_tests/run_cosim_programs.py`](ports/riscv_arch_tests/README.md) | CoreMark, `pmp_smoke`, `dit_test`, `dummy_instr_test` | all six | Spike per instruction, on both harnesses |
| [`ibex_cs_registers`](ports/ibex_cs_registers/README.md) | 1,119 random CSR transactions | the `tb_cs_registers` defaults | a C++ model of the CSR block |

**Flow 3 is complete.** `run_cosim_programs.py` runs the same four programs
across the same six configurations as `.github/actions/ibex-rtl-ci-steps`, with
upstream's own pass criteria taken from `ci/run-cosim-test.sh`, and with the
same feature gating: `pmp_smoke` only where `PMPEnable=1`, `dit_test` and
`dummy_instr_test` only where `SecureIbex=1`, both asked through
`util/ibex_config.py query_fields` exactly as CI asks.

    11/11 passed on both harnesses
    13 not run because the configuration lacks the feature, which is what CI does
    16,977,869 instructions checked against Spike

**Flow 4 is ported** as [`ibex_cs_registers`](ports/ibex_cs_registers/README.md),
which is the only one here that runs no program: it drives random CSR
transactions at a submodule and scores them against upstream's C++ model.

`riscv_arch_tests` additionally runs a suite upstream does not run at all.
Flows 1, 2, 5, 6 and 7 remain.

### The rule every port follows

Every test runs under **both** harnesses and the results are compared. An error
that appears on one side and not the other is reported as a defect in the port;
an error that appears on both is reported as a property of the core or the
test, and never as a difference in speed.

That is checked against real failures rather than assumed:

- Under `bmfull`, 16 PMP tests do not pass. All 16 fail identically on both
  harnesses -- 12 never complete, 3 take an early trap, 1 reports failure.
- Rebuilt without the `menvcfgh` workaround, the co-simulation tests fail on
  both harnesses with the same message and the same addresses.
- CoreMark built without `SUPPRESS_PCOUNT_DUMP=1` reproduces exactly the
  failure Ibex's own cosim README documents; built with it, 2,794,236
  instructions match.

## What the exercise found

Consolidated from all three ports. Grouped by who owns the problem.

### In cpptb, found by the CoreMark port and since fixed

1. Packed type names collided when two generate blocks declared different enums
   of the same name, which also blocked hierarchy access to all 2120 signals.
2. Parameters were passed both as wrapper localparams and as `-G`.
3. String parameters were emitted as `localparam int`, so a firmware path
   silently loaded nothing while the build succeeded.
4. The watchdog was hard-coded at 1,000,000 cycles.

### In cpptb, found since and not yet addressed

5. **The discovery pass runs the testbench.** `cpptb build` compiles it again
   with `-DCPPTB_HIERARCHY_DISCOVERY` and executes it to learn which signals
   are clocks. A testbench that returns early when its environment is empty --
   which it always is at build time -- is discovered as one that never starts a
   clock, and every run then dies at time zero with "scheduler starvation".
   A build-time problem reported as a scheduler problem.
6. **Rebuilds do not track sources added through `verilator_args`.** Editing
   such a file and rebuilding is a no-op, so a fix appears not to work.
   `--rebuild` is the workaround.
7. **`.svh` is rejected in the source list.** Defensible, but upstream's fusesoc
   core compiles `cosim_dpi.svh` directly because it holds DPI declarations.
8. **`co_await RisingEdge` resumes before the design evaluates the edge**, so it
   is right for sampling and wrong for driving: `set()` is immediate, and a
   value written there is captured by the edge just awaited. The cocotb shape a
   new author reaches for therefore drives a cycle early. The phase waits that
   would express this directly, `ReadOnly` and `ReadWrite`, cannot be used from
   a project because no timing backend providing them is selectable. Written up
   in [Scheduling](../../docs/scheduling.md#coming-from-cocotb) and on the
   roadmap as
   [aligning the scheduling semantics with cocotb](../../docs/future-directions.md#align-the-scheduling-semantics-with-cocotb).

### In the port, all costly to diagnose

9. The upstream ELF loader places segments relative to the file's lowest
   address, so a hole at the bottom of the image loads everything shifted.
10. `SpikeCosim` has two base classes, so `get_spike_cosim` must `static_cast`
   to `Cosim*`; the DPI signature returns `void*`, which is why the compiler
   cannot catch it.
11. `bit [31:0]` crosses DPI as `const svBitVecVal*`, not `uint32_t`.
12. slang rejects the implicit `.PARAM,` shorthand in upstream's bind file,
    which is a Verilator extension.

### In upstream

One is filed. The other three are not, and are listed so that is visible
rather than assumed.

13. **Upstream's CSR model does not implement MML write suppression.**
    `ibex_cs_registers.sv` refuses a `pmpcfg` write when `MSECCFG.MML` is set,
    `RLB` is clear and the new byte has `lock` set with `{r,w,x}` in
    `{001,010,011,101}`. `PmpCfgRegister::RegisterWrite` masks by `lock & !RLB`
    only. The model knows MML *relaxes* which encodings are legal
    (`HandleReservedVals`) but not that it *blocks* these writes. Found by the
    `ibex_cs_registers` port from upstream's own stimulus. Independently
    reported by someone else in
    [lowRISC/ibex#2242](https://github.com/lowRISC/ibex/issues/2242), open since
    January 2025 and unfixed; the code-level cause and a suggested fix are
    [added there](https://github.com/lowRISC/ibex/issues/2242#issuecomment-5084571451).
14. **`tb_cs_registers` is vacuous on Verilator 5.050.** It reports `TEST
    PASSED` having driven zero transactions, and never terminates without a
    cycle limit. Ibex pins v4.210 for it. Not root-caused, not confirmed against
    4.210, **not filed**. Arguably the most serious of these: a green result
    that checked nothing.
15. **The pinned Spike never registers `CSR_MENVCFGH`.** It registers
    `CSR_MENVCFG` and has no entry for the high half, so on RV32 it traps where
    Ibex correctly implements it as read-only zero. A model with the low half
    and not the high half is not a coherent RV32 configuration. **Not filed**;
    belongs on `lowRISC/riscv-isa-sim` rather than `ibex`.
16. **The suite's `Zicsr` tests cannot be built for a core with U-mode but no
    F, no V and no `time` CSR.** They pick a scratch CSR from a fixed ladder
    and fall off the end into `#error`.
17. **The suite's PMP tests assume a granularity of 2 or more.** They size NAPOT
    padding as `(1 << (UDB_PMP_GRANULARITY - 3)) - 1`, which underflows for
    granularity 0 and reaches `.rept` as 2^64-1.

Upstream handles CSR divergences the same way this port does -- by not
exercising the CSR. `SUPPRESS_PCOUNT_DUMP=1` for the counters, `MENVCFGH`
absent from riscv-dv's implemented-CSR list, and `MINSTRET`/`MINSTRETH`
commented out under a TODO reading "these are currently removed as they can
cause co-sim mismatches. These must be investigated and fixed".

## What is left

Ordered by cost, not by value.

| | work | effort | what it adds |
| --- | --- | --- | --- |
| ~~A~~ | ~~Three more co-sim programs~~ | done | flow 3's workload |
| ~~B~~ | ~~Four more configurations~~ | done | flow 3's matrix, including `opentitan` with icache, ECC, scrambling and SecureIbex |
| ~~C~~ | ~~`tb_cs_registers`~~ | done | flow 4, a different *shape* of testbench, and a model bug |
| D | RISC-V Compliance, `ibex_riscv_compliance` | medium | flow 2, exact CI parity |
| E | riscv-dv under UVM, `dv/uvm/core_ibex` | large | flow 5, Ibex's real verification depth |
| F | `dv/uvm/icache` | large | flow 6, a block-level testbench |
| G | `dv/formal` | — | not a simulation flow; out of scope for cpptb |

Flow 1 is lint. It is not a testbench and nothing about it would exercise cpptb,
so it is not listed above.

**A and B are done.** The parameters come from `util/ibex_config.py` rather
than a table, so a configuration added upstream needs no transcription and one
that changes cannot silently diverge. `opentitan` is the notable addition: an
instruction cache with ECC and scrambling plus `SecureIbex`, which is the most
RTL any of these elaborates, and the only configuration on which the two
security tests run at all.

**C works**, in
[`ports/ibex_cs_registers`](ports/ibex_cs_registers/README.md). It drives 1,119
transactions against upstream's model and then stops on a gap in that model,
which it demonstrates rather than assumes: Ibex suppresses a PMP configuration
write when `MSECCFG.MML` is set, `RLB` is clear and the new byte would create an
M-mode-executable region, and the model has no such rule. All three conditions
are shown to hold at the failing transaction.

The control was built and does not settle it, for a reason worth recording on
its own: **upstream's `tb_cs_registers` drives zero transactions and reports
`TEST PASSED` on Verilator 5.050.** Its driver object is constructed but
`driver_tick`'s lookup never finds it, so nothing is ever driven, and without a
cycle limit the simulation never terminates. Ibex pins `VERILATOR_VERSION=v4.210`
in `ci/vars.env`; every port here runs on 5.050. A testbench that silently
checks nothing and reports success is a worse failure than one that crashes,
and the cpptb port drives 1,119 real transactions where the original drives
none.

It was the most interesting of the three for the framework, and that held up.
Every other port is "load a program, run it, check the result"; this one drives
transactions at a submodule and scores them against a model, which is where
findings 8 and 5 came from. Three testbench bugs on the way there are in its
README.

**D is largely redundant.** It is the archived predecessor of the suite already
ported; its value is CI parity, not new signal.

**E is its own project.** It needs UVM, which `experiments/uvm_comparison`
found unreliable on Verilator, and it is where the CSR exclusions above come
from.

## Reproducing

Each port's `README.md` has the commands. The shared prerequisites:

```sh
python3 fetch.py --all      # pinned upstream sources into deps/
python3 local_deps.py       # dtc, libelf, lz4 without root (Linux only)
python3 build_spike.py      # the co-simulation reference model
```

Nothing here is wired into `make test`, the benchmark registry, the performance
guard, or CI.
