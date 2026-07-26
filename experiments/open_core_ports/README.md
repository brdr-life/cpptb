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
deps/            gitignored: fetched upstream trees
work/            gitignored: build scratch
results/         gitignored except committed summaries
ports/<name>/    one directory per ported testbench
```

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

## Ports

Both drive the same Ibex design against the same upstream harness, and measure
deliberately different things. Together they separate what a simulated cycle
costs from what a run costs.

- **[`ibex_simple_system`](ports/ibex_simple_system/RESULTS.md)** — CoreMark at
  100 iterations, one program of 40.7 million cycles. Steady-state throughput:
  `1.140x`, cpptb over upstream.

- **[`riscv_arch_tests`](ports/riscv_arch_tests/RESULTS.md)** — 96
  architectural tests plus two cost baselines, 98 programs of about 190,000
  cycles each. Per-run cost: `1.069x` overall, which decomposes into cpptb
  starting a run and loading a program about 27% faster and simulating about
  13% slower. That `1.133x` simulation figure and CoreMark's `1.140x` are
  independent measurements of the same per-cycle cost on workloads sharing
  nothing but the core.

Both build their software with a pinned prebuilt RISC-V toolchain, so neither
needs one installed. `ibex_simple_system` commits its firmware as a VMEM and
does not need the toolchain to run at all.

## Planned ports

Nothing scheduled. The two Ibex ports cover the throughput and per-run axes on
one core; the natural next step is a second design rather than a third
workload on this one. See [future directions](../../docs/future-directions.md).
