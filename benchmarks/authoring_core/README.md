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
| `wait_until` | `wait_until=N` | `2N + 2` |
| `event` | `event_set=N`, `event_wait=N` | `2N + 2` |
| `channel` | `channel_send=N`, `channel_receive=N` | `2N + 2` |
| `all` | all usage counts `N`, timeout hits `floor(N/2)` | `6N + 2` |

## Semantic mapping

| C++ coroutine construct | Pure-SystemVerilog equivalent |
|---|---|
| `Task<uint32_t>` with `co_return` and `co_await` | `task automatic` with an output argument and a blocking task call |
| `clock_cycles(clk, 1)` | `repeat (1) @(posedge clk)` |
| `with_timeout(RisingEdge{clk}, timeout)` returning `TimeoutOutcome` | named `fork...join_any`, `@(posedge clk)` versus `#timeout`, then `disable fork` |
| `wait_until(req_ready, predicate, clk)` | `while (!req_ready) @(posedge clk)` |
| sticky `Event::clear/set/wait` | a sticky bit plus a SystemVerilog event; the waiter first tests the bit |
| unbounded `Channel<uint32_t>::put_nowait/get` | an unbounded SV queue push/pop plus its queue predicate |

General task timeout and bounded channels are deliberately not benchmarked
because those APIs are deferred. The channel kernel measures the finalized
unbounded FIFO semantics exactly; it does not model backpressure or capacity.
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
make authoring-core-benchmark
```

The benchmark runner defaults to 100,000 iterations and 15 adjacent, warmed,
alternating DPI/SV pairs per kernel. Kernels are visited round-robin with a
rotating start point. Before feature kernels, it runs one unchanged peripheral
suite DPI/SV equivalence preflight by default; `--skip-preflight` is available
for focused development when those binaries are intentionally unavailable.

The absolute median of paired `C++ DPI process wall / pure SV process wall` is
the hard guard. Any value strictly greater than `1.10` aborts immediately once
that kernel's initial batch is complete. If the median passes but its exact
distribution-free one-sided 95% upper median bound exceeds `1.10`, the runner
collects exactly one additional 15-pair batch. A still-wide bound is reported
as a warning. Normalization versus `control` is diagnostic only.

Raw samples, global execution order, pair order, exact confidence bounds,
environment/tool metadata, preflight data, and per-kernel summaries are written
to `results/latest.json`; the compact report is `results/latest.md`.

## Direct tests

```sh
python3 -m unittest discover -s benchmarks/authoring_core/tests -v
```

The tests cover strict result parsing, workload formulas at boundary iteration
values, cross-mode mismatches, invalid and nonfinite timing samples, adjacent
alternating pair order, exact confidence bounds, the extra-batch policy, and
the strict `> 1.10` hard-failure boundary.
