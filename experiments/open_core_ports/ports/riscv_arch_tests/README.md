# riscv-arch-tests on Ibex Simple System

A cpptb port of the RISC-V architectural test suite, running against the same
Ibex design as `../ibex_simple_system` and compared against the same upstream
Verilator harness.

See [RESULTS.md](RESULTS.md) for the measurement. This file is about how the
port is put together.

## Ibex's own co-simulation tests

`run_cosim_programs.py` reproduces the flow Ibex's CI runs: four programs across
six configurations, under both harnesses, with upstream's own pass criteria from
`ci/run-cosim-test.sh` and upstream's own feature gating.

```sh
python3 configure.py --all      # generate a project per configuration
python3 run_cosim_programs.py --build-software --verbose
```

Each configuration needs its cpptb project built, and its upstream counterpart
built with fusesoc using `util/ibex_config.py <config> fusesoc_opts` and
`--build-root=build-cosim-<config>`. See
[ibex-coverage.md](../../ibex-coverage.md).

## Three configurations

The same suite, the same testbench and the same tooling, run three ways. Which
tests apply and what counts as passing both follow from the configuration.

| | Ibex config | tests | reference |
| --- | --- | ---: | --- |
| `small` | `small` | 98 | the upstream harness |
| `bmfull` | `maxperf-pmp-bmfull` | 193 | the upstream harness |
| `cosim` | `small` + Spike | 98 | Spike, per instruction, on both sides |

`bmfull` roughly doubles the applicable set: `RV32BFull` brings in Zba, Zbb, Zbc
and Zbs, `RV32ZcaZcbZcmp` brings in Zcb, and `PMPEnable` with 16 regions brings
in `tests/priv/pmp`. It is also a materially different core — PMP checkers, a
writeback stage, a branch-target ALU — elaborated from identical RTL, so it
exercises cpptb's code generation rather than only running more programs.

`cosim` answers a different question. In the other two, both harnesses drive the
same RTL and agreeing only proves they drove it the same way; neither knows what
the right answer is. Here Ibex's own co-simulation checker is bound in, Spike
runs in lockstep, and every retired instruction and every data memory access is
compared against it.

## Running it

```sh
python3 ../../fetch.py riscv_arch_test riscv_gcc15   # once
python3 build_tests.py                               # ~4s, builds 98 programs
uv run cpptb build --project experiments/open_core_ports/ports/riscv_arch_tests
python3 run_suite.py                                 # ~44s, both harnesses
```

For `bmfull`, generate its project first — it is not committed, because it
differs from `cpptb.toml` in a dozen lines out of 240 and the rest is a list of
141 sources that would drift if copied:

```sh
python3 configure.py --config bmfull
uv run cpptb build --project experiments/open_core_ports/ports/riscv_arch_tests_bmfull
python3 build_tests.py --config bmfull
python3 run_suite.py  --config bmfull
```

To check the co-simulation port against upstream's own baseline, run their
workload through it. `ACT_CYCLE_LIMIT` exists for this: CoreMark needs 40.7
million cycles where an architectural test needs 190,000.

```sh
# Upstream's cosim README requires this flag; without it the run fails on a
# performance-counter read, which is a real property of their flow, not of this
# port. Both outcomes reproduce here.
make -C ../../deps/ibex/examples/sw/benchmarks/coremark SUPPRESS_PCOUNT_DUMP=1
ACT_FIRMWARE=coremark.vmem ACT_CYCLE_LIMIT=80000000 \
  ../../work/riscv_arch_tests_cosim/cpptb/riscv_arch_tests_cosim/obj/Vdpi_riscv_arch_tests_cosim
```

For `cosim`, build Spike first. It installs under `deps/`, not `/opt`:

```sh
python3 ../../fetch.py spike_cosim
python3 ../../local_deps.py        # only where `sudo apt install` is unavailable
python3 ../../build_spike.py       # a few minutes
python3 configure.py --config cosim
uv run cpptb build --project experiments/open_core_ports/ports/riscv_arch_tests_cosim
python3 build_tests.py --config cosim
python3 run_suite.py  --config cosim
```

`run_suite.py` also needs the upstream harness built for the configuration it is
comparing against, which `../ibex_simple_system/README.md` covers; `bmfull`
needs a second fusesoc build with the matching parameters.

Everything here is self-contained: nothing outside this directory is modified,
`deps/` and `build/` are gitignored and reproducible, and this port is not part
of `make test`, the benchmark registry, or CI.

## Why one build runs 98 tests

`../ibex_simple_system` loads its firmware during elaboration, through
`ibex_simple_system`'s `SRAMInitFile` parameter and `$readmemh`. That bakes the
program path into the build, which is fine for one program and impossible for
98: the design takes 56 seconds to elaborate.

So this loads at run time, writing each program straight into the RAM array
through cpptb's memory backdoor:

```cpp
dut.u_ram.u_ram.mem[index].deposit(word);
```

The generated hierarchy exposes the array as a typed object, so there is no DPI
export to write, no ELF reader to link, and no rebuild between tests. Upstream
reaches the same memory through `MemArea`, `DpiMemUtil` and the
`simutil_memload` export, backed by libelf.

The testbench takes its program from the environment rather than the command
line, matching the convention in `tests/conformance/runtime` and keeping the
file free of any Verilator header:

| | |
| --- | --- |
| `ACT_FIRMWARE` | path to the VMEM to load (required) |
| `ACT_NAME` | test name, for the report |
| `ACT_SIG_BEGIN`, `ACT_SIG_END` | signature region, for the backdoor read |

## How a test is checked

The suite's normal flow builds each test twice — once to capture a golden
signature from a reference model, once with that signature linked in so the
program self-checks on the DUT. That needs Sail.

This port asks a different question, so it needs no reference model: run one
binary under both harnesses and require them to agree. Getting the signature
back out of each run is the only difficulty, and the upstream harness has no
memory-dump option, so the program reports its own. `target/rvmodel_macros.h`
digests the signature region on the way out and prints it through the Simple
System character register, which both harnesses already forward to their log.

cpptb additionally reads the same region through the backdoor and digests it
with the same function. The two are computed by completely different means —
one by the core executing loads, one by the host reading the array — so
agreement is a real check that the backdoor sees the memory the core used.

`run_suite.py` requires all three to line up.

## Layout

| | |
| --- | --- |
| `build_tests.py` | selects and compiles the applicable tests |
| `run_suite.py` | runs each under both harnesses and compares |
| `testbench.cpp` | the cpptb harness |
| `cpptb.toml` | derived from `../ibex_simple_system/cpptb.toml` |
| `target/rvtest_config.h` | the DUT configuration, written by hand |
| `target/rvmodel_macros.h` | halt, IO and the signature digest |
| `target/link.ld` | the Simple System memory map |
| `target/vectors.S` | Ibex's reset vector table |
| `target/null_test.S` | the per-run cost baselines |
| `build/` | generated ELFs, VMEMs and `manifest.json` (gitignored) |

Every file in `target/` explains the choices it encodes; between them they are
the whole of what porting the suite to a new core requires.

## Two things that are not what upstream does

**`rvtest_config.h` is written by hand.** Upstream generates it from a UDB
configuration with a Ruby toolchain. Nothing else in this repository needs
Ruby, and the file is 20 macros of which most are vector and PMP settings that
do not apply, so it is written out and committed with each value justified
against the Ibex RTL. Drift shows up as a missing-macro error rather than as a
silently wrong configuration.

**`-D_SAIL_MACROS_H` is set.** `-DSIGNATURE` is the mode that makes the tests
store their results, but it also pulls in `sail_macros.h`, which redefines the
target's halt and IO macros for Sail. Setting the header's own include guard
suppresses it. There is no supported switch for this; RESULTS.md explains why
the alternatives do not work.

## Extending it

Porting to another core means replacing the four files in `target/` and the
design section of `cpptb.toml`. The selection logic, the build, the digest and
the comparison are all core-independent.

Two things that would be worth doing here and are not done:

- **A reference model.** Comparing against Sail would turn this from "the two
  harnesses agree" into "Ibex is architecturally correct", at the cost of a
  dependency and a second build per test.
- **Batch mode.** The backdoor makes it possible to run every test in one
  process, resetting between programs, which would remove 98 process starts
  from both the measurement and the wall clock. It is not done because
  `simulator_ctrl` issues `$finish` from RTL when a program halts, which ends
  the process; avoiding that means the programs would no longer terminate under
  the upstream harness, and the comparison would stop being like-for-like.
