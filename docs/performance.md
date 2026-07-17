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
registry enforces the `1.10x` hard guard for this feature.

```sh
make feature-test FEATURE=test_lifecycle
make feature-benchmark FEATURE=test_lifecycle
```

## Dynamic process creation

Four exact C++/pure-SV pairs separate coroutine construction, core scheduling,
test lifecycle tracking, and real suspension. Each immediate-process pair runs
5,000,000 iterations; the suspending pair runs two communicating processes
with an event handshake. The pure-SV process twins use `fork ... join`.

| Pair | C++ DPI median | Pure SV median | Paired C++ / SV |
| --- | ---: | ---: | ---: |
| Direct coroutine task | 23.03 ns | 3.26 ns | `7.052x` |
| Core scheduler process | 43.88 ns | 40.88 ns | **`1.080x`** |
| Lifecycle-tracked process | 49.88 ns | 41.39 ns | **`1.199x`** |
| Two suspending processes | 181.35 ns | 272.55 ns | **`0.661x`** |

These are machine-specific July 16, 2026 serial measurements. The direct-task
ratio is intentionally harsh: Verilator inlines the zero-time SV task to about
3 ns, while C++ still constructs and destroys a coroutine frame. Its absolute
delta is more useful than its ratio. The two time columns are independent
per-mode medians; the ratio column is the median of adjacent paired ratios.

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

`dynamic_task` is a diagnostic control rather than a framework gate. Verilator
can inline its zero-time SV task while C++ must still construct a coroutine, so
the ratio describes a compiler optimization mismatch rather than spawned
process overhead.

The `dynamic_monitor` pair covers the common verification shape that the
microbenchmarks omit. Two long-lived lifecycle-owned processes observe every
`rsp_valid` edge, one transfers response values through a capacity-eight
`Queue`, foreground stimulus drives 100,000 DUT requests, and teardown cancels
both observers. The pure-SV twin uses the same edge waits, a bounded mailbox,
`fork...join_none`, and `disable`.

Both forms reported two spawned processes, 100,000 queue puts and gets,
100,000 transactions, 100,003 checks, 500,003 cycles, and the same checksum.
The final valid July 16, 2026 run passed at **`0.744x`** C++ DPI over pure SV,
with `0.742x` DPI-first, `0.748x` SV-first, `0.742x` independent, and `0.29%`
paired/independent disagreement. Median process time was 2.85 us per
transaction for C++ DPI and 3.85 us for pure SV on this machine.

The tabs below show the authored process bodies from the complete runnable
benchmark pair. Common DUT reset, deterministic stimulus helpers, final DUT
count checks, and result reporting are omitted from the excerpts. The commands
below build and execute the complete C++ DPI and pure-SV sources.

<div class="cpptb-code-tabs" data-tabs="2" data-tab-group="simulator" data-tab-label="Persistent monitor benchmark implementation"></div>

<div class="cpptb-code-tab-label">cpptb (C++ DPI)</div>

```cpp
Task<void> response_monitor(Context context, Queue<uint32_t>& observed) {
    while (true) {
        co_await RisingEdge{context.dut.rsp_valid};
        co_await Delay{1_ps};
        co_await observed.put(context.dut.rsp_data.get());
        ++context.result.features.queue_put;
    }
}

Task<void> response_edge_watcher(Context context, uint64_t& response_edges) {
    while (true) {
        co_await RisingEdge{context.dut.rsp_valid};
        ++response_edges;
    }
}

Task<void> run_dynamic_monitor(Context context) {
    TestContext test{context.scheduler, context.result};
    Queue<uint32_t> observed{8};
    uint64_t response_edges = 0;
    context.result.spawned_processes += 2;
    auto monitor = test.spawn(response_monitor(context, observed));
    auto watcher = test.spawn(response_edge_watcher(context, response_edges));

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        co_await drive_request(context, stimulus(iteration));
        const uint32_t response = co_await observed.get();
        ++context.result.features.queue_get;
        check(context, "monitored response", response,
              expected_response(iteration));
        context.result.checksum =
            (context.result.checksum ^ response) * 0x0100'0193u;
        ++context.result.transactions;
    }

    monitor.cancel();
    watcher.cancel();
    co_await monitor;
    co_await watcher;
    check64(context, "observed response edges", response_edges,
            context.iterations);
}
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
task automatic dynamic_response_monitor();
  logic [31:0] response;
  forever begin
    @(posedge rsp_valid);
    #1ps;
    response = rsp_data;
    dynamic_monitor_queue.put(response);
    queue_put_count++;
  end
endtask

task automatic dynamic_response_watcher();
  forever begin
    @(posedge rsp_valid);
    dynamic_monitor_edges++;
  end
endtask

task automatic run_dynamic_monitor();
  logic [31:0] response;
  dynamic_monitor_queue = new(8);
  spawned_processes += 2;
  fork : dynamic_monitor_processes
    dynamic_response_monitor();
    dynamic_response_watcher();
  join_none

  for (int unsigned i = 0; i < iterations; i++) begin
    drive_request(stimulus(i));
    dynamic_monitor_queue.get(response);
    queue_get_count++;
    check32(response, expected_response(i), "monitored response");
    checksum = (checksum ^ response) * 32'h0100_0193;
    transactions++;
  end

  disable dynamic_monitor_processes;
  check64(dynamic_monitor_edges, iterations, "observed response edges");
endtask
```

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
make feature-test FEATURE=dynamic_monitor
make feature-benchmark FEATURE=dynamic_monitor
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

## Transaction analysis fan-out

The `analysis_fanout` pair measures the first reusable component slice without
mixing in repeated process creation. For every one of 100,000 DUT responses,
the test publishes one expected transaction, then publishes the observed
transaction to both an in-order scoreboard and a capacity-eight audit buffer.
A single persistent monitor performs the DUT sampling in both implementations.

The C++ and pure-SV forms match `200,000` analysis writes, `300,000`
deliveries, `100,000` transactions, `100,006` checks, one spawned process,
and the final checksum. The valid July 16, 2026 run passed at `0.712x` C++ DPI
over pure SV, with `0.719x` DPI-first, `0.707x` SV-first, `0.716x`
independent, and `0.54%` paired/independent disagreement. These values are
machine-specific; the registry enforces the same `1.10x` hard guard as other
framework features.

```sh
make feature-test FEATURE=analysis_fanout
make feature-benchmark FEATURE=analysis_fanout
```

## Deterministic random stimulus

The `random_stimulus` pair performs the same mixed random workload for every
DUT request: one full-width `randint`, one four-way `weighted_choice`, one
65-bit `randbits`, and one four-element `shuffle`. Both implementations use
seed `1` and the versioned `xoshiro256ss-v1` transition, consume words in the
same order, drive the resulting payload through the DUT, and compare all
100,000 responses plus the final checksum.

The valid July 17, 2026 run passed at `0.687x` C++ DPI over pure SV, with
`0.684x` DPI-first, `0.693x` SV-first, `0.694x` independent, and `1.08%`
paired/independent disagreement. These values are machine-specific; the
registry retains the ordinary `1.10x` hard guard.

```sh
make feature-test FEATURE=random_stimulus
make feature-benchmark FEATURE=random_stimulus
```

## Constrained-random packets

The `constrained_packet` pair declares opcode, length, address, and tag fields.
Both implementations apply identical range, modulo-alignment, and cross-field
constraints, consume the same `xoshiro256ss-v1` words, reject the same
candidates, and drive 100,000 accepted packets through the DUT. Every response
and the final checksum must match before timing is considered.

The C++ default adaptive backend caches the immutable constraint problem,
folds direct bounds into candidate domains, omits checks already guaranteed by
those domains, and keeps assignments of up to eight fields inline. The
sampling fast path completes every packet without invoking a solver. These are
generic runtime optimizations; the packet transaction has no benchmark-only
fast path.

The valid July 17, 2026 formal run measured `0.995x` C++ DPI over pure SV, with
`0.998x` DPI-first, `0.975x` SV-first, `0.998x` independent, and `0.36%`
paired/independent disagreement. It passes the `1.10x` hard guard.

```sh
make feature-test FEATURE=constrained_packet
make feature-benchmark FEATURE=constrained_packet
```

## Constraint extensions

The `constraint_extensions` pair exercises membership sets, a weighted value
and range distribution, a soft default, a disabled constraint, a nested
randomized object, a fixed randomized array, and a 65-bit randomized value.
The C++ and pure-SystemVerilog forms consume the same random words, apply the
same whole-candidate rejection rule, drive 100,000 transactions through the
same DUT, and require exact response and checksum agreement.

The July 17, 2026 run measured `0.816x` C++ DPI over pure SV, with `0.813x`
DPI-first, `0.824x` SV-first, `0.824x` independent, and `0.88%`
paired/independent disagreement. It passed the ratio guard, but is published as
load-inconclusive because normalized one-minute host load reached `1.211`.

```sh
make feature-test FEATURE=constraint_extensions
make feature-benchmark FEATURE=constraint_extensions
```

## Functional coverage sampling

The `coverage_sampling` pair samples one transaction for every DUT transfer.
Both implementations perform equivalent ordinary, ignore, illegal,
transition, and 3-by-3 cross-bin accounting. Five final checks retain the
sample total, point accounting, transition count, and cross count so the work
cannot be optimized away before timing.

The valid July 17, 2026 run measured `0.705x` C++ DPI over pure SV, with
`0.702x` DPI-first, `0.716x` SV-first, `0.706x` independent, and `0.08%`
paired/independent disagreement. It passes the standard `1.10x` hard guard.

```sh
make feature-test FEATURE=coverage_sampling
make feature-benchmark FEATURE=coverage_sampling
```

## APB verification components

The `apb_component` pair performs 100,000 APB writes and matching reads through
a byte-enabled register array. Both implementations execute the same setup and
access phases, passive transaction monitoring, protocol checks, in-order
scoreboard comparisons, response checks, and checksum updates. The C++ side
uses the public `cpptb_vc` master, monitor, checker, analysis port, and
scoreboard rather than benchmark-local helpers.

The valid July 17, 2026 run measured `0.916x` C++ DPI over pure SV, with
`0.925x` DPI-first, `0.899x` SV-first, `0.920x` independent, and `0.45%`
paired/independent disagreement. It passes the standard `1.10x` hard guard.

```sh
make feature-test FEATURE=apb_component
make feature-benchmark FEATURE=apb_component
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

The July 16, 2026 reference run used Verilator 5.050 and Cocotb 2.0.1. Values
are median whole-process wall time over four serialized, mode-rotated samples.

| Workload | Work units | Sim cycles | Pure SV | C++ DPI | C++ VPI | Cocotb |
|---|---:|---:|---:|---:|---:|---:|
| PicoRV32 firmware kernel | 20,000 loops | 920,043 | 274.8 ms / 1.00x | 357.7 ms / 1.30x | 625.2 ms / 2.27x | 2434.2 ms / 8.86x |
| secworks AES-128 | 4,000 blocks | 252,032 | 122.2 ms / 1.00x | 172.6 ms / 1.41x | 315.1 ms / 2.58x | 3986.8 ms / 32.62x |
| 64-bit Ethernet FCS | 2,000 frames | 209,616 | 261.3 ms / 1.00x | 251.6 ms / 0.96x | 841.6 ms / 3.22x | 7110.3 ms / 27.21x |

The DPI/pure-SV advisory guard passes for Ethernet and reports the PicoRV32
and AES ratios above `1.10`. The raw journal records every sample and its
one-minute load average under
`benchmarks/framework_comparison/open_cores/results/latest.jsonl`; this run
observed load averages from 3.21 to 4.31 on an 8-logical-CPU host. Treat the
absolute times as a local reference and the within-row ratios as the useful
comparison.
