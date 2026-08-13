# Tasks and concurrency

This is the reference for running more than one thing at a time: spawning
concurrent processes, racing and joining them, passing data between them, and
cancelling them cleanly.

Every coroutine that does not return a value is declared `Task<void>`.
`Task<T>` carries a typed result from `co_return` to `co_await`, including
move-only values. Tasks themselves are move-only and can only be awaited as
rvalues; a completed result is moved to the awaiting coroutine. Scheduler
roots passed to `spawn()` or `spawn_detached()` remain `Task<void>`, and every
child passed to `Join` must also be a `Task<void>`.

If you have not written a cpptb test yet, start with
[Getting started](getting-started.md) and [Core ideas](core-ideas.md).

## The waits at a glance

Everything a cpptb coroutine can wait on, in one place. **Simulator waits** can
advance simulation time; **coordination waits** resume when another coroutine
acts and add no delay of their own.

| Simulator waits | | Coordination waits | |
|---|---|---|---|
| `RisingEdge{sig}` | low-to-high | `event` / `event.wait()` | another process calls `event.set()` |
| `FallingEdge{sig}` | high-to-low | `queue.put(v)` / `queue.get()` | space, or an item, is available |
| `Edge{sig}` | either transition | `lock.acquire()` / `semaphore.acquire()` | ownership is handed over |
| `clock_cycles(clk, n)` | after `n` rising edges | `process` | that process finishes |
| `Delay{10_ns}` | after a delay | `Join{a, b, c}` | all children finish |
| `ReadWrite{}` | queued writes flush here | `First{a, b}` | the first fires; returns its index |
| `ReadOnly{}` | end of timestep | `with_timeout(x, 100_ns)` | `x` completes, or the deadline expires |
| `NextTimeStep{}` | next timestep | `wait_until(sig, pred, clk)` | the predicate turns true |

Each is used directly with `co_await`. The phase waits — `ReadWrite`,
`ReadOnly`, `NextTimeStep` — are supplied by the timing backend that every
cpptb project gets by default; see
[The write model](scheduling.md#the-write-model).

[Scheduling](scheduling.md) is the authoritative reference for the simulator
waits: exactly when each resumes, and what state you are guaranteed to observe
when it does. The rest of this page covers the coordination waits, which are
what you use to make several processes cooperate.

## Racing, joining, and deadlines

- `co_await First{RisingEdge{signal}, Delay{100_ns}}` races two or more
  triggers and returns the zero-based index of the one that fired first.
- `co_await Join{producer(tb), consumer(tb), scoreboard(tb)}` runs two or more
  concurrent `Task<void>` children and resumes once all have finished.
- `co_await with_timeout(RisingEdge{signal}, 100_ns)` races a rising, falling,
  or either-edge trigger against a `SimTime` deadline. It returns
  `TimeoutOutcome::Triggered` or `TimeoutOutcome::TimedOut`.
- `co_await with_timeout(operation(), 100_ns)` races any `Task<T>` against a
  deadline. It returns `TimeoutResult<T>`; `has_value()`, `triggered()`, and
  boolean conversion report completion, `timed_out()` reports timeout, and
  `value()`, `operator*`, and `operator->` access a completed value. The
  `Task<void>` specialization provides the same state queries and a checked
  no-op `value()` for completed operations.
- `co_await wait_until(signal, predicate, clock)` evaluates the predicate
  immediately and, while it is false, polls it after each rising edge of
  `clock`. The predicate receives the signal's `uint32_t` value. Because it
  polls on a clock, it is the one coordination wait that does advance time.

## Events and queues

`Event` is a sticky, manually reset notification. `set()` leaves it set and
wakes all current waiters in FIFO registration order; later waits also finish
immediately until `clear()` is called. Both `co_await event.wait()` and
`co_await event` are supported, and `is_set()` reports the current state.

`Queue<T>` is a FIFO for move-constructible values. Its optional constructor
argument sets the maximum number of queued items; zero means unbounded.

```cpp
Queue<Packet> requests{8};

co_await requests.put(std::move(packet));  // waits only while full
Packet next = co_await requests.get();

const bool accepted = requests.put_nowait(std::move(another_packet));
std::optional<Packet> available = requests.get_nowait();
```

`put_nowait()` returns `false` when a bounded queue is full and does not move
from its argument on that path. `get_nowait()` returns an empty `optional` when
no unreserved item is available. `empty()`, `full()`, `size()`, and `maxsize()`
report the current state. Blocking producers and consumers resume in FIFO
registration order. Move-only payloads are supported by every operation.

## When to use a queue

Use a queue when one coroutine produces typed data that another coroutine must
consume in FIFO order. Common examples are a sequence feeding a driver, a
monitor feeding a scoreboard, or a reference model publishing expected
transactions. A bounded queue is useful when the producer must not outrun the
consumer indefinitely:

```cpp
Task<void> source(Queue<Packet>& packets) {
    for (uint32_t index = 0; index < kPacketCount; ++index) {
        co_await packets.put(make_packet(index));
    }
}

Task<void> driver(Dut dut, Queue<Packet>& packets) {
    for (uint32_t index = 0; index < kPacketCount; ++index) {
        Packet packet = co_await packets.get();
        co_await drive_packet(dut, packet);
    }
}

Queue<Packet> packets{4};
co_await Join{source(packets), driver(dut, packets)};
```

Prefer blocking `put()` and `get()` for normal producer/consumer flow. Use
`put_nowait()` or `get_nowait()` only when failure to transfer immediately is
itself part of the test logic. Avoid polling `empty()` or `full()` around a
blocking operation; another coroutine can change the queue before the next
statement runs.

A queue transfers one value to one consumer. Use `Event` for a payload-free
broadcast notification, `Join` to wait for a fixed group of tasks, `Semaphore`
to limit concurrent resource use, and `Lock` to serialize a short update to
shared C++ state. Queue operations do not advance simulation time and do not
model DUT protocol timing: the driver still writes signals and awaits edges or
delays explicitly.

## Locks and semaphores

`Semaphore` controls a count of available permits. `try_acquire()` is the
nonblocking operation, `co_await acquire()` waits in FIFO order, `release()`
returns one or more permits, and `available()` reports permits that have not
already been handed to a waiter.

```cpp
Semaphore credits{4};

co_await credits.acquire();
co_await send_transaction();
credits.release();
```

`Lock` provides `try_acquire()`, `co_await acquire()`, `release()`, and
`locked()`. It performs direct FIFO ownership handoff to a waiting coroutine:

```cpp
co_await scoreboard_lock.acquire();
model.apply(transaction);
scoreboard_lock.release();
```

The lock does not track a coroutine owner. Code that has acquired a lock or
semaphore permit remains responsible for releasing it, including on early
returns. Cancellation safely removes tasks that are still waiting and returns
any queue slot, lock handoff, or semaphore permit reserved for them.

An `Event`, queue, lock, or semaphore must outlive its active waiters;
destroying one with live waiters aborts with a diagnostic. Current waiters must
belong to one scheduler, although the object may be reused with another
scheduler after its wait queue is empty. Scheduler destruction and process
cancellation invalidate their registrations, which are removed during cleanup.
Unused synchronization objects do not register scheduler waits. This is a
lifetime property of the API, not a benchmark claim about hot-path cost.

`TimeoutResult<T>` stores its result in an `optional`, so `T` need only be
move-constructible and does not need a default constructor. Calling `value()`
after a timeout aborts with a diagnostic. A timed-out task is recursively
cancelled, including nested tasks and waits on `Process`, `Event`, or
`Queue`; its frames are destroyed at the existing scheduler cleanup
boundary. If task completion and the deadline share a timestamp, completion
wins when the task's result is present as the parent resumes. Zero and
sub-precision timeout durations are rejected under the same rules as
`Delay`.

## Putting it together

These pieces compose into ordinary driver, monitor, and scoreboard code. The
following is the core of the runnable `fifo_scoreboard` example:

```cpp
constexpr uint32_t kWordCount = 24;

Task<void> scoreboard(TestContext& test,
                      Queue<uint32_t>& expected_words,
                      Queue<uint32_t>& observed_words) {
    for (uint32_t index = 0; index < kWordCount; ++index) {
        const uint32_t expected = co_await expected_words.get();
        const uint32_t actual = co_await observed_words.get();
        test.expect_eq("FIFO payload", actual, expected);
    }
}

Task<void> fifo_test(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
    test.start_clock(dut.clk, 10_ns);

    Event reset_done;
    Queue<uint32_t> expected_words;
    Queue<uint32_t> observed_words;

    co_await Join{reset_dut(dut, reset_done),
                  input_driver(dut, reset_done, expected_words),
                  output_ready_driver(dut, reset_done),
                  output_monitor(dut, reset_done, observed_words),
                  scoreboard(test, expected_words, observed_words)};
}

CPPTB_REGISTER_TEST(fifo_test);
```

## Spawning and cancelling processes

A spawned process can be awaited or cancelled explicitly:

```cpp
auto monitor = test.spawn(monitor_bus(dut));
auto driver = test.spawn(drive_packets(dut));

co_await driver;
monitor.cancel();
```

`TestContext::now()` reports the scheduler's current absolute simulation time.
`spawn_detached()` starts a root that needs no handle. Neither signal writes
nor backdoor operations add an implicit delay; drive, wait, settle, and sample
remain visible in user code.

The `TestContext&` passed to the registered root refers to a context stored in
that root's coroutine frame. A child that may outlive the root must not retain
that reference. Pass `TestContext` by value to such a child, especially a
detached child:

```cpp
Task<void> detached_monitor(Dut dut, TestContext test) {
    co_await RisingEdge{dut.alert};
    co_await ReadOnly{};
    test.expect_eq("alert payload", dut.payload.get(), 0x42u);
}

Task<void> root_test(Dut dut, TestContext& test) {
    test.spawn_detached(detached_monitor(dut, test));
    co_return;
}
```

Copying `TestContext` copies its handles to the scheduler and result; it does
not schedule work or advance simulation time. Children that are joined before
the root returns may use the reference as long as their lifetime is bounded by
that join.

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
Use `TestContext::spawn_detached()` when user code needs no process handle. It
is still owned by the current test so failures are attributed correctly and
unfinished work is cancelled when that test ends. The lower-level scheduler
has a metadata-free detached primitive, but user testbenches should use the
context method for lifecycle ownership.

`examples/watchdog_timeout/testbench.cpp` shows both forms of `with_timeout()`
and a complete `spawn()`/`cancel()`/await lifecycle. Its deadlines are kept
strictly separate from response times so the expected result is independent of
same-timestamp ordering.

For typed transaction endpoints, analysis fan-out, and scoreboards built on
these primitives, see
[Verification components](verification-components.md). For exactly when each
trigger resumes, see [Scheduling](scheduling.md).

## Related APIs

- [Coordination reference](library/coordination.md) — the signatures for
  `Task`, `Join`, `First`, `with_timeout`, `Event`, `Queue`, `Lock`, and
  `Semaphore`, with their cancellation guarantees.
- [FIFO scoreboard example](examples/fifo-scoreboard.md) — the runnable
  project behind this page's driver/monitor/scoreboard excerpts.
- [Watchdog timeout example](examples/watchdog-timeout.md) — `with_timeout`
  and recovery in a complete bench.
- [Reference card](refcard.md) — the concurrency idioms, one line each.
