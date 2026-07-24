# Authoring-core C++ DPI vs pure-SystemVerilog benchmark

This suite measures the authoring cost of the coroutine primitives in
`include/cpptb/coro_runtime.hpp` against direct SystemVerilog equivalents. Both
sides instantiate the same deterministic `authoring_core_dut`, issue the same
request stream, check the same responses, and fold them into the same 32-bit
checksum.
The generated DPI wrapper comes from the Slang manifest and public DPI runtime;
the benchmark does not use a Verilator-private API.

## Workload contract

Iteration `i` sends:

```text
stimulus(i) = (((i + 1) * 0x1f123bb5) mod 2^32) XOR 0xc001d00d
response(i) = ((stimulus(i) XOR 0xa5a55a5a) + i) mod 2^32
checksum(0) = 0x811c9dc5
checksum(i + 1) = ((checksum(i) XOR response(i)) * 0x01000193) mod 2^32
```

The DUT accepts one request at a time, emits its response after a fixed
pipeline delay, and exposes accepted-request and accepted-response counters.
Every paired result must match on kernel, iterations, transactions, checks,
simulation cycles, checksum, failures, and every feature-use counter. The
runner also validates counts and checksum against `workload.py`, independently
of the other implementation.

For `N > 0`, every protocol kernel has `transactions=N`, one response check
per transaction, and two final DUT-count checks. Most feature kernels add one
semantic check per iteration. `array_index` adds eight, `array_wide` adds four,
and `force_release` adds two; `clock_cycles` adds none because its effect is
proved by its use counter and paired cycle count.

`force_direct` is a deliberately separate zero-time microbenchmark. It has no
protocol transaction or clock activity and performs one force, immediate
readback of the forced net, and release per iteration. The runner always
reports its raw `1.10x` guard result. The top-level feature registry applies a
narrow `1.20x` waiver ceiling to this one transport-bound case; it does not
waive semantic parity, malformed results, invalid environments, or any other
feature.

| Kernel | Feature counts | Checks |
|---|---|---:|
| `control` | all zero | `N + 2` |
| `task_value` | `task_value=N` | `2N + 2` |
| `clock_cycles` | `clock_cycles=N` | `N + 2` |
| `timeout` | `timeouts=N`, `timeout_hits=floor(N/2)` | `2N + 2` |
| `task_timeout` | `task_timeouts=N`, `task_timeout_hits=floor(N/2)` | `2N + 2` |
| `wait_until` | `wait_until=N` | `2N + 2` |
| `event` | `event_set=N`, `event_wait=N` | `2N + 2` |
| `queue` | `queue_send=N`, `queue_receive=N` | `2N + 2` |
| `queue_sync` | `queue_put=N`, `queue_get=N`, `lock_acquire=N`, `semaphore_acquire=N` | `2N + 2` |
| `wide64` | `wide64=N` | `2N + 2` |
| `wide_echo_137` | `wide_echo_137=N` | `2N + 2` |
| `wide_slice` | `wide_slice=N` | `2N + 2` |
| `fixed_mac` | `fixed_mac=N` | `2N + 2` |
| `array_index` | `array_index=N` | `9N + 2` |
| `array_wide` | `array_wide=N` | `5N + 2` |
| `mem_rw` | `mem_rw=N` | `2N + 2` |
| `hier_probe` | `hier_probe_reads=2N`, `hier_probe_deposits=N` | `3N + 2` |
| `mem_backdoor` | `mem_backdoor_reads=N`, `mem_backdoor_deposits=N` | `3N + 2` |
| `mem_probe_read` | `probe_diag_reads=N` | `3N + 2` |
| `mem_probe_deposit` | `probe_diag_deposits=N` | `2N + 2` |
| `mem_probe_read_deposit` | both probe diagnostic counts `N` | `3N + 2` |
| `signal_edge` | `signal_edges=N` | `N + 2` |
| `array_multidim` | `array_multidim=N` | `7N + 2` |
| `force_release` | `force_release=N` | `3N + 2` |
| `packed_view` | `packed_view=N` | `5N + 2` |
| `force_direct` | `force_release=N`, zero transactions and cycles | `N` |
| `hier_data` | `hier_data_reads=2N`, `hier_data_deposits=2N` | `3N + 2` |
| `timing_phases` | `timing_phases=N`, zero transactions | `2N` |
| `test_lifecycle` | `test_lifecycle=N`, `spawned_processes=1`, zero transactions and cycles | `3N` |
| `dynamic_task` | `dynamic_spawn=N`, `spawned_processes=0`, zero transactions and cycles | `N` |
| `dynamic_spawn_scheduler` | `dynamic_spawn=N`, `spawned_processes=N`, zero transactions and cycles | `N` |
| `dynamic_spawn` | `dynamic_spawn=N`, `spawned_processes=N`, zero transactions and cycles | `N` |
| `dynamic_spawn_suspending` | `dynamic_spawn=N`, `spawned_processes=2N`, zero transactions and cycles | `N` |
| `dynamic_monitor` | `queue_put=N`, `queue_get=N`, `spawned_processes=2`, `transactions=N` | `N + 3` |
| `process_pipeline` | `queue_put=2N`, `queue_get=2N`, `spawned_processes=3`, `transactions=N` | `N + 4` |
| `analysis_fanout` | `analysis_write=2N`, `analysis_delivery=3N`, `spawned_processes=1`, `transactions=N` | `N + 6` |
| `apb_component` | `apb_component=2N`, `transactions=2N` | `5N + 4` |
| `transaction_recording` | `analysis_write=4N`, `analysis_delivery=6N`, `transactions=2N`, retained record metadata and JSON payloads | `5N + 9` |
| `memory_model` | `memory_model=2N`, `transactions=2N` | `5N + 4` |
| `memory_model_direct` | `memory_model_direct=2N`, `transactions=2N` | `3N + 2` |
| `register_prediction_validity` | `register_prediction_validity=N`, zero DUT transactions and cycles | `5N + 4` |
| `register_backdoor` | `register_backdoor=N`, zero bus transactions, one deposit and one read per iteration | `N + 2` |
| `register_hierarchy` | `register_hierarchy=N`, four typed array elements and deterministic traversal, zero DUT transactions | `5N + 2` |
| `register_split` | `register_split=N`, two 16-bit writes and two 16-bit reads per 32-bit logical register operation | `2N + 2` |
| `register_wide` | `register_wide=N`, exact 128-bit register/memory frontdoors, raw backdoors, and passive prediction over 32-bit transfers | `6N + 2` |
| `register_enum` | `register_enum=N`, one typed enum field write and read per operation | `2N + 2` |
| `register_memory` | four-entry semantic backdoor write/read spans, zero bus transactions | `6N + 2` |
| `register_sequences` | reset/access/bit-bash sequence policy, 21 frontdoor transactions per iteration | `46N + 2` |
| `register_coverage` | `coverage_sampling=N`, ten observed register/memory transactions per iteration | `12` |
| `register_maps` | primary, alias, and custom-frontdoor register/memory views, ten transactions per iteration | `8N + 2` |
| `register_user_effects` | two policy-controlled transfers and narrow/wide prediction checks per iteration | `4N + 2` |
| `all` | all aggregate usage counts enabled, both timeout-hit counts `floor(N/2)` | `28N + 2` |

`memory_model` is the clocked APB integration workload.
`memory_model_direct` performs the same sparse-memory write/read semantics
without DUT traffic or simulation-time advancement, isolating model overhead.
`register_prediction_validity` executes matched reset-mask, field-update,
direct/read/write prediction, side-effect-validity bookkeeping, and passive
`AnalysisPort` predictor dispatch without DUT traffic so the RAL cost is
measured directly.
`register_backdoor` compares a generated-style typed RAL adapter against the
same direct hierarchical deposit/read/check sequence in pure SystemVerilog.
Neither side inserts a delay.
`register_hierarchy` compares compile-time indexed register-array views and
ordered traversal against the same four-element operations and traversal in
pure SystemVerilog.
`register_split` compares a 32-bit RAL frontdoor over a 16-bit transport with
the same two-write/two-read sequence authored directly in SystemVerilog.
`register_wide` compares 128-bit `Bits` RAL register and memory frontdoors,
raw backdoors, and passive prediction over a 32-bit transport
with the same four-write/four-read sequence in SystemVerilog.
`register_enum` compares generated-style typed field access, including the same
enum casts in its pure-SystemVerilog peer.
`register_memory` compares the generated-memory `AccessPath::Backdoor` span API
against the same four HDL memory deposits, four reads, checks, and checksum
updates in pure SystemVerilog. The generated standard-DPI adapter carries up to
four adjacent entries in one packed export, while the public register-memory
API and semantic operation count remain unchanged. Neither side advances
simulation time.
`register_sequences` compares reusable reset-check, mixed access-check, and
policy-aware bit-bash sequences with the same literal procedures in pure SV.
`register_coverage` compares passive address, byte-enable, field-policy, and
memory-index classification with an exact SV coverage-counter twin.
`register_maps` compares primary and aliased address views plus exceptional
register and memory frontdoors while preserving one logical mirror.
`register_user_effects` compares an authored user-effect policy with the same
XOR-on-write and invert-on-read equations in pure SV. These RAL kernels do not
insert simulation delays; every transaction and check count is validated by
the independent workload contract. Because this kernel compares a generated
runtime abstraction with directly inlined SV equations, it uses a 10,000,000
iteration diagnostic policy. The timed secworks AES integration is the
release-facing register-model performance guard.

## Semantic mapping

| C++ coroutine construct | Pure-SystemVerilog equivalent |
|---|---|
| `Task<uint32_t>` with `co_return` and `co_await` | `task automatic` with an output argument and a blocking task call |
| `clock_cycles(clk, 1)` | `repeat (1) @(posedge clk)` |
| `with_timeout(RisingEdge{clk}, timeout)` returning `TimeoutOutcome` | named `fork...join_any`, `@(posedge clk)` versus `#timeout`, then `disable fork` |
| `with_timeout(Task<uint32_t>, timeout)` returning `TimeoutResult<uint32_t>` | named-block `fork...join_any`, a delayed value task versus `#timeout`, then `disable block` |
| `wait_until(req_ready, predicate, clk)` | `while (!req_ready) @(posedge clk)` |
| sticky `Event::clear/set/wait` | a sticky bit plus a SystemVerilog event; the waiter first tests the bit |
| unbounded `Queue<uint32_t>::put_nowait/get` | an unbounded SV queue push/pop plus its queue predicate |
| bounded `Queue`, `Semaphore`, and `Lock` contention | bounded mailbox, semaphore credits, and semaphore lock |
| `Bits<W>` packed access and slices | packed vectors and indexed part-selects |
| `Fixed` multiply and quantize | signed packed arithmetic with matching nearest-even saturation |
| `array.at(index).get()/set()` | fixed unpacked-array element access with the same SV index |
| scalar memory front-door ports | synchronous address/data/write-enable sequence against internal SV memory |
| internal `get()` | direct hierarchical read in the same scheduler callback |
| internal `deposit(value)` followed by explicit `Delay{1_ps}` | blocking hierarchical assignment followed by user-authored `#1ps` before downstream observation |
| `RisingEdge{rsp_valid}` on a DUT output observer | `@(posedge rsp_valid)` |
| rank-2 `.at(row).at(column).set()/get()` with 65-bit elements | nested unpacked-array indexed writes/reads |
| internal `.force(value)`, explicit `Delay{1_ps}`, `.release()`, explicit `Delay{1_ps}` | `force`, `#1ps`, `release`, `#1ps` on the same internal net |
| internal `.force(value)`, immediate `.get()`, `.release()` | zero-time `force`, hierarchical readback, and `release` in a standalone SV top |
| direct `co_await Task<void>` | blocking call to an automatic SV task |
| low-level scheduler process | one-child `fork ... join` |
| lifecycle-tracked `TestContext::spawn()` | the same one-child `fork ... join` plus framework ownership on the C++ side |
| two event-suspending processes | two forked SV tasks with the same event handshake |
| two long-lived response observers, bounded `Queue`, and cancellation | named `fork...join_none`, bounded mailbox handoff, and `disable` after the same DUT traffic |
| finite driver, response worker, and scoreboard processes | three automatic tasks in `fork...join` connected by two bounded mailboxes |
| synchronous `AnalysisPort` fan-out to a scoreboard and bounded audit buffer | direct expected/actual queues plus a bounded mailbox audit subscriber |
| `cpptb_vc` APB master, passive monitor, checker, and scoreboard | matching APB tasks, passive forked processes, and expected/actual transaction queues |

`dynamic_spawn_scheduler` and `dynamic_spawn` intentionally share the same
one-child `fork ... join` SystemVerilog baseline. Their C++ difference is the
thing under measurement: direct scheduler ownership versus `TestContext`
lifecycle attribution. Separate SV bodies would add unrelated work and make
that comparison less exact.

The `signal_edge`, `array_multidim`, `force_release`, `packed_view`, and
`force_direct` kernels are intentionally isolated and are not included in
`all`. `force_direct` uses a standalone pure-SV binary so its zero-time loop
does not distort or inherit the larger timed testbench's generated code.

The task-timeout kernel alternates deterministically: even iterations complete
the task after `500ps` against a `3ns` limit and consume `value()`; odd
iterations time out after `500ps` while the task would take `3ns`. Both sides
check the completion/timeout state, retain the same stimulus on timeout, and
therefore issue the identical DUT workload and checksum. It uses
`triggered()`, `timed_out()`, and `value()` from `TimeoutResult<T>`. The
`queue` kernel measures unbounded FIFO semantics; `queue_sync` adds bounded
producer backpressure, semaphore credits, and lock contention.
The Event kernel exercises sticky set-before-wait behavior, and the Queue
kernel exercises queued-before-get behavior, so neither kernel includes an
unrequested process-spawn or producer-scheduling cost.

Four dynamic-process kernels deliberately decompose direct task creation,
core scheduler process creation, lifecycle-tracked `spawn()`, and processes
that genuinely suspend. They default to 5,000,000 iterations so startup noise
cannot hide a per-process regression. Each has an exact pure-SV twin and the
runner rejects mismatched checks, feature counts, or checksums before applying
the performance guard. `dynamic_task` and `dynamic_spawn` are diagnostic
controls for compiler inlining and minimum lifecycle cost; they are not
framework performance gates. `dynamic_monitor` is a hard-gated persistent
companion workload: two observers exchange every DUT response through a bounded
queue and are cancelled after 100,000 foreground transactions.
`process_pipeline` is the hard-gated finite companion: a driver, response
worker, and scoreboard repeatedly suspend, communicate through two bounded
queues, complete naturally, and are joined.

`analysis_fanout` publishes one expected and one observed transaction per DUT
response. The observed stream reaches both an in-order scoreboard and a
bounded audit buffer synchronously; one persistent monitor process drains the
DUT response. The pure-SV twin performs the same three deliveries, buffer
operation, checks, transaction count, and checksum.

`spawned_processes` counts explicit independently scheduled process operations
authored by these lifecycle and dynamic-process kernels. It does not count the
root benchmark task or implementation processes internal to `Join`, timeout,
and queue primitives.

The C++ kernel is selected at compile time with `AUTHORING_CORE_KERNEL`, so
feature dispatch is absent from its hot loop. The pure-SV testbench selects one
dedicated task in an initial `case` before entering that task's loop.

## Build and run

```sh
make authoring-core-dpi-codegen
make authoring-core-dpi-codegen-check
make authoring-core-dpi-build
make authoring-core-sv-build
make authoring-core-build

AUTHORING_CORE_ITERS=10000 make authoring-core-dpi-run AUTHORING_CORE_KERNEL=event
AUTHORING_CORE_ITERS=10000 make authoring-core-sv-run AUTHORING_CORE_KERNEL=event
python3 benchmarks/authoring_core/run_benchmark.py --example event
python3 benchmarks/authoring_core/run_benchmark.py --example event --with-preflight
```

The benchmark runner defaults to 100,000 iterations and 16 adjacent, warmed,
alternating DPI/SV pairs for exactly one registry-selected authoring example.
`--example` is required exactly once; missing, repeated, unknown, and integration
example names are rejected before any result file is opened. `--pairs` accepts
any even value of at least 16 and rejects odd counts. The peripheral-suite
DPI/SV equivalence preflight is disabled by default and can be requested with
`--with-preflight`; its report field remains present as `skipped` when omitted.
The runner build step compiles only the selected C++ DPI kernel plus its
pure-SystemVerilog twin. Most kernels share one SV binary; `force_direct` builds
its isolated zero-time SV binary. Both binaries use the same
`AUTHORING_CORE_OPT_FAST=-O3` fast-code setting by default; callers may
override that Make variable for controlled compiler experiments.

The absolute median of paired `C++ DPI process wall / pure SV process wall` is
the hard guard. A median at or below `1.10` passes the hard limit. If that median
passes but its exact distribution-free one-sided 95% upper median bound exceeds
`1.10`, the runner collects exactly one additional 16-pair batch for the
selected example. A still-wide final bound is reported as
`passed_inconclusive` with a warning.

A paired median strictly above `1.10` is provisionally confirmed only when
both order-stratified paired medians are strictly above `1.05` and the ratio
of independent process-time medians is within 5%, relative to the paired
median. That provisional crossing collects exactly one additional 16-pair
confirmation batch. Only the combined result may become `failed`; a crossing
without confirming diagnostics is `invalid_environment` and does not collect
more samples. At the run level, `failed` takes precedence over
`invalid_environment`; either classification returns nonzero. Timings and
paired ratios are always the raw observations and are never normalized for
host load, power, temperature, or the control kernel.

Before opening or truncating any result file, the runner captures the starting
git commit, complete porcelain status, host load averages, tool versions, and
configuration. Every child execution records process wall time plus child user
CPU, system CPU, total CPU, CPU utilization, and maximum-RSS resource data.
The CPU values are deltas around that child, not cumulative runner usage.

Immediately before and after every measured pair, the runner records a UTC and
monotonic timestamp, 1/5/15-minute host load averages normalized by logical CPU
count, and platform power/thermal probes. Unsupported probes remain explicit
in the JSON rather than being guessed. Excess normalized load, a power-source
transition, or a reported CPU speed/scheduler limit below 100% marks the
environment invalid. That evidence vetoes an otherwise confirmed threshold
crossing as `invalid_environment` and qualifies an apparent pass as
`passed_inconclusive`. It never changes samples or ratios, never turns a
passing measurement into a failure, and never changes the one fixed
conditional 16-pair extra-batch policy.

Every isolated kernel uses the same current DUT port surface. The DPI wrapper
packs all observed ports at each scheduler step, even when a particular kernel
does not read them, while the pure-SV side has no equivalent transport copy.
This keeps comparisons within one revision apples-to-apples, but a control
ratio from a larger DUT revision is not directly comparable to historical
control results from a smaller port surface.

Every sample records its binary path and SHA256, one-based slot within the pair,
zero-based global sequence index, pair order, and warmup/initial/extra batch.
Each completed performance sample, including warmups, is appended immediately
to `results/<example>/latest.jsonl`, then flushed and `fsync`ed. The JSON report also
stores those raw samples, child CPU metrics, binary metadata, order-stratified
and independent-median diagnostics, exact confidence bounds, pair-boundary
load/power/thermal evidence, environment/tool metadata, preflight data, and
per-kernel summaries.

Success, hard failure, invalid-environment, and operational-error runs all write
`results/<example>/latest.json` and `results/<example>/latest.md` before
returning. Each file is flushed and `fsync`ed through a temporary sibling, then
installed atomically with `os.replace`. The selected example's result directory
is resolved before truncation or error handling, and one run never reads,
truncates, or replaces another example's artifacts. Completed samples and
metadata therefore remain available after a nonzero exit, including failures
partway through a pair. `--semantic-only` writes `semantic.json`,
`semantic.md`, and `semantic.jsonl` in the same directory, leaving the latest
performance measurement byte-for-byte unchanged.

## Direct tests

```sh
python3 -m unittest discover -s benchmarks/authoring_core/tests -v
```

The tests cover strict result parsing, workload formulas at boundary iteration
values, cross-mode mismatches, invalid and nonfinite timing samples, balanced
interleaving, slots and sequence indices, exact confidence bounds, every guard
classification and boundary, one-example selection, odd-pair rejection, the fixed extra-batch policy,
binary hashes, child resource metrics, pair-boundary probes, environment-veto
semantics, starting-state ordering, per-sample journaling, atomic writes,
cross-example isolation, and evidence preservation after an injected mid-pair
error.
