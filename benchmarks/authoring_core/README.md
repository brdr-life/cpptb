# Authoring-core C++ DPI vs pure-SystemVerilog benchmark

This suite measures the authoring cost of the coroutine primitives in
`cpptb/coro_runtime.hpp` against direct SystemVerilog equivalents. Both sides
instantiate the same deterministic `authoring_core_dut`, issue the same request
stream, check the same responses, and fold them into the same 32-bit checksum.
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

For `N > 0`, every kernel has `transactions=N`, one response check per
transaction, and two final DUT-count checks. Feature kernels add one semantic
check per iteration except `clock_cycles`, whose effect is proved by its use
counter and paired cycle count.

| Kernel | Feature counts | Checks |
|---|---|---:|
| `control` | all zero | `N + 2` |
| `task_value` | `task_value=N` | `2N + 2` |
| `clock_cycles` | `clock_cycles=N` | `N + 2` |
| `timeout` | `timeouts=N`, `timeout_hits=floor(N/2)` | `2N + 2` |
| `task_timeout` | `task_timeouts=N`, `task_timeout_hits=floor(N/2)` | `2N + 2` |
| `wait_until` | `wait_until=N` | `2N + 2` |
| `event` | `event_set=N`, `event_wait=N` | `2N + 2` |
| `channel` | `channel_send=N`, `channel_receive=N` | `2N + 2` |
| `all` | all usage counts `N`, both timeout-hit counts `floor(N/2)` | `7N + 2` |

## Semantic mapping

| C++ coroutine construct | Pure-SystemVerilog equivalent |
|---|---|
| `Task<uint32_t>` with `co_return` and `co_await` | `task automatic` with an output argument and a blocking task call |
| `clock_cycles(clk, 1)` | `repeat (1) @(posedge clk)` |
| `with_timeout(RisingEdge{clk}, timeout)` returning `TimeoutOutcome` | named `fork...join_any`, `@(posedge clk)` versus `#timeout`, then `disable fork` |
| `with_timeout(Task<uint32_t>, timeout)` returning `TimeoutResult<uint32_t>` | named-block `fork...join_any`, a delayed value task versus `#timeout`, then `disable block` |
| `wait_until(req_ready, predicate, clk)` | `while (!req_ready) @(posedge clk)` |
| sticky `Event::clear/set/wait` | a sticky bit plus a SystemVerilog event; the waiter first tests the bit |
| unbounded `Channel<uint32_t>::put_nowait/get` | an unbounded SV queue push/pop plus its queue predicate |

The task-timeout kernel alternates deterministically: even iterations complete
the task after `500ps` against a `3ns` limit and consume `value()`; odd
iterations time out after `500ps` while the task would take `3ns`. Both sides
check the completion/timeout state, retain the same stimulus on timeout, and
therefore issue the identical DUT workload and checksum. It uses
`triggered()`, `timed_out()`, and `value()` from `TimeoutResult<T>`. Bounded
channels are deliberately not benchmarked. The channel kernel measures the
finalized unbounded FIFO semantics exactly; it does not model backpressure or
capacity.
The Event kernel exercises sticky set-before-wait behavior, and the Channel
kernel exercises queued-before-get behavior, so neither kernel includes an
unrequested process-spawn or producer-scheduling cost.

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
The runner build step compiles only the selected C++ DPI kernel plus the shared
pure-SystemVerilog binary.

The absolute median of paired `C++ DPI process wall / pure SV process wall` is
the hard guard. A median at or below `1.10` passes the hard limit. If that median
passes but its exact distribution-free one-sided 95% upper median bound exceeds
`1.10`, the runner collects exactly one additional 16-pair batch for the
selected example. A still-wide final bound is reported as
`passed_inconclusive` with a warning.

A paired median strictly above `1.10` is `failed` only when both order-stratified
paired medians are strictly above `1.05` and the ratio of independent process
time medians is within 5%, relative to the paired median. Otherwise the result
is `invalid_environment`. A valid initial hard failure is final and is never
diluted by an extra batch. At the run level, `failed` takes precedence over
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

Every sample records its binary path and SHA256, one-based slot within the pair,
zero-based global sequence index, pair order, and warmup/initial/extra batch.
Each completed sample, including warmups, is appended immediately to
`results/<example>/latest.jsonl`, then flushed and `fsync`ed. The JSON report also
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
partway through a pair.

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
