# Open-core ports

A work area for porting verification testbenches that other projects wrote, to
cpptb, so the framework can be compared against real testbenches for designs it
does not own.

Nothing here is wired into `make test`, the benchmark registry, the `1.10`
performance guard, or CI. It is deliberately isolated: a port that turns out to
be worth keeping graduates into `benchmarks/framework_comparison/`, and one
that does not can be deleted without touching anything else.

## Why this is separate

`benchmarks/` carries the feature registry and the hard performance guard.
Immature work does not belong behind a gate that is supposed to mean something.
This directory follows the pattern `experiments/uvm_comparison` already
established: keep the harness in git, fetch upstream into a gitignored `deps/`
at a pinned commit, and commit only summarised results.

## Layout

```
sources.toml     upstream projects, each pinned to an exact commit
fetch.py         clones a pinned commit into deps/ and verifies it
FINDINGS.md      every defect this exercise turned up, with links
open-issues.md   the subset still needing a decision
deps/            gitignored: fetched upstream trees
work/            gitignored: build scratch
results/         gitignored except committed summaries
ports/<name>/    one directory per ported testbench
```

[FINDINGS.md](FINDINGS.md) is the short version of what porting these
testbenches found: eleven Verilator defects, fifteen in Ibex and lowRISC's DV
library, seven in riscv-dv, one in Spike, and eight limitations of cpptb itself.
Each entry links to the problematic code and says which feature it was hit
through. It also carries an audit of every tool guard in the Ibex tree, tested
rather than reasoned about — which is how the assertions came to be on by
default here.

## Usage

```sh
python3 fetch.py --list      # what is pinned, and whether it is fetched
python3 fetch.py ibex        # fetch one project
python3 fetch.py --all
make -C experiments/open_core_ports fetch
```

Upstream is never modified. A port reads from `deps/` and writes only under its
own directory, `work/`, or `results/`.

## Adding a project

1. Add a table to `sources.toml` with the repository, an exact commit, the
   licence, and what the port intends to use.
2. Fetch it and confirm the pin resolves.
3. Create `ports/<name>/` with the cpptb testbench, a `cpptb.toml`, and a
   README naming the upstream harness it replaces.
4. Record evidence for both sides, not only timings. See below.

Fetched sources are not redistributed from this repository, so they need no
entry in `THIRD_PARTY_NOTICES.md`. Anything actually committed here does.

## What a port should report

Performance alone wastes the exercise, because the performance case is already
made elsewhere in this repository. Each port should record:

- the semantic evidence both sides produce, and that they agree;
- wall time, with the validity the existing environment guard would demand;
- testbench lines written, and how much was generated rather than authored;
- what a failure reports before a debugger is opened;
- time from editing a test to seeing a result.

## Comparison peers

Verilator runs plain SystemVerilog and cocotb dependably, and both are already
modes in the four-mode harness. UVM runs but is not yet dependable there:
`experiments/uvm_comparison` measured one test passing three times out of
three, one segfaulting, and both random tests reporting scoreboard errors. Use
UVM as an ergonomics peer, and plain SystemVerilog or cocotb when a number has
to mean something. See [future directions](../../docs/future-directions.md).

## Coverage

[ibex-coverage.md](ibex-coverage.md) maps what Ibex verifies itself with against
what runs under cpptb, consolidates what the exercise found, and lists what is
left. Start there for review.

## Ports

The first two drive the same Ibex design against the same upstream harness, and
measure deliberately different things. Together they separate what a simulated
cycle costs from what a run costs. The third is a block-level testbench with a
UVM baseline, and measures whether two scoreboards agree. The fourth is Ibex's
own core-level verification environment, and measures whether a cpptb port of it
reaches the same verdict on the same 944 tests.

- **[`ibex_simple_system`](ports/ibex_simple_system/RESULTS.md)** — CoreMark at
  100 iterations, one program of 40.7 million cycles. Steady-state throughput:
  `1.140x`, cpptb over upstream.

- **[`riscv_arch_tests`](ports/riscv_arch_tests/RESULTS.md)** — the RISC-V
  architectural test suite, run in three configurations against two Ibex
  builds. Per-run cost rather than throughput: many short programs, where
  process start, memory load and reset dominate.

  | | Ibex config | tests | passed | result |
  | --- | --- | ---: | ---: | --- |
  | `small` | `small` | 98 | 98 | `1.086x` |
  | `bmfull` | `maxperf-pmp-bmfull` | 193 | 177 | `0.997x` |
  | `cosim` | `small` + Spike | 98 | 98 | `0.989x`, every instruction checked |

  Every configuration runs both harnesses on every test, co-simulation
  included, so any error has to appear on both sides or it is reported as a
  port defect. None did. The 16 that did
  not pass are PMP tests both harnesses agree about, which says something about
  the core or the tests and nothing about the port.

  cpptb starts a run about 29% faster and simulates 3-10% slower, so the two
  meet on the larger design.

  `bmfull` is a materially different core — PMP, a writeback stage, bitmanip —
  elaborated from identical RTL, so it exercises cpptb's code generation and
  not only the tests. `cosim` binds in Ibex's own co-simulation checker and
  runs Spike in lockstep, which answers the question the other two cannot: they
  show the harnesses agree, this shows the core is right.

- **[`ibex_icache_cpptb`](ports/ibex_icache_cpptb/RESULTS.md)** — three of the
  ten tests in Ibex's `dv/uvm/icache`, run against
  [`ports/ibex_icache_uvm`](ports/ibex_icache_uvm/README.md), which runs the
  UVM environment unmodified and passes all ten.

  18 of 18 runs pass on both harnesses, and a further 120 cpptb runs across 40
  seeds. Every rate the two can be compared on agrees within a few per cent
  except one, and that one is a Verilator defect the comparison found: a
  constrained `randomize()` over an `inside` range is not uniform over it, so
  the baseline runs about 25% more instruction fetches per transaction than its
  own constraints ask for.

  This is the only port whose reference is a scoreboard rather than a
  signature or a reference model. Every returned fetch is checked against every
  memory seed the model still holds.

- **[`core_ibex_cpptb`](ports/core_ibex_cpptb/RESULTS.md)** — Ibex's real
  verification environment, `dv/uvm/core_ibex`, against
  [`ports/core_ibex_uvm`](ports/core_ibex_uvm/README.md), which runs the UVM
  environment on Verilator and passes 912 of the 944 hand-written directed
  tests.

  The cpptb port passes **the same 912**, and **943 of the 944 entries reach
  the identical outcome**; all 744 ePMP exit codes recovered from the execution
  trace agree exactly. The one that differs runs out of wall clock on the
  baseline and out of cycles here.

  Scoped to `core_ibex_base_test`, which 943 of the 944 entries name, plus the
  thirty-line class the 944th needs. The other eight test classes and the
  interrupt and debug agents serve the riscv-dv testlist, where the baseline's
  own accounting says 17 of its 30 passes are hollow.

  Both harnesses run byte-identical program binaries: the cpptb runner imports
  the baseline's compile rather than reimplementing it. Record-and-replay closes
  the loop that a shared verdict cannot: the baseline writes down every pin its
  environment drives, and **375,100 cycles replay with all thirteen output
  fields matching on every one**, with this port's Spike scoreboard accepting
  all 26,090 instructions of the baseline's own runs.

  The whole testlist is 150 simulator-seconds here against the baseline's 9,240,
  and 105 seconds of wall clock against 2,365 at the same four jobs.

The two program-driven ports build their software with a pinned prebuilt RISC-V
toolchain, so neither needs one installed. `ibex_simple_system` commits its firmware as a
VMEM and does not need the toolchain to run at all. Co-simulation additionally
needs Spike, which `build_spike.py` builds into `deps/` rather than `/opt`.

## Planned ports

Nothing scheduled. The Ibex ports cover the throughput axis, the per-run axis
and one block-level testbench with a UVM baseline; the natural next step is a
second design rather than a fourth workload on this one. See
[future directions](../../docs/future-directions.md).
