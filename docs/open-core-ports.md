# Ports of real testbenches

The strongest evidence that a framework works is somebody else's testbench
running on it. `experiments/open_core_ports` takes verification environments
that other projects wrote — chiefly lowRISC's [Ibex](https://github.com/lowRISC/ibex)
RISC-V core and its DV tree — and ports them to cpptb, running every test
under **both** harnesses and comparing the results. An error on one side only
is a port defect by construction; an error on both is a property of the core
or the test.

This page is the guide to what exists there and what it showed. It is distinct
from the [open-source core benchmarks](examples/open-source-cores.md), which
time framework-authored testbenches over vendored cores; the ports here run
*upstream's own tests* — 944 directed programs, an architectural test suite,
a UVM block environment — with upstream sources pinned by exact commit and
never modified.

Nothing under `experiments/` is wired into `make test`, the benchmark
registry, or the performance guard. Each port's `README.md` and `RESULTS.md`
are the authoritative record; this page carries the shape and the headline
numbers.

## The ports at a glance

| Port | Upstream reference | Result |
|---|---|---|
| `core_ibex_cpptb` | Ibex's core-level UVM environment, `dv/uvm/core_ibex` | 944 directed tests: the same 912 pass on both harnesses; 871,825 instructions co-simulated against Spike |
| `ibex_icache_cpptb` | Ibex's icache UVM block environment | all ten tests, ten seeds each, on both harnesses: 200 of 200 runs pass |
| `riscv_arch_tests` (+ cosim variants) | riscv-arch-test suite on Ibex Simple System | `small` 98/98, `bmfull` 177/193 with 16 failures shared identically; cosim 98/98 with Spike in lockstep on both sides |
| `ibex_cs_registers` | Ibex's CSR block bench and C++ reference model | reproduces upstream's run transaction for transaction; found a reference-model gap, filed upstream |
| `ibex_simple_system` | Ibex's hand-written Verilator harness | CoreMark 100 iterations: identical cycle count and validated score, `1.140x` wall time |
| `riscv_dv` | riscv-dv random instruction generation (pyflow) | generated programs run under both harnesses with Spike checking every retired instruction, no mismatches |

## The core-level Ibex port

`ports/core_ibex_cpptb` is the largest port: Ibex's real verification
environment, `dv/uvm/core_ibex`, rewritten against cpptb and run against
`ports/core_ibex_uvm`, the unmodified UVM environment on Verilator. Both
harnesses drive the same Ibex (`--config opentitan`: PMP, icache with ECC and
scrambling, SecureIbex), elaborated with the same parameters, running
byte-identical program binaries — the cpptb runner imports the baseline's
compile rather than reimplementing it.

Across the 944-entry directed testlist:

- **912 of 944 pass — the same 912 on both harnesses.** 943 of 944 entries
  reach the identical outcome (the one difference is a test that runs out of
  wall clock on the baseline and out of cycles here), and all 744 ePMP exit
  codes recovered from the execution trace agree exactly.
- The run covers 14,335,393 cycles and 881,470 retired instructions, of which
  **871,825 are co-simulated against Spike** instruction by instruction.
- Record-and-replay closes the loop a shared verdict cannot: the baseline
  writes down every pin its environment drives, and 375,100 cycles replay on
  the port with all thirteen output fields matching on every cycle.
- Fault injection shows the checks are live, not merely quiet: forty
  instruction-memory corruptions in one program are caught 38 times, and a
  data-memory sweep that the co-simulation scoreboard catches 17 times is
  caught **zero** times with the reference model switched off.

The whole testlist takes 105 seconds of wall clock against the baseline's
2,365 at the same parallelism. The honest framework-to-framework number is
smaller than that headline: measured like for like — the same testbench
decomposition on both sides, interleaved on one host — the ratio is **48.8x**,
with about a fifth of the raw gap attributable to testbench differences
rather than the frameworks. The port's `RESULTS.md` records that decomposition
and its caveats in full.

## Spike co-simulation

Every instruction-level claim above rests on lockstep co-simulation against
[Spike](https://github.com/riscv-software-src/riscv-isa-sim), the RISC-V
reference simulator. The integration is deliberately **upstream's own**:
lowRISC maintains an `ibex_cosim` branch of Spike with a co-simulation API,
and Ibex's `dv/cosim` layer (`spike_cosim.cc`, `cosim_dpi.cc`) drives it. The
ports pin that branch and reuse the layer unmodified, in two shapes:

- **Bound in as RTL, through DPI.** The Simple System ports compile Ibex's
  own co-simulation checker and bind it into the design, exactly as
  upstream's Verilator flow does. Both harnesses carry their own Spike, so a
  mismatch must appear on both sides or neither.
- **Linked directly as C++.** `core_ibex_cpptb` holds the `SpikeCosim` object
  in the testbench and calls its C++ interface where the UVM environment
  holds a handle and calls the same functions through DPI imports. The
  co-simulation scoreboard is ported whole — nothing it checks was dropped.

Per retired instruction, the check is substantive: the PC must match, a
register write must match (or be correctly suppressed), synchronous traps
must match including the trapping PC, and every data-side memory access is
compared on address, direction, data, and byte enables. Interrupt, NMI,
debug-request, and performance-counter state are pushed into Spike each step
so asynchronous events land at the same instruction on both models.

Spike is built once, without root, into the experiment's own tree:

```sh
make -C experiments/open_core_ports fetch    # pinned sources, exact commits
make -C experiments/open_core_ports tools    # local dtc/libelf/z3, no sudo
make -C experiments/open_core_ports spike    # ~5 minutes, into deps/
```

`build_spike.py` configures the two flags the checker depends on
(`--enable-commitlog`, `--enable-misaligned`) and installs a `pkg-config`
file; every consumer finds Spike through `pkg-config` alone.

Co-simulation also produced the one finding where the reference model was
wrong and the DUT was right: the pinned Spike registers `menvcfg` but not
`menvcfgh`, so on RV32 a write to the high half traps in the model where Ibex
correctly implements it as read-only zero. The disagreement reproduces
identically on both harnesses — which is exactly the kind of verdict the
both-sides discipline exists to deliver.

## The architectural tests

`ports/riscv_arch_tests` runs the official
[riscv-arch-test](https://github.com/riscv-non-isa/riscv-arch-test) suite on
Ibex Simple System, with applicability derived from each test's declared
requirements rather than hand-maintained lists. Three measured
configurations: `small` (98 of 98 pass, `1.086x` upstream's wall time),
`bmfull` (a materially different core — PMP, writeback stage, bitmanip; 177
of 193 pass with all 16 failures identical on both harnesses), and `cosim`
(98 of 98 with Spike in lockstep on both sides, `0.989x`). Checking every
retired instruction costs about 2.6x the uncosimulated run — and costs both
harnesses the same.

Notably, Ibex upstream has both an architectural test suite and a reference
model but never uses them together; the cosim configuration here is that
missing combination. A six-configuration matrix mirroring Ibex's own CI
(`small` through `opentitan`) additionally runs upstream's four co-simulated
programs: 11 of 11 runnable combinations pass on both harnesses, with
**16,977,869 instructions checked**.

`ports/riscv_dv` closes the stimulus side: riscv-dv's Python flow generates
random programs that run under both harnesses with Spike checking every
retired instruction — including a run under asynchronous interrupt stimulus
that matched 1,046,965 instructions with no mismatches.

## The block-level ports

`ports/ibex_icache_cpptb` ports all ten tests of Ibex's icache UVM
environment: 200 of 200 runs pass across ten seeds on both harnesses, and a
replay harness drives the baseline's recorded stimulus into the port — 180 of
180 replays pass, with DUT outputs matching cycle for cycle over 4,699,689
cycles. This port is discussed throughout the docs as the origin of the
[write-model](coming-from-cocotb.md) work; its `RESULTS.md` also documents
the Verilator randomization defect the comparison surfaced.

`ports/ibex_cs_registers` ports the CSR block bench against upstream's C++
reference model and reproduces upstream's run transaction for transaction —
until transaction 1,119, where the reference model itself has a gap
(ePMP MML write suppression is not modeled). That finding was confirmed
against the RTL's documented behavior and
[filed upstream](https://github.com/lowRISC/ibex/issues/2242). The control
run also showed why paired running matters: the unmodified upstream bench on
Verilator 5.050 drives **zero transactions and prints `TEST PASSED`**.

## What porting found

Porting somebody else's testbenches is an adversarial exercise for every tool
in the chain. The tally, each entry with a reduced reproducer and a
workaround in use: **eleven Verilator defects** (five of them in constrained
randomization — one, a `randomize() with` constraint naming a variable
`item` being silently dropped, was behind 22 of 27 failing test classes),
**fifteen in Ibex and lowRISC's DV library** (including all 132 assertions
compiling away under Verilator, upstream and here — the ports now run them by
default), **seven in riscv-dv**, **one in Spike** (the `menvcfgh` case
above), and **eight limitations of cpptb itself**, of which the discovery
and timing-backend defects were fixed in the framework and the rest are
recorded. `experiments/open_core_ports/FINDINGS.md` is the master list.

## Reproducing

Everything is pinned and fetched, never vendored: `sources.toml` records
exact commits, licenses, and the purpose of every dependency, and `fetch.py`
(standard library only) verifies and unpacks them. The per-port commands live
in each port's README; the shared prerequisites are the three `make` targets
shown [above](#spike-co-simulation). Ports write only under their own
directory, `work/`, or `results/`; upstream sources under `deps/` are never
modified.
