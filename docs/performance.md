# Performance

Performance comparisons use an equivalent C++ DPI and pure SystemVerilog
testbench for every authoring feature. The peripheral suite additionally keeps
cocotb and C++ VPI implementations for four-mode comparisons.

The hard framework guard rejects a final C++ DPI to SystemVerilog process-wall
ratio above `1.10`. Measurements run serially, alternate pair order, record
load and resource evidence, and do not normalize measured samples. Hard-gate
runs require source, executable, build-flag, compiler, and Verilator stamps;
refuse a normalized one-minute load above `0.30`; and require the paired
child-CPU ratio to corroborate wall time. Samples are never silently discarded:
abnormal CPU-time placement or an invalid host probe invalidates the complete
run and returns a nonzero status.

```sh
make feature-benchmark FEATURE=signal_edge
make feature-regression
```

Shared CI is used for semantic and equivalence checks only. Reference
performance reports belong in `benchmarks/baselines/`; raw journals and
machine-specific command captures are local artifacts.

## Build policy for measured binaries

Every measured binary in every suite and every mode is compiled at
`CPPTB_BENCH_OPT_FAST`, which defaults to `-O3`. That means all four modes:
pure SystemVerilog, C++ DPI, C++ VPI, and cocotb, whose runners read the same
variable from the environment because cocotb drives Verilator itself.

Each mode needs the setting applied twice. Verilator applies `OPT_FAST` only to
the files it generates, and adds no optimization flag of its own to testbench
sources given on the command line. A mode that receives only `-MAKEFLAGS
OPT_FAST` therefore runs an unoptimized testbench against an optimized model,
and a mode that receives neither is slower still. Because the guard compares
modes against each other, optimizing one and not another does not merely add
noise: it changes the reported result.

Override `CPPTB_BENCH_OPT_FAST` to sweep every suite, or one suite's own
`*_OPT_FAST` variable to change a single suite:

```sh
make feature-benchmark FEATURE=event CPPTB_BENCH_OPT_FAST=-O2
```

Baselines recorded before this policy measured an unoptimized C++ side against
an optimized model, so their ratios overstate C++ DPI cost and are not
comparable with later runs.

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

The valid July 18, 2026 serial run passed at `0.983x` C++ DPI over pure SV,
with `0.984x` DPI-first, `0.983x` SV-first, `0.992x` independent, and `0.96%`
paired/independent disagreement. This is a machine-specific measurement; the
registry enforces the `1.10x` hard guard for this feature.

```sh
make feature-test FEATURE=test_lifecycle
make feature-benchmark FEATURE=test_lifecycle
```

## Structured logging

The `structured_logging` pair models a hot scoreboard loop with realistic
sparse output. Each of 5,000,000 iterations attempts one lazy debug message
below the active `Info` threshold. Every 1,024 iterations emits one constant
info record through a counting sink. Both implementations use one owned
process and check the emitted count, provenance count, metadata count, and
disabled-factory count.

The `structured_logging` C++ path constructs no disabled messages and retains
no log records. The pure-SV peer performs the same runtime level comparisons
and sparse sink updates. Exact semantic parity passes at 5,000,000 iterations.
The first formal timing attempt was rejected before sampling because normalized
one-minute host load was `0.644`, above the `0.300` admission limit; no
performance ratio is published from that run.

The separate `structured_log_history` pair retains each enabled record rather
than hiding storage cost inside the baseline. Both implementations own the
level, message, scope, test name, source and process provenance, sequence, and
simulation time. Six checks compare output count, output metadata, retained
count, chronological order, retained metadata, and disabled lazy formatting.
The exact 5,000,000-iteration semantic workload passes. Its first formal timing
attempt was rejected during environment settling: normalized one-minute load
started at `0.526`, above the `0.300` admission limit, and did not settle before
the runner timeout. No retained-history ratio is published from that run.

The `mixed_logging` pair exercises the complete cross-language path. Every
transaction attempts one disabled C++ debug message and one disabled RTL debug
message. Every 1,024 transactions, C++ and RTL each emit one retained info
record. Both sides validate the same request/response traffic, record count,
language origin, source metadata, and chronological ordering. This keeps the
DPI callback frequency and retained metadata visible instead of timing an
empty bridge. Its 5,000,000-transaction semantic pair passes exactly. The
first formal timing attempt was rejected before sampling after normalized
one-minute load remained above the `0.300` admission limit for 60 seconds
(`0.480` initially and `0.882` at timeout), so no mixed-language ratio is
published from that run.

```sh
make feature-test FEATURE=structured_logging
make feature-benchmark FEATURE=structured_logging
make feature-test FEATURE=structured_log_history
make feature-benchmark FEATURE=structured_log_history
make feature-test FEATURE=mixed_logging
make feature-benchmark FEATURE=mixed_logging
```

See [Structured logging](logging.md) for the user API and sink contract.

## Dynamic process creation

Four exact C++/pure-SV pairs separate coroutine construction, core scheduling,
test lifecycle tracking, and real suspension. Each immediate-process pair runs
5,000,000 iterations; the suspending pair runs two communicating processes
with an event handshake. The pure-SV process twins use `fork ... join`.

| Pair | C++ DPI median | Pure SV median | Paired C++ / SV |
| --- | ---: | ---: | ---: |
| Direct coroutine task | 21.71 ns | 1.37 ns | `15.764x` |
| Core scheduler process | 43.60 ns | 40.34 ns | **`1.072x`** |
| Lifecycle-tracked process | 41.22 ns | 38.96 ns | **`1.067x`** |
| Two suspending processes | 166.71 ns | 279.08 ns | **`0.601x`** |

These are historical machine-specific July 18, 2026 serial measurements under
the earlier admission policy, not current formal gate results. The direct-task
ratio is intentionally harsh: Verilator inlines the zero-time SV task to about
1 ns, while C++ still constructs and destroys a coroutine frame. Its absolute
delta is more useful than its ratio. The two time columns are independent
per-mode medians; the ratio column is the median of adjacent paired ratios.

The core scheduler path remains within the `1.10x` guard. A later fresh-build
screen measured it at `1.085x`, with a `1.086x` child-CPU ratio. The
lifecycle-owned immediate-process pair is now diagnostic because its child
performs one zero-time check and exits. Its fresh CPU-coherent `1.131x` screen
characterizes minimum process cost but is not a framework release gate.

The retained lifecycle design keeps provenance in scheduler
process state, reports exceptional completions through the test callback, and
skips that callback for successful processes. Test-state deletion is deferred
once at teardown if owned work remains, rather than extending and releasing
test-state ownership for every spawned process. Lifecycle execution context is
stored only in owned process controls, leaving the generic scheduler control
compact. Failure attribution, cancellation, random-stream cleanup, and
detached-process lifetime remain covered by unit regressions.

Every authoring binary now carries a generated provenance stamp beside the
executable. `--skip-build` refuses to run when the binary hash, any declared
source hash, build flags, compiler version, or Verilator version differs. This
prevents a dirty-tree benchmark from timing a binary produced before the current
runtime headers or with leftover experimental optimization flags.

The suspending workload is faster than its pure-SV twin because useful
scheduling work amortizes setup and Verilator's generated SV event machinery
is more expensive for this exact handshake. Rejected TLS models, persistent
pooled controls, and duplicated owned-spawn implementations remain documented
in the optimization notes.

`dynamic_task` and `dynamic_spawn` are diagnostic controls rather than
framework gates. Verilator can inline their zero-time SV work aggressively,
while C++ must still construct a coroutine and, for `dynamic_spawn`, attach
lifecycle ownership. Their ratios characterize fixed costs rather than typical
verification throughput.

They deliberately have no ratio waiver ceiling. The pure-SV denominator can
collapse toward a compiler-inlined constant as the zero-time child body changes,
so a ratio ceiling would not provide a stable lifecycle regression bound. The
absolute per-child cost remains a useful diagnostic; the hard-gated suspending,
persistent-monitor, and finite-pipeline pairs bound lifecycle performance under
actual verification work.

The `dynamic_monitor` pair covers the common verification shape that the
microbenchmarks omit. Two long-lived lifecycle-owned processes observe every
`rsp_valid` edge, one transfers response values through a capacity-eight
`Queue`, foreground stimulus drives 100,000 DUT requests, and teardown cancels
both observers. The pure-SV twin uses the same edge waits, a bounded mailbox,
`fork...join_none`, and `disable`.

This persistent workload is a hard `1.10x` gate. Its historical result below
predates the current build-provenance and `0.30` load-admission policy, so an
admitted current-policy rerun remains required before publishing a new formal
number.

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

### Finite process pipeline

`process_pipeline` is the finite-lifetime hard-gate companion. A spawned driver
publishes expected responses and drives 100,000 DUT requests, a spawned worker
samples every response, and a spawned scoreboard consumes both bounded streams.
All three processes suspend repeatedly and complete naturally before the parent
joins them. The exact C++ and pure-SV forms report `N` transactions, `N + 4`
checks, three spawned processes, `2N` queue puts, and `2N` queue gets.

<div class="cpptb-code-tabs" data-tabs="2" data-tab-group="simulator" data-tab-label="Finite process pipeline implementation"></div>

<div class="cpptb-code-tab-label">cpptb (C++ DPI)</div>

```cpp
Task<void> process_pipeline_driver(Context context,
                                   Queue<uint32_t>& expected) {
    for (uint32_t i = 0; i < context.iterations; ++i) {
        co_await expected.put(expected_response(i));
        co_await drive_request(context, stimulus(i));
    }
}

Task<void> process_pipeline_worker(Context context,
                                   Queue<uint32_t>& observed) {
    for (uint32_t i = 0; i < context.iterations; ++i) {
        co_await RisingEdge{context.dut.rsp_valid};
        co_await Delay{1_ps};
        co_await observed.put(context.dut.rsp_data.get());
    }
}

Task<void> process_pipeline_scoreboard(Context context,
                                       Queue<uint32_t>& expected,
                                       Queue<uint32_t>& observed) {
    for (uint32_t i = 0; i < context.iterations; ++i) {
        const auto wanted = co_await expected.get();
        const auto actual = co_await observed.get();
        check(context, "pipeline response", actual, wanted);
    }
}

Queue<uint32_t> expected_values{8};
Queue<uint32_t> observed_values{8};

auto driver = test.spawn(
    process_pipeline_driver(context, expected_values));
auto worker = test.spawn(
    process_pipeline_worker(context, observed_values));
auto scoreboard = test.spawn(
    process_pipeline_scoreboard(context, expected_values, observed_values));

co_await driver;
co_await worker;
co_await scoreboard;
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
task automatic process_pipeline_driver();
  for (int unsigned i = 0; i < iterations; i++) begin
    process_expected_queue.put(expected_response(i));
    drive_request(stimulus(i));
  end
endtask

task automatic process_pipeline_worker();
  logic [31:0] response;
  for (int unsigned i = 0; i < iterations; i++) begin
    @(posedge rsp_valid);
    #1ps;
    response = rsp_data;
    process_observed_queue.put(response);
  end
endtask

task automatic process_pipeline_scoreboard();
  logic [31:0] expected;
  logic [31:0] actual;
  for (int unsigned i = 0; i < iterations; i++) begin
    process_expected_queue.get(expected);
    process_observed_queue.get(actual);
    check32(actual, expected, "pipeline response");
  end
endtask

process_expected_queue = new(8);
process_observed_queue = new(8);

fork
  process_pipeline_driver();
  process_pipeline_worker();
  process_pipeline_scoreboard();
join
```

The complete runnable implementations are in
`benchmarks/authoring_core/testbenches/cpp_dpi/testbench.cpp` and
`benchmarks/authoring_core/testbenches/systemverilog/authoring_core_sv_tb.sv`.

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
make feature-test FEATURE=process_pipeline
make feature-benchmark FEATURE=process_pipeline
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
load-inconclusive because normalized one-minute host load reached `1.211`. An
admitted rerun remains pending under the current `0.30` admission policy.

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

The July 17, 2026 baseline measured `0.916x` C++ DPI over pure SV, but it
predates the monitor-owned observation envelope introduced with transaction
recording. The current semantic pair passes at 100,000 iterations with exact
work. A July 20 remeasurement was rejected as `invalid_environment`, so no
current ratio is published and the standard `1.10x` hard guard remains pending
for the next admitted serial run.

```sh
make feature-test FEATURE=apb_component
make feature-benchmark FEATURE=apb_component
```

## Typed transaction recording

The `transaction_recording` pair extends the APB workload with one typed
observation feeding both an in-order scoreboard and an in-memory transaction
sink. At the default 100,000 iterations, each implementation completes
100,000 writes and matching reads and retains 200,000 records containing the
same stream name, transaction type, sequence, begin/end times, completion
disposition, and formatted JSON payload.

The semantic pair passes with exact counts and checksum. The July 20, 2026
timing attempt was rejected as `invalid_environment` because normalized host
load exceeded the `0.30` limit, so no ratio is published from that run. The
standard `1.10x` hard guard remains active for the next admitted serial run.

```sh
make feature-test FEATURE=transaction_recording
make feature-benchmark FEATURE=transaction_recording
```

## Sparse memory prediction

The `memory_model` pair retains the same APB setup/access phases, monitor,
protocol checker, response checks, transaction count, and checksum as the APB
component workload. It replaces the hand-authored expected transaction queue
with sparse byte storage, region decoding, byte-enable updates, and passive
read/write prediction in both C++ and pure SystemVerilog.

The 100,000-iteration semantic run passes with exact counts and checksum. The
July 18, 2026 timing attempt was rejected as `invalid_environment` after the
host-load settle window timed out, so no ratio is published from that run.
The `1.10x` hard gate remains active for the next admitted serial run.

```sh
make feature-test FEATURE=memory_model
make feature-benchmark FEATURE=memory_model
```

## Direct sparse memory operations

The `memory_model_direct` pair isolates the sparse memory container from APB
and scheduler timing. Each iteration performs one byte-enabled word write and
one word read, validates operation status and returned data, and updates the
same checksum in C++ and pure SystemVerilog. Neither implementation advances
simulation time or accesses the DUT.

The 100,000-iteration semantic run matches exactly at `200,000` operations,
`300,002` checks, zero simulated cycles, and the same checksum. This benchmark
answers whether sparse storage and prediction add host-language overhead. Read
it alongside `memory_model`, which measures the realistic integration cost of
the model behind APB components and scheduler activity.

```sh
make feature-test FEATURE=memory_model_direct
make feature-benchmark FEATURE=memory_model_direct
```

A timing ratio is published only after the serial runner admits host load and
the result passes the repository's `1.10x` hard guard.

## Generated register memory

The exact `register_memory` pair isolates the generated-memory hierarchy path.
Each iteration writes a four-entry span through
`AccessPath::Backdoor`, reads the same four entries into caller-owned storage,
checks both completed counts and all four values, and applies the same checksum
updates. The pure-SystemVerilog twin performs the same four deposits and four
reads against the same DUT array. Neither side advances simulation time or
issues a bus transaction.

The C++ test rotates through all three supported coordinate forms: a lightweight
`memory.slice(first, 4)` view, `read/write_offset(byte_offset, span)`, and
`read/write_absolute(address, span)`. The SystemVerilog twin performs the same
index, offset-to-index, and absolute-address-to-index calculations before the
same four deposits and reads. This keeps every chunk-addressing form in the
measured path without changing the workload.

```sh
make feature-test FEATURE=register_memory
make feature-benchmark FEATURE=register_memory
```

The semantic pair passes at 10,000,000 iterations with `60,000,002` checks,
zero simulated cycles, and matching checksums. The timing default is also
10,000,000 iterations so process startup is small relative to the measured
memory work.
Each side still performs four deposits and four reads per iteration. The
generated C++ transport groups those adjacent operations into standard DPI
packed blocks of up to four entries; the pure-SV twin performs the operations
directly against the same array.

Backdoor register-memory operations complete inline beneath their common
awaitable interface, while frontdoor operations retain normal asynchronous bus
scheduling. The batching and immediate-ready paths do not change checks,
checksum, access policy, bounds diagnostics, or simulated time. A ratio is
published only from a serial run admitted under the normal host-load policy
and must satisfy the unmodified `1.10x` hard guard.

## Standard register sequences

The exact `register_sequences` pair exercises the optional reusable RAL policy
layer. Each iteration performs frontdoor and backdoor reset checks, a mixed
backdoor-to-frontdoor and frontdoor-to-backdoor access check, and an eight-bit
bit-bash through both access paths. Both sides restore the original value,
check the same transport outcomes and data, count the same 21 frontdoor
operations, and apply the same checksum update.

```sh
make feature-test FEATURE=register_sequences
make feature-benchmark FEATURE=register_sequences
```

The semantic pair passes at 100,000 iterations with `2,100,000` frontdoor
operations, `4,600,002` checks, zero simulated cycles, and matching checksums.
The formal timing command retains the standard `1.10x` hard guard. The latest
attempt was rejected as `invalid_environment` because host load did not enter
the admitted window; it is not a performance result.

## Arbitrary-width register models

The exact `register_wide` pair covers a 128-bit register and a 128-bit
register-backed memory over a 32-bit transport. Each iteration performs full
frontdoor writes and reads, generated-style raw backdoors, and passive
prediction across all four transfer addresses. Its pure-SystemVerilog twin
performs the same 20 transport/accounting operations and six checks.

```sh
make feature-test FEATURE=register_wide
make feature-benchmark FEATURE=register_wide
```

The 100,000-iteration semantic contract is `2,000,000` transactions and
`600,002` checks with the complete 128-bit values compared on both sides.

## Register access coverage

The exact `register_coverage` pair observes the same ten frontdoor register and
memory transactions per iteration, including byte enables, legal field access,
memory indices, one failed transfer, and one unmapped transfer. C++ uses the
opt-in `RegisterAccessCoverage` subscriber; pure SV updates the equivalent
coverage counters directly. Final snapshots compare every register, field,
memory, path, and error counter in 12 checks.

```sh
make feature-test FEATURE=register_coverage
make feature-benchmark FEATURE=register_coverage
```

Coverage snapshots allocate only when requested. A model that does not
construct the subscriber executes no coverage path, so unused coverage is not
charged to unrelated RAL kernels.

## Register maps and custom frontdoors

The exact `register_maps` pair runs primary and alias register views, a custom
register frontdoor, an aliased register memory, and a custom memory frontdoor.
Both implementations issue ten transactions and eight checks per iteration
while preserving one logical mirror.

```sh
make feature-test FEATURE=register_maps
make feature-benchmark FEATURE=register_maps
```

The 100,000-iteration semantic pair matches at `1,000,000` transactions and
`800,002` checks. The latest timing attempt was rejected by the host-load
admission window, so no timing ratio is published from it.

## User-defined register effects

The exact `register_user_effects` pair applies the same XOR-on-write and
invert-on-read policy to user-defined effect bits. The C++ side exercises
`RegisterUserEffectPolicy`; the pure-SV side evaluates the same equations
directly. Each iteration performs two transactions and four value/validity
checks. Separate unit and generated-model tests cover the no-policy unknown-bit
rule and wide-register prediction without inflating this focused hot path.

```sh
make feature-test FEATURE=register_user_effects
make feature-benchmark FEATURE=register_user_effects
```

The 100,000-iteration semantic pair matches at `200,000` transactions and
`400,002` checks. Profiling the 10,000,000-iteration kernel reduced the C++
runtime from `3.362 s` to a `0.994 s` diagnostic median. The matching pure-SV
median was `0.400 s`, giving a raw `2.49x` ratio and a `70.4%` reduction in the
C++ runtime. Retained changes batch user effects per field, reuse the
single-transfer write prediction at commit, cache register metadata, and avoid
thread-local lookup in the coroutine-frame pool.

This zero-time pair measures the absolute cost of the C++ register abstraction:
the SV compiler can inline its policy equations, mirror, and validity updates,
whereas C++ deliberately retains virtual policy dispatch, locking, and
coroutine ownership. It is semantically exact but not abstraction-equivalent.
A subsequent formal run was rejected before sampling because normalized host
load was `0.934`, above the `0.300` admission limit. Therefore `2.49x` is a
diagnostic, not publishable formal evidence. The registry runs this zero-time
abstraction-versus-inlined-equations kernel at 10,000,000 iterations with a
diagnostic policy; the matched timed secworks AES integration retains the
release-facing guard.

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

## Interface semantic pair

The `dpi_interfaces` integration entry runs the same eight checks and one
primary-clock cycle in C++ DPI and pure SystemVerilog. It covers a
parameterized modport, a two-element interface array, two independently
registered clocks, an interface-member inout, and a top-level inout:

```sh
make feature-test FEATURE=dpi_interfaces
make feature-benchmark FEATURE=dpi_interfaces
```

This short example is an exact semantic gate, not a stable timing ratio. It
must match `iterations`, `checks`, `sim_cycles`, and `failures` exactly. The
long-running authoring and open-core suites remain the performance-regression
signal for changes to shared scheduler and transport paths.

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

## Register-model ground truth

The secworks AES register-model suite adds a stricter correctness oracle. Its
unchanged upstream top-level testbench, generated cpptb `RegModel` bench, and
matched pure-SV bench must produce the same ordered 720-event register trace,
20 NIST cases, 80 checked words, and checksum before performance is measured.

```sh
make secworks-aes-regmodel-equivalence
make secworks-aes-regmodel-benchmark
```

The July 18, 2026 diagnostic run used Verilator 5.050 and symmetric
`OPT_FAST=-O3`. Each process executed 180 complete suites, or 3,600 AES cases;
15 measured samples were serialized and order-rotated after warm-up. The
one-minute load average moved from 3.74 to 4.08 on eight logical CPUs, above
the current `0.30` normalized-load admission limit.

| Workload | Pure SV | C++ DPI generated RegModel | Ratio |
|---|---:|---:|---:|
| secworks AES, 3,600 cases | 200.7 ms | 318.5 ms | 1.587x |

The exact semantic gate passes. The `1.587x` ratio is diagnostic rather than
accepted benchmark evidence because of host load, and it is also above the
`1.10x` performance guard. The benchmark now requires an even number of paired
samples and writes an `invalid_environment` JSON result when normalized load
exceeds `0.30`. See the
[oracle example](examples/secworks-aes-regmodel.md) for provenance, authored
code, and workload details.

The profile counted 88,001 authored delay callbacks and zero clock callbacks
over 200 suites. Lazy, sticky clock-interest gating improved cpptb by about 8%
against the previous unconditional rising-edge callback. A same-binary
decomposition put generated register-model overhead at about 2.6% and the
per-access bus-master task layer at about 0.5%; almost all residual overhead is
the simulator/DPI/C++ scheduler transition at each timing boundary. A fused
timer-deadline ABI measured only a 0.6% paired gain and was removed. The full
methodology and experiment table live in the benchmark's `PROFILE.md`.
