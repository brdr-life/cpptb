# C++ VPI Testbench Prototype

This is the C++ counterpart to the MojoTB prototype in `mojotb/`.

The point is to keep the same cocotb-like shape while using direct C++ and
standard VPI hooks:

- `vpi_handle_by_name` for hierarchical signal lookup.
- `vpi_get_value` for signal reads.
- `vpi_put_value` for signal writes.
- `vpi_register_cb` with `cbValueChange` for trigger callbacks.

Run from the repo root:

```sh
make cpp-vpi-run
```

The testbench is split into concurrent C++ processes:

- `ResetDriver`
- `EnableDriver`
- `CountMonitor`

Each process returns a `WaitRequest`. The host stores one wait request per
process and resumes every process whose trigger fires.

## Coroutine Authoring Core

The coroutine API is implemented in `cpptb/coro_runtime.hpp`. The example under
`cpptb/rggen_apb_event/` uses it with a no-port simulation wrapper around a
copied RgGen/PeakRDL APB event unit:

```sh
make cpp-apb-event-run
```

The testbench shape is closer to cocotb:

```cpp
void register_tests(coro::Testbench& tb, ApbEventDut dut) {
    tb.spawn(reset_driver(tb, dut));
    tb.spawn(apb_register_sequence(tb, dut));
    tb.spawn(irq_monitor(tb, dut));
    tb.spawn(sleep_monitor(tb, dut));
}

Task<void> apb_register_sequence(coro::Testbench& tb, ApbEventDut dut) {
    const ApbMaster apb{tb, dut};

    co_await apb.write("write irq enable", kIrqEnable, 0x0000'002a);
    dut.irq_i.set(0x0000'0008);
    co_await clock_cycles(dut.HCLK, 3);
    co_await apb.read_expect("irq pending from irq_i", kIrqPending, 0x8);
}
```

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

These pieces compose into ordinary driver, monitor, and scoreboard code:

```cpp
struct Sample {
    uint32_t expected;
    uint32_t actual;
};

Task<uint32_t> make_word(uint32_t index) {
    co_return index ^ 0xa5a5'5a5a;
}

Task<void> driver(Dut dut, Channel<uint32_t>& expected) {
    for (uint32_t index = 0; index < 16; ++index) {
        const uint32_t word = co_await make_word(index);
        co_await wait_until(dut.ready,
                            [](uint32_t value) { return value != 0; },
                            dut.clk);
        dut.data.set(word);
        dut.valid.set(1);
        co_await RisingEdge{dut.clk};
        dut.valid.set(0);
        co_await expected.put(word);
    }
}

Task<void> monitor(Dut dut, Channel<uint32_t>& observed) {
    for (uint32_t count = 0; count < 16;) {
        co_await RisingEdge{dut.clk};
        if (dut.out_valid.get() == 0) continue;
        observed.put_nowait(dut.out_data.get());
        ++count;
    }
}

Task<void> scoreboard(Channel<uint32_t>& expected,
                      Channel<uint32_t>& observed) {
    for (uint32_t count = 0; count < 16; ++count) {
        const Sample sample{co_await expected.get(), co_await observed.get()};
        check_equal(sample.actual, sample.expected);
    }
}

Task<void> test(Dut dut) {
    Channel<uint32_t> expected;
    Channel<uint32_t> observed;
    co_await Join{driver(dut, expected), monitor(dut, observed),
                  scoreboard(expected, observed)};
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

The end-to-end multi-clock example is in `cpptb/examples/dpi_multiclock/`:

```sh
make cpp-coro-runtime-test
make cpp-dpi-multiclock-run
```

## Scheduler ordering and cleanup

Waiters registered on the same edge resume in registration order. Timers with
the same deadline also resume FIFO by registration order. Registrations made
stale by `First`, cancellation, or completion are removed without resuming the
coroutine; this cleanup includes edge queues and falling-edge interest counts,
not only the timer heap.

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

The peripheral performance guard uses an initial batch of 15 warmed, adjacent
C++ DPI/pure-SV pairs and alternates execution order. It compares the median of
the paired process-time ratios and hard-fails above `1.10x`. When the median
passes but its one-sided 95% upper confidence bound is inconclusive, the guard
collects one additional 15-pair batch and evaluates the combined samples; it
does not rerun the complete benchmark. A still-inconclusive passing median is
reported with a warning. Result artifacts include raw pairs and the
environment/build metadata needed to interpret them. Close ratios are treated
as noisy measurements, not evidence that either implementation is
directionally faster.

## Current scope

The coroutine runtime currently targets the Verilator-hosted VPI and DPI paths
in this repository. Signal values use `uint32_t`; four-state X/Z handling and
signals wider than 32 bits are not part of this API. Bounded channels remain
deferred. No compatibility claim is made for additional simulator backends.

The Authoring Core sources currently present under
`benchmarks/authoring_core/` exercise typed tasks, cycle waits, edge timeouts,
predicate waits, events, and channels. Its C++ DPI testbench is
`benchmarks/authoring_core/cpp_dpi/testbench.cpp`, the corresponding pure-SV
source is `benchmarks/authoring_core/pure_sv/authoring_core_sv_tb.sv`, and the
shared workload contract is `benchmarks/authoring_core/workload.py`. Runtime
API tests are in `cpptb/tests/coro_runtime_test.cpp`.

The wrapper module `vpi_apb_event_unit` keeps all testbench-driven signals as
internal `logic` objects. That matches the counter example and avoids driving a
child DUT port copy that Verilator overwrites from its top-level input.

## Reusable DPI runtime

`cpptb/dpi_runtime.hpp` owns the design-independent DPI host behavior:

- input/output array transport and driven-signal tracking;
- typed signal `get()`/`set()` callbacks and dirty-output detection;
- scheduler construction, edge dispatch, and delay deadlines;
- falling-edge interest and precision-aware time transport;
- timeout invocation, elapsed wall time, completion, and result reporting;
- the three standard C exports expected by the generated wrapper.

`cpptb/test_result.hpp` keeps the standard check/failure result contract
independent of DPI, so user-facing fixtures do not include simulator transport
headers.

A design supplies a small `DpiAdapter` containing its DUT and result types,
generated signal metadata, binding call, testbench registration call, result
name, and timeout policy. `CPPTB_DEFINE_DPI_RUNTIME(Adapter)` provides the C
entry points. No design transport needs to copy open arrays, decode events, or
format a result line.

## Benchmark

The cocotb comparison benchmark is in `benchmarks/cocotb_cpp_compare/`:

```sh
python3 benchmarks/cocotb_cpp_compare/run_benchmark.py --iters 1000 --runs 3
```

It runs the same APB event-unit traffic in cocotb and in the C++ coroutine
model, then writes results to `benchmarks/cocotb_cpp_compare/results/`.
