# Ports of real testbenches

The clearest way to judge a verification framework is to run testbenches
that other people wrote. `experiments/open_core_ports` takes verification
environments from other projects, mainly lowRISC's
[Ibex](https://github.com/lowRISC/ibex) RISC-V core and its DV tree, and
ports them to cpptb. Every test runs under both harnesses and the results
are compared. An error that shows up on one side only is a port defect. An
error on both sides is a property of the core or the test.

This page summarizes what exists there and what came out of it. It is a
different thing from the
[open-source core benchmarks](examples/open-source-cores.md), which time
framework-authored testbenches over vendored cores. The ports here run
upstream's own tests: 944 directed programs, an architectural test suite,
and a UVM block environment, with upstream sources pinned by exact commit
and never modified.

Nothing under `experiments/` is wired into `make test`, the benchmark
registry, or the performance guard. Each port's `README.md` and `RESULTS.md`
are the authoritative record. This page carries the shape and the headline
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

`ports/core_ibex_cpptb` is the largest port. It rewrites Ibex's real
verification environment, `dv/uvm/core_ibex`, against cpptb, and runs
against `ports/core_ibex_uvm`, the unmodified UVM environment on Verilator.
Both harnesses drive the same Ibex (`--config opentitan`: PMP, icache with
ECC and scrambling, SecureIbex), elaborated with the same parameters, and
run byte-identical program binaries. The cpptb runner imports the baseline's
compile rather than reimplementing it.

Across the 944-entry directed testlist:

- 912 of 944 pass, and they are the same 912 on both harnesses. 943 of 944
  entries reach the identical outcome; the one difference is a test that
  runs out of wall clock on the baseline and out of cycles here. All 744
  ePMP exit codes recovered from the execution trace agree exactly.
- The run covers 14,335,393 cycles and 881,470 retired instructions, of
  which 871,825 are co-simulated against Spike instruction by instruction.
- Record-and-replay closes the loop that a shared verdict cannot. The
  baseline writes down every pin its environment drives, and 375,100 cycles
  replay on the port with all thirteen output fields matching on every
  cycle.
- Fault injection shows the checks are live rather than merely quiet. Forty
  instruction-memory corruptions in one program are caught 38 times. A
  data-memory sweep that the co-simulation scoreboard catches 17 times is
  caught zero times with the reference model switched off.

The whole testlist takes 105 seconds of wall clock against the baseline's
2,365 at the same parallelism. The honest framework-to-framework number is
smaller. Measured like for like, with the same testbench decomposition on
both sides and the runs interleaved on one host, the ratio is 48.8x, and
about a fifth of the raw gap comes from testbench differences rather than
the frameworks. The port's `RESULTS.md` records that decomposition and its
caveats in full.

## Spike co-simulation

Every instruction-level claim above rests on lockstep co-simulation against
[Spike](https://github.com/riscv-software-src/riscv-isa-sim), the RISC-V
reference simulator. The integration is upstream's own. lowRISC maintains an
`ibex_cosim` branch of Spike with a co-simulation API, and Ibex's `dv/cosim`
layer (`spike_cosim.cc`, `cosim_dpi.cc`) drives it. The ports pin that
branch and reuse the layer unmodified, in two shapes:

- Bound in as RTL, through DPI. The Simple System ports compile Ibex's own
  co-simulation checker and bind it into the design, exactly as upstream's
  Verilator flow does. Both harnesses carry their own Spike, so a mismatch
  has to appear on both sides or neither.
- Linked directly as C++. `core_ibex_cpptb` holds the `SpikeCosim` object in
  the testbench and calls its C++ interface, where the UVM environment holds
  a handle and calls the same functions through DPI imports. The
  co-simulation scoreboard is ported whole; nothing it checks was dropped.

The per-instruction check is substantive. The PC must match, a register
write must match or be correctly suppressed, and synchronous traps must
match, including the trapping PC. Every data-side memory access is compared
on address, direction, data, and byte enables. Interrupt, NMI,
debug-request, and performance-counter state are pushed into Spike each step
so that asynchronous events land at the same instruction on both models.

Spike is built once, without root, into the experiment's own tree:

```sh
make -C experiments/open_core_ports fetch    # pinned sources, exact commits
make -C experiments/open_core_ports tools    # local dtc/libelf/z3, no sudo
make -C experiments/open_core_ports spike    # ~5 minutes, into deps/
```

`build_spike.py` configures the two flags the checker depends on
(`--enable-commitlog`, `--enable-misaligned`) and installs a `pkg-config`
file. Every consumer finds Spike through `pkg-config` alone.

Co-simulation also produced the one finding where the reference model was
wrong and the DUT was right. The pinned Spike registers `menvcfg` but not
`menvcfgh`, so on RV32 a write to the high half traps in the model, where
Ibex correctly implements it as read-only zero. The disagreement reproduces
identically on both harnesses.

## The architectural tests

`ports/riscv_arch_tests` runs the official
[riscv-arch-test](https://github.com/riscv-non-isa/riscv-arch-test) suite on
Ibex Simple System. Applicability is derived from each test's declared
requirements rather than maintained by hand. Three configurations are
measured: `small` (98 of 98 pass, at `1.086x` upstream's wall time),
`bmfull` (a materially different core with PMP, a writeback stage, and
bitmanip; 177 of 193 pass, and all 16 failures are identical on both
harnesses), and `cosim` (98 of 98 with Spike in lockstep on both sides, at
`0.989x`). Checking every retired instruction costs about 2.6x the
uncosimulated run, and it costs both harnesses the same.

Ibex upstream has both an architectural test suite and a reference model,
but never uses them together. The cosim configuration here is that missing
combination. A six-configuration matrix mirroring Ibex's own CI (`small`
through `opentitan`) additionally runs upstream's four co-simulated
programs: 11 of 11 runnable combinations pass on both harnesses, with
16,977,869 instructions checked.

`ports/riscv_dv` closes the stimulus side. riscv-dv's Python flow generates
random programs that run under both harnesses with Spike checking every
retired instruction, including a run under asynchronous interrupt stimulus
that matched 1,046,965 instructions with no mismatches.

## The block-level ports

`ports/ibex_icache_cpptb` ports all ten tests of Ibex's icache UVM
environment. 200 of 200 runs pass across ten seeds on both harnesses. A
replay harness drives the baseline's recorded stimulus into the port: 180 of
180 replays pass, with DUT outputs matching cycle for cycle over 4,699,689
cycles. This port appears throughout the docs as the origin of the
[write-model](coming-from-cocotb.md) work, and its `RESULTS.md` documents
the Verilator randomization defect the comparison surfaced.

`ports/ibex_cs_registers` ports the CSR block bench against upstream's C++
reference model and reproduces upstream's run transaction for transaction,
until transaction 1,119, where the reference model itself has a gap: ePMP
MML write suppression is not modeled. The finding was confirmed against the
RTL's documented behavior and
[filed upstream](https://github.com/lowRISC/ibex/issues/2242). The control
run also showed why paired running matters. The unmodified upstream bench on
Verilator 5.050 drives zero transactions and prints `TEST PASSED`.

## What porting found

Porting somebody else's testbenches exercises every tool in the chain. The
tally, with a reduced reproducer and a working workaround for each entry:
eleven Verilator defects, five of them in constrained randomization (one of
those, a `randomize() with` constraint naming a variable `item` being
silently dropped, was behind 22 of 27 failing test classes); fifteen in Ibex
and lowRISC's DV library, including all 132 assertions compiling away under
Verilator both upstream and here, which is why the ports now run them by
default; seven in riscv-dv; one in Spike (the `menvcfgh` case above); and
eight limitations of cpptb itself, of which the discovery and timing-backend
defects were fixed in the framework and the rest are recorded.
`experiments/open_core_ports/FINDINGS.md` is the master list.

## Reproducing

Everything is pinned and fetched, never vendored. `sources.toml` records
exact commits, licenses, and the purpose of every dependency, and `fetch.py`
(standard library only) verifies and unpacks them. The per-port commands
live in each port's README; the shared prerequisites are the three `make`
targets shown [above](#spike-co-simulation). Ports write only under their
own directory, `work/`, or `results/`. Upstream sources under `deps/` are
never modified.
