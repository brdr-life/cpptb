# Testbench authoring

cpptb testbenches are C++20 coroutines connected to a generated, typed DUT.
The public runtime is under `include/cpptb/`; generated SystemVerilog uses DPI
to exchange signal batches and simulator events with that runtime.

Start with the small counter, then move to the FIFO scoreboard or APB example:

```sh
make cpp-dpi-counter-run
make cpp-dpi-fifo-scoreboard-run
make cpp-dpi-apb-regfile-run
```

The APB example keeps protocol timing in a reusable helper, leaving the test
sequence register-oriented:

```cpp
Task<void> register_sequence(ApbRegfileTb tb) {
    co_await reset_dut(tb);
    const ApbMaster apb{tb};

    co_await apb.write(0x04, 0x1234'5678);
    co_await apb.read_expect("register readback", 0x04, 0x1234'5678);
}
```

The complete implementation, including `Task<uint32_t> read(...)`, is in
`examples/apb_regfile/testbench.cpp`.

The user-facing primitives are deliberately small:

- `tb.spawn(task)` to attach concurrent reset, driver, sequence, and monitor
  coroutines and return a process handle.
- `co_await RisingEdge{dut.HCLK}` and `co_await FallingEdge{dut.HCLK}` for
  simulator triggers.
- `dut.signal.set(value)` and `dut.signal.get()` for explicit signal access.
- `co_await helper_task(...)` for reusable bus operations such as APB writes
  and read/check transactions.

Every coroutine that does not return a value is declared `Task<void>`.
`Task<T>` carries a typed result from `co_return` to `co_await`, including
move-only values. Tasks themselves are move-only and can only be awaited as
rvalues; a completed result is moved to the awaiting coroutine. Scheduler
roots passed to `spawn()` or `spawn_detached()` remain `Task<void>`, and every
child passed to `Join` must also be a `Task<void>`.

The coroutine scheduler also supports:

- `co_await Edge{signal}` for either transition.
- `co_await Delay{1_ps}` for a physical simulation-time delay. Integer
  `fs`, `ps`, `ns`, `us`, and `ms` literals are supported and must be
  positive and representable at the wrapper's configured `timeprecision`.
- `co_await First{RisingEdge{signal}, Delay{100_ns}}`, which returns the index
  of the trigger that fired first. `First` accepts two or more triggers.
- `co_await Join{producer(tb), consumer(tb), scoreboard(tb)}` for two or more
  concurrent `Task<void>` children.
- `co_await clock_cycles(clock, count)` to wait for `count` rising edges.
  A count of zero completes immediately without suspending.
- `co_await with_timeout(RisingEdge{signal}, 100_ns)` to race a rising,
  falling, or either-edge trigger against a `SimTime` delay. It returns
  `TimeoutOutcome::Triggered` or `TimeoutOutcome::TimedOut`.
- `co_await with_timeout(operation(), 100_ns)` to race any `Task<T>` against
  a deadline. It returns `TimeoutResult<T>`; `has_value()`, `triggered()`, and
  boolean conversion report completion, `timed_out()` reports timeout, and
  `value()`, `operator*`, and `operator->` access a completed value. The
  `Task<void>` specialization provides the same state queries and a checked
  no-op `value()` for completed operations.
- `co_await wait_until(signal, predicate, clock)` to evaluate the predicate
  immediately and, while it is false, poll it after each rising edge of
  `clock`. The predicate receives the signal's `uint32_t` value.

`Event` is a sticky, manually reset notification. `set()` leaves it set and
wakes all current waiters in FIFO registration order; later waits also finish
immediately until `clear()` is called. Both `co_await event.wait()` and
`co_await event` are supported, and `is_set()` reports the current state.

`Channel<T>` is an unbounded FIFO for move-constructible values.
`put_nowait(value)` enqueues immediately, `co_await put(value)` is the
coroutine form, and `co_await get()` waits for and moves out the oldest value.
Move-only payloads are supported. A capacity limit and producer backpressure
are not implemented.

An `Event` or `Channel` must outlive its active waiters; destroying one with
live waiters aborts with a diagnostic. Current waiters must belong to one
scheduler, although the object may be reused with another scheduler after its
wait queue is empty. Scheduler destruction and process cancellation invalidate
their registrations, which are removed during cleanup.
Unused `Event` and `Channel` objects do not register scheduler waits. This is a
lifetime property of the API, not a benchmark claim about hot-path cost.

`TimeoutResult<T>` stores its result in an `optional`, so `T` need only be
move-constructible and does not need a default constructor. Calling `value()`
after a timeout aborts with a diagnostic. A timed-out task is recursively
cancelled, including nested tasks and waits on `Process`, `Event`, or
`Channel`; its frames are destroyed at the existing scheduler cleanup
boundary. If task completion and the deadline share a timestamp, completion
wins when the task's result is present as the parent resumes. Zero and
sub-precision timeout durations are rejected under the same rules as
`Delay`.

These pieces compose into ordinary driver, monitor, and scoreboard code. The
following is the core of the runnable `fifo_scoreboard` example:

```cpp
Task<void> scoreboard(FifoScoreboardTb tb,
                      Channel<uint32_t>& expected_words,
                      Channel<uint32_t>& observed_words) {
    for (uint32_t index = 0; index < tb.iterations(); ++index) {
        const uint32_t expected = co_await expected_words.get();
        const uint32_t actual = co_await observed_words.get();
        tb.expect_eq("FIFO payload", actual, expected);
    }
}

Task<void> fifo_test(FifoScoreboardTb tb) {
    Event reset_done;
    Channel<uint32_t> expected_words;
    Channel<uint32_t> observed_words;

    co_await Join{reset_dut(tb, reset_done),
                  input_driver(tb, reset_done, expected_words),
                  output_ready_driver(tb, reset_done),
                  output_monitor(tb, reset_done, observed_words),
                  scoreboard(tb, expected_words, observed_words)};
}
```

A spawned process can be awaited or cancelled explicitly:

```cpp
auto monitor = tb.spawn(monitor_bus(tb));
auto driver = tb.spawn(drive_packets(tb));

co_await driver;
monitor.cancel();
```

`Process` is a copyable handle to scheduler-owned process state. Copying a
handle does not copy the coroutine or transfer ownership; all copies observe
the same completion state. The scheduler owns the root coroutine and destroys
its frame when it completes or is cancelled. A `Process` does not extend the
life of its `Testbench`. After the testbench is destroyed, outstanding handles
are invalid: status queries are safe and return false, `cancel()` is a no-op,
and attempting to `co_await` the invalid handle aborts with a diagnostic.

`done()` reports either normal completion or completed cancellation.
`cancelled()` is true only after cancellation has taken effect, so
`cancelled()` implies `done()`. If normal completion wins a race with a pending
cancellation request, the process is done but not cancelled. Cancellation
recursively stops nested tasks and wakes process waiters. A running process may
cancel itself; that request is cooperative and is applied after the coroutine
returns control to the scheduler. Code still executing before that scheduler
boundary observes the process as neither done nor cancelled.

Use `spawn()` when callers need a handle for status, awaiting, or cancellation.
Use `spawn_detached()` for fire-and-forget roots. Both forms reclaim completed
root coroutine frames; detached roots omit the process-control metadata used
for handles, status, awaiting, and cancellation.

`examples/watchdog_timeout/testbench.cpp` shows both forms of `with_timeout()`
and a complete `spawn()`/`cancel()`/await lifecycle. Its deadlines are kept
strictly separate from response times so the expected result is independent of
same-timestamp ordering.

## Cocotb concept map

cpptb follows familiar cocotb testbench structure while keeping signal access
and timing explicit:

| Verification concept | cocotb | cpptb | Pure-SV twin |
|---|---|---|---|
| Timed wait | `await Timer(1, unit="ns")` | `co_await Delay{1_ns}` | `#1ns` |
| Signal edge | `await RisingEdge(dut.clk)` | `co_await RisingEdge{dut.clk}` | `@(posedge clk)` |
| Concurrent work | `start_soon()` / task groups | `spawn()` or `Join{...}` | `fork ... join` |
| FIFO communication | `Queue` | `Channel<T>` | `mailbox` |
| Notification | `Event` | `Event` | `event` |
| Deadline | `with_timeout()` | `with_timeout()` | explicit event/deadline race |

The mapping is informed by cocotb's official documentation for
[testbench structure](https://docs.cocotb.org/en/stable/writing_testbenches.html),
[coroutines and concurrency](https://docs.cocotb.org/en/stable/coroutines.html),
and the [timing model](https://docs.cocotb.org/en/stable/timing_model.html).
In particular, an edge wake does not by itself promise that downstream
sequential or combinational logic has settled. The examples therefore show
the sampling point explicitly with `Delay{1_ps}`, mirrored by `#1ps` in SV.

The complete learning path is indexed in `examples/README.md`. Run individual
pairs or all examples through the standard target:

```sh
make feature-test FEATURE=dpi_fifo_scoreboard
make feature-test FEATURE=dpi_apb_regfile
make feature-test FEATURE=dpi_watchdog_timeout
make examples-test
```

## Scheduler ordering and cleanup

Waiters registered on the same edge resume in registration order. Timers with
the same deadline also resume FIFO by registration order. Registrations made
stale by `First`, cancellation, or completion are removed without resuming the
coroutine; this cleanup includes edge queues and falling-edge interest counts,
not only the timer heap.

Generated clocks are configured as static edge sources before the first wait is
registered. Their edges are delivered unconditionally, so waits on those IDs do
not contribute dynamic edge-interest masks or publications; waiter lifecycle,
cancellation, `First`, and falling-edge summaries are otherwise unchanged.

Generated DPI wrappers use one persistent, clock-agnostic timer owner. The
module-level `timer_deadline` is the source of truth, `timer_owner_target`
describes only the owner's current positive sleep, and `timer_kick` wakes an
idle owner. A generation-checked one-shot process is retained only when a
non-owner callback inserts a deadline strictly earlier than the owner's stale
sleep target. Clock drivers and observers remain independent, so
`clock_cycles()` is still an edge primitive while `Delay` works with no clocks.

The generated timer contract is:

- **I1:** after `STEP_TIMER_CHANGED`, `timer_deadline` equals the scheduler's
  earliest live deadline or `NO_TIMER`;
- **I2:** the persistent owner and strict-earlier fallback deliver each live
  deadline exactly once; generation checks prevent stale fallback delivery;
- **I3:** timer dispatch uses no zero delay, delayed nonblocking assignment, or
  `disable fork`;
- **I4:** every live deadline has an owner or fallback wake no later than that
  deadline;
- **I5:** the fallback is unreachable in a clockless wrapper because no
  non-owner step can insert an earlier deadline while the owner sleeps;
- **I6:** there remains one next-deadline DPI query per timer-change request,
  and steady-state timer arms allocate no SystemVerilog process. Only the
  exceptional strict-earlier fallback allocates a one-shot process.

Zero-duration delays and delays that cannot be represented at the configured
simulation precision abort with a diagnostic. Awaiting a default-constructed,
expired, or otherwise invalid `Process` also aborts instead of silently
continuing.

## Scheduler performance

A macOS sampling profile first identified full coroutine-state scans and
hash-table lookups as scheduler hot spots. The scheduler now uses reusable
numeric state slots, direct signal-indexed wait queues, targeted child cleanup,
conditional drains, and an active-coroutine counter.

A second profile of the exact dual-clock C++ DPI/pure-SV comparison showed
that the remaining cost was primarily in generated SystemVerilog timing
processes rather than C++ queue management. Generated periodic clocks use one
absolute-deadline process, falling-edge DPI calls are skipped unless the
scheduler has a matching waiter, and physical delays are scheduled only when
a coroutine awaits `Delay`.

The performance guard uses an initial batch of 16 warmed, adjacent
C++ DPI/pure-SV pairs and alternates execution order. It compares the median of
the paired process-time ratios and hard-fails above `1.10x`. When the median
passes but its one-sided 95% upper confidence bound is inconclusive, the guard
collects one additional 16-pair batch and evaluates the combined samples; it
does not rerun the complete benchmark. A still-inconclusive passing median is
reported with a warning. Result artifacts include raw pairs and the
environment/build metadata needed to interpret them. Close ratios are treated
as noisy measurements, not evidence that either implementation is
directionally faster.

## Current scope

The end-to-end test suite currently targets Verilator. Scalar signal values use
`uint32_t`; packed values use `uint64_t` through 64 bits and `Bits<W>` above 64
bits. Generated DPI bindings support fixed multidimensional unpacked arrays,
packed enum and struct views, wide values, fixed-point helpers, and generated
hierarchical probes with read, deposit, force, and release operations.
Four-state X/Z propagation and bounded channels remain deferred. The generated
transport uses standard SystemVerilog DPI, but additional simulator backends
have not yet passed the conformance suite.

The Authoring Core sources currently present under
`benchmarks/authoring_core/` exercise typed tasks, cycle waits, edge timeouts,
predicate waits, events, channels, wide packed signals, fixed-point arithmetic,
fixed unpacked arrays, and a synchronous memory front door. Its C++ DPI testbench is
`benchmarks/authoring_core/testbenches/cpp_dpi/testbench.cpp`, the corresponding pure-SV
source is `benchmarks/authoring_core/testbenches/systemverilog/authoring_core_sv_tb.sv`, and the
shared workload contract is `benchmarks/authoring_core/workload.py`. Runtime
API tests are in `tests/unit/coro_runtime_test.cpp`.

## Reusable DPI runtime

`include/cpptb/dpi_runtime.hpp` owns the design-independent DPI host behavior:

- compact directional input/output transport and driven-signal tracking;
- typed signal `get()`/`set()` callbacks and dirty-output detection;
- generated internal-probe `get()`/`deposit()` access for packed variables and
  fixed memories;
- scheduler construction, edge dispatch, and delay deadlines;
- falling-edge interest and precision-aware time transport;
- timeout invocation, elapsed wall time, completion, and result reporting;
- the standard init, step, output-pull, deadline, and edge-interest C exports
  expected by the generated wrapper.

`include/cpptb/test_result.hpp` keeps the standard check/failure result contract
independent of DPI, so user-facing fixtures do not include simulator transport
headers.

A design supplies a small `DpiAdapter` containing its DUT and result types,
generated signal metadata, binding call, testbench registration call, result
name, and timeout policy. `CPPTB_DEFINE_DPI_RUNTIME(Adapter)` provides the C
entry points. No design transport needs to copy open arrays, decode events, or
format a result line.

The hot scheduler step receives only the compact observed-word array. Driven
words are fetched through a separate idempotent output-pull export on
initialization or after `STEP_OUTPUTS_CHANGED`, so unchanged steps do not carry
an output argument through the simulator ABI.

`deposit()` performs the underlying SystemVerilog blocking assignment
immediately. It does not insert a scheduler delay or observation phase;
testbench code uses an explicit `co_await Delay{...}` when downstream RTL must
evaluate before observation.

## Benchmark

The cocotb comparison benchmark is in `experiments/cocotb_cpp_comparison/`:

```sh
python3 experiments/cocotb_cpp_comparison/run_benchmark.py --iters 1000 --runs 3
```

It runs the same APB event-unit traffic in cocotb and in the C++ coroutine
model, then writes results to `experiments/cocotb_cpp_comparison/results/`.
