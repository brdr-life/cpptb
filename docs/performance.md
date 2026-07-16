# Performance

Performance comparisons use an equivalent C++ DPI and pure SystemVerilog
testbench for every authoring feature. The peripheral suite additionally keeps
cocotb and C++ VPI implementations for four-mode comparisons.

The hard framework guard rejects a final C++ DPI to SystemVerilog process-wall
ratio above `1.10`. Measurements run serially, alternate pair order, record
load and resource evidence, and do not normalize measured samples.

```sh
make feature-benchmark FEATURE=signal_edge
make feature-regression
```

Shared CI is used for semantic and equivalence checks only. Reference
performance reports belong in `benchmarks/baselines/`; raw journals and
machine-specific command captures are local artifacts.

Historical scheduler experiments and their accepted or rejected rationale are
recorded in `benchmarks/authoring_core/OPTIMIZATION_NOTES.md` in the source
repository.

## Coverage layers

Performance and usability are checked at three different scales:

1. **Authoring kernels** isolate one construct, run long enough to suppress
   process-startup noise, and enforce the `1.10x` guard against an exact pure-SV
   twin.
2. **User-shaped examples** cover counter, multiclock, timer, FIFO scoreboard,
   APB register access, watchdog cancellation, fault injection, and rich packed
   data. Their C++ and pure-SV forms must match iterations, checks, simulation
   cycles, and failures exactly. These short scenarios are semantic integration
   tests rather than stable nanosecond gates.
3. **Heavy and open-source-core suites** exercise longer protocol and compute
   workloads where scheduler and transport costs can be measured in a realistic
   testbench. See [Heavy benchmarks](examples/heavy-benchmarks.md) and
   [Open-source core benchmarks](examples/open-source-cores.md).

## Test lifecycle checks

The `test_lifecycle` pair isolates the passing-check path. Each of 5,000,000
iterations performs one boolean condition and one equality comparison in both
C++ and pure SV, for 10,000,000 matching checks, no DUT transactions, and no
clock cycles. Both forms use one final 1 ps reporting step after the complete
loop; there is no per-check scheduler or DPI crossing.

The valid July 16, 2026 serial run passed at `0.983x` C++ DPI over pure SV,
with `0.976x` DPI-first, `1.021x` SV-first, `0.985x` independent, and `0.13%`
paired/independent disagreement. This is a machine-specific measurement; the
registry enforces the same `1.10x` hard guard as other non-waived features.

```sh
make feature-test FEATURE=test_lifecycle
make feature-benchmark FEATURE=test_lifecycle
```

## Dynamic process creation

Four exact C++/pure-SV pairs separate coroutine construction, core scheduling,
test lifecycle tracking, and real suspension. Each immediate-process pair runs
5,000,000 iterations; the suspending pair runs two communicating processes
with an event handshake. The pure-SV process twins use `fork ... join`.

| Pair | C++ DPI | Pure SV | C++ / SV |
| --- | ---: | ---: | ---: |
| Direct coroutine task | 23.03 ns | 3.26 ns | `7.052x` |
| Core scheduler process | 43.88 ns | 40.88 ns | **`1.080x`** |
| Lifecycle-tracked process | 49.88 ns | 41.39 ns | **`1.199x`** |
| Two suspending processes | 181.35 ns | 272.55 ns | **`0.661x`** |

These are machine-specific July 16, 2026 serial measurements. The direct-task
ratio is intentionally harsh: Verilator inlines the zero-time SV task to about
3 ns, while C++ still constructs and destroys a coroutine frame. Its absolute
delta is more useful than its ratio.

The core scheduler is within the `1.10x` guard. The remaining immediate-spawn
gap is lifecycle ownership, process provenance, exactly-once finalization, and
failure attribution. Replacing per-spawn ownership-vector maintenance with a
small reusable provenance record reduced the lifecycle-tracked C++ result from
56.68 ns to 49.88 ns, a 12.0% improvement. The lifecycle layer now costs about
6.00 ns over the core scheduler. The suspending workload is faster than its
pure-SV twin because useful scheduling work amortizes that fixed setup cost.

TLS relocation changes, embedding execution context in every process control,
and splitting process controls into pooled subclasses all regressed the core
path and were removed. A lazy scheduler-adoption path was not retained because
the decomposition showed that core scheduler adoption is already within the
guard; it would optimize the wrong layer. The lifecycle-tracked zero-time case
remains above the hard guard as an explicit performance issue.

Use `spawn()` for actual concurrent work, cancellation, or an independently
attributed process. For sequential helper composition, await the task directly:

```cpp
// Sequential composition: no independent process is needed.
const uint32_t value = co_await authored_value(iteration);

// Concurrent work: retain a cancellable, independently owned process.
auto driver = test.spawn(input_driver(dut, test));
co_await driver;
```

```sh
make feature-test FEATURE=dynamic_spawn_scheduler
make feature-benchmark FEATURE=dynamic_spawn_scheduler
make feature-test FEATURE=dynamic_spawn
make feature-benchmark FEATURE=dynamic_spawn
make feature-test FEATURE=dynamic_spawn_suspending
make feature-benchmark FEATURE=dynamic_spawn_suspending
```

## Bounded queue and synchronization

The `queue_sync` pair runs a capacity-one FIFO under sustained producer
backpressure. A two-credit semaphore bounds outstanding work, a deliberately
contended lock protects the consumer check, and every consumed item drives the
same DUT transaction. The C++ and pure-SV forms use `Queue`/`Semaphore`/`Lock`
and `mailbox`/`semaphore` respectively.

At 100,000 iterations, both forms reported 100,000 queue puts, queue gets,
lock acquisitions, semaphore acquisitions, and DUT transactions. They matched
200,002 checks, 500,003 simulated cycles, and the final checksum exactly.

The valid July 15, 2026 serial run passed at `0.788x` C++ DPI over pure SV,
with `0.784x` DPI-first, `0.803x` SV-first, `0.792x` independent, and `0.43%`
paired/independent disagreement. After making `Queue` the sole public FIFO
type, the unchanged unbounded `queue` control passed at `0.838x`; its earlier
control result was `0.824x`, a `1.7%` shift and below the `5%` investigation
threshold. These are machine-specific measurements; the registry continues to
enforce `1.10x` for both features.

```sh
make feature-test FEATURE=queue
make feature-benchmark FEATURE=queue
make feature-test FEATURE=queue_sync
make feature-benchmark FEATURE=queue_sync
```

## Timing-phase dispatch

The exact `timing_phases` pair performs one falling-edge wait, one
`ReadWrite`, one `ReadOnly`, one `NextTimeStep`, two settled combinational
checks, and two driven values per iteration. A July 14, 2026 profile at 100,000
iterations initially measured `1.675x` C++ DPI over pure SV.

Three retained changes reduced that ratio:

1. Removing a redundant read/write settle callback reduced scheduler steps,
   model evaluations, and VPI callbacks by 100,000 each.
2. The framework-only Verilator host loop stopped scanning unused value,
   timed, start-of-slot, and end-of-slot VPI callback classes.
3. Direct Verilator phase dispatch removed 300,000 one-shot VPI callback
   registrations while preserving the portable VPI fallback.

Consolidating callback legality state into one thread-local object removed
additional TLS resolver work. The final 32-pair run passed at `0.834x`, with
`0.823x` DPI-first, `0.834x` SV-first, `0.835x` independent, and `0.17%`
paired/independent disagreement. All 200,000 checks matched. This is a
machine-specific result; the registry's `1.10x` hard guard remains the
acceptance criterion.

### Portable timing experiments

Three pure-DPI timing transports were prototyped against that same
`timing_phases` workload. All leave simulator time ownership in generated
SystemVerilog and return pending phase requests in the existing DPI step
result:

- **Inline phase pump:** dispatches `ReadWrite` and `ReadOnly` immediately.
  It is fast structurally but invalid: `ReadOnly` can run before the DUT gets
  a settle turn. The semantic probe reported 1,000 failures in 2,000 checks.
- **NBA phase pump:** crosses an explicit generated NBA token barrier before
  dispatching `ReadWrite` or `ReadOnly`. It passes the strengthened timing
  conformance suite, including simultaneous unrelated clock edges and
  pre-/post-NBA observations.
- **Centralized calendar:** gives one generated SystemVerilog process ownership
  of compile-time-discovered clocks, framework timers, and phase dispatch.
  It uses the NBA token only when a settled phase is requested and retains
  deterministic timer-before-coincident-clock ordering.

The July 14, 2026 serialized comparison used the exact pure-SV twin:

| Timing backend | Semantic result | C++ / pure SV |
|---|---|---:|
| Direct Verilator dispatch | Pass | 0.831x |
| Portable VPI callbacks | Pass | 1.303x |
| Pure-DPI inline pump | Fail | Not benchmarked |
| Pure-DPI NBA pump | Pass | 1.356x |
| Pure-DPI centralized calendar | Pass | 0.976x |

The calendar reduced wall time by about 28% relative to the NBA pump and
cleared the `1.10x` hard guard. It remains an experiment while cross-simulator
semantics are unverified: standard DPI cannot discover arbitrary hidden DUT
events, so its `NextTimeStep` knowledge is limited to generated clocks,
framework timers, and explicitly observed signals. Full implementation details
and reproduction commands are in
`benchmarks/authoring_core/TIMING_BACKEND_EXPERIMENTS.md`.

## Scoped direct-force waiver

`force_direct` isolates one zero-time force, immediate readback, and release.
It has no scheduler resumption, protocol transaction, clock edge, or simulated
time advance. A generated callback-local cache removes the redundant exported
DPI read while preserving release invalidation and fresh reads in later DPI
callbacks. The exact one-million-iteration comparison still measured `1.135x`
because force and release each cross the exported-DPI boundary, while the pure
SystemVerilog twin executes in process.

The registry therefore carries one explicit waiver approved on 2026-07-14.
The raw `1.10x` failure remains visible, but this isolated feature may progress
only while its ratio is at most `1.20x`. Missing or malformed results, semantic
differences, invalid environments, and ratios above that waiver ceiling still
fail. All other authoring features retain the unmodified hard `1.10x` policy.

## Heavy four-mode comparison

The heavy suite runs independent reference models in pure SystemVerilog, C++
DPI, raw C++ VPI, and Cocotb against one shared DUT:

```sh
make framework-comparison-heavy-benchmark
```

The three workloads exercise different verification shapes:

- **32-tap streaming FIR:** signed samples, 32 software MACs per sample,
  history state, and one output check per accepted sample.
- **Variable-length packet CRC32:** 32-95 byte frames, byte-wise CRC reference
  calculation, framing, and one result check per packet.
- **4x4 signed matrix accelerator:** block loading, 64 software MACs, 16
  indexed result words, and 32 checks per block.

The July 14, 2026 reference run used Verilator 5.050 and Cocotb 2.0.1. Values
are median whole-process wall time over four rotated, serialized samples after
one warm-up per mode and are normalized to the exact matching pure-SV
testbench. The observed one-minute load average was 3.10-3.46.

| Workload | Work units | Sim cycles | Pure SV | C++ DPI | C++ VPI | Cocotb |
|---|---:|---:|---:|---:|---:|---:|
| 32-tap streaming FIR | 100,000 samples | 100,004 | 47.8 ms / 1.00x | 84.1 ms / 1.76x | 120.2 ms / 2.51x | 2684.5 ms / 56.16x |
| Variable-length packet CRC32 | 2,000 packets | 126,620 | 41.1 ms / 1.00x | 70.1 ms / 1.70x | 117.4 ms / 2.86x | 2281.2 ms / 55.51x |
| 4x4 signed matrix accelerator | 2,000 blocks | 100,003 | 36.5 ms / 1.00x | 66.4 ms / 1.82x | 103.3 ms / 2.83x | 1989.1 ms / 54.50x |

These are machine-specific reference values, not universal simulator claims.
The useful comparison is within each row: all four versions report identical
transactions, checks, simulation cycles, checksum, and failures. The heavy
suite records the 1.10x C++ DPI guard as advisory unless the runner receives
`--enforce-guard`; this reference run exceeds it on all three workloads.

The complete testbench sources and machine-readable sample journal are under
`benchmarks/framework_comparison/heavy_suite/`.

## Open-source core comparison

The open-core suite uses the same four modes and semantic gate with pinned,
unmodified upstream RTL:

- PicoRV32 executing an RV32I firmware kernel;
- secworks AES programmed through its register interface;
- verilog-ethernet's 64-bit AXI-stream FCS core.

```sh
make framework-comparison-open-cores-test
make framework-comparison-open-cores-benchmark
```

Each workload elaborates only its selected core. This keeps the runtime
comparison honest: unused third-party RTL cannot increase model evaluation or
DPI transport cost. See [Open-source core benchmarks](examples/open-source-cores.md)
for the user-facing sequences and provenance.

The following reference run used Verilator 5.050 and Cocotb 2.0.1. Values are
median whole-process wall time over four serialized, mode-rotated samples.

| Workload | Work units | Sim cycles | Pure SV | C++ DPI | C++ VPI | Cocotb |
|---|---:|---:|---:|---:|---:|---:|
| PicoRV32 firmware kernel | 20,000 loops | 920,043 | 490.5 ms / 1.00x | 603.4 ms / 1.23x | 1092.8 ms / 2.23x | 4062.0 ms / 8.28x |
| secworks AES-128 | 4,000 blocks | 252,032 | 202.5 ms / 1.00x | 290.1 ms / 1.43x | 512.2 ms / 2.53x | 7000.4 ms / 34.57x |
| 64-bit Ethernet FCS | 2,000 frames | 209,616 | 407.9 ms / 1.00x | 366.9 ms / 0.90x | 1325.6 ms / 3.25x | 13111.4 ms / 32.14x |

The DPI/pure-SV advisory guard passes for Ethernet and reports the PicoRV32
and AES ratios above `1.10`. The raw journal records every sample and its
one-minute load average under
`benchmarks/framework_comparison/open_cores/results/latest.jsonl`; this run
observed load averages from 5.7 to 9.2 on an 8-logical-CPU host. Treat the
absolute times as a local reference and the within-row ratios as the useful
comparison.
