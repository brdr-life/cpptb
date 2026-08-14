# Scheduling

Read this page when you need to know exactly when a coroutine resumes — which
is most of what separates a testbench that works from one that reads the
previous cycle's values. It defines every trigger, what each one guarantees
about the state you observe, and how concurrent processes are ordered.

cpptb maps coroutine suspension points to simulator events. Ordinary C++ runs
inside the current DPI callback until the coroutine reaches `co_await`.
See [clocking](clocking.md) for clock registration, waveform ownership, and
the clock-edge callback policy.

## Trigger reference

Every `co_await` in a cpptb testbench falls into one of two groups, and the
difference is the one worth internalizing: **simulator waits can advance
simulation time; coordination waits cannot advance it by themselves.**

### Simulator waits

The simulator decides when these resume, so time may pass.

| Wait | Resumes | Notes |
|---|---|---|
| `co_await RisingEdge{signal}` | on a low-to-high transition | any one-bit signal, including DUT-produced clocks and hierarchy objects |
| `co_await FallingEdge{signal}` | on a high-to-low transition | |
| `co_await Edge{signal}` | on either transition | |
| `co_await clock_cycles(clock, count)` | after `count` rising edges of `clock` | `count == 0` completes without suspending |
| `co_await Delay{10_ns}` | after an absolute simulation-time delay | works in designs with no clocks |
| `co_await ReadWrite{}` | after the current evaluation settles | may still read and drive; phase wait |
| `co_await ReadOnly{}` | at the stable end of the current timestep | writes are an error here; phase wait |
| `co_await NextTimeStep{}` | at the start of the next scheduled timestep | before that timestep's HDL evaluation; phase wait |

The last three are *simulator phase* waits. They are how a testbench hooks into
a specific point of any timestep, and they are what make cpptb's timing match
cocotb's. Every project has them: `timing_backend` defaults to
`"verilator-direct"`, and the only other choice is `"vpi"`. See
[Timing backend support](#timing-backend-support) for how the two differ --
they are held to identical results, so the choice is about speed and
portability, not semantics.

### Coordination waits

These resume when another coroutine acts, not when the simulator does. They add
no delay of their own; time passes only if the work they are waiting on
suspends on a simulator wait.

| Wait | Resumes |
|---|---|
| `co_await event` / `co_await event.wait()` | when another process calls `set()` |
| `co_await queue.put(v)` / `co_await queue.get()` | when space, or an item, becomes available |
| `co_await lock.acquire()` / `co_await semaphore.acquire()` | when ownership or a permit is handed over |
| `co_await process` | when that spawned process completes or is cancelled |
| `co_await Join{a, b, c}` | when every child task has finished |
| `co_await First{a, b}` | when the first of several triggers fires; returns its index |
| `co_await with_timeout(x, 100_ns)` | when `x` completes, or the deadline expires |
| `co_await wait_until(signal, predicate, clock)` | when the predicate is true, re-evaluated on each rising edge of `clock` |

`wait_until` is the one hybrid: it polls on a clock, so it does advance time
while the predicate stays false.

[Tasks and concurrency](testbench-authoring.md) covers the coordination waits
in depth. The rest of this page covers the simulator waits.

## Time and edges

`test.start_clock(dut.clk, 10_ns)` starts a periodic DUT input clock. Call it
before the test's first `co_await`; it does not itself advance time.
`clock_cycles` is not tied to a primary or generated clock — it counts rising
edges of whichever clock you name.

`Delay` accepts integer `fs`, `ps`, `ns`, `us`, and `ms` literals. A duration
must be positive and representable at the wrapper's configured
`timeprecision`; zero and sub-precision durations abort with a diagnostic.

For multiple domains, initialize and register each input separately. The
optional phase argument offsets the first edge by `phase + period / 2`:

```cpp
dut.core_clk.set_now(0);
dut.peripheral_clk.set_now(0);
test.start_clock(dut.core_clk, 4_ns);
test.start_clock(dut.peripheral_clk, 10_ns, 1_ns);
```

Do not call `start_clock()` for an output clock. A divided, recovered, or gated
DUT output is observed with the same `RisingEdge`, `FallingEdge`, and `Edge`
primitives as any other one-bit signal.

An edge wait resumes in the simulator callback associated with that edge. To
observe logic after the design has evaluated, follow the edge with
`co_await ReadOnly{}` — the settle idiom every clocked example uses. An
explicit `Delay{1_ps}` remains the right tool only in clockless benches that
step physical time directly. Signal writes and backdoor operations never
settle anything automatically.

## Simulator phases

Use phase waits when a test needs a specific point within the current or next
simulator timestep, rather than a particular signal transition.

`ReadWrite{}` is the drive anchor: the coroutine may read and drive there. A
coroutine resuming at `ReadWrite{}` finds its own queued writes freshly
flushed but not yet re-evaluated, so reads at that point still see the
pre-flush design state; by a later `ReadOnly{}` in the same timestep the
flushed writes are evaluated and settled. `ReadOnly{}` is the sample anchor:
it is for observation and checking, and `set()`, `deposit()`, `force()`, and
`release()` all report an error in that phase. `NextTimeStep{}` steps out of
the current timestep entirely, resuming before the next one's HDL
evaluation — which is how a test leaves `ReadOnly` in order to drive again.

```cpp
dut.request.set(request);   // queued; flushes at this timestep's ReadWrite
co_await ReadOnly{};        // flushed and evaluated: safe to check
test.expect_eq("combinational request", dut.request_seen.get(), request);

co_await NextTimeStep{};    // leave ReadOnly before driving again
dut.request.set(next_request);
```

These waits describe simulator ordering, not arbitrary delays. Use `Delay` for
elapsed simulation time and edge waits for a particular signal transition.
The Verilator backend dispatches these phases directly when cpptb owns the
host loop; other supported simulator integrations use the equivalent standard
VPI callbacks. Both paths run the same conformance contracts.

### Timing backend support

Generated DPI remains the signal and data transport in every mode. The timing
backend only determines how the simulator resumes `ReadWrite`, `ReadOnly`, and
`NextTimeStep` waiters.

cpptb supports exactly two timing backends, selected by name in `cpptb.toml`:

```toml
[build]
timing_backend = "verilator-direct"   # fastest; Verilator's scheduler, driven directly
# or
timing_backend = "vpi"                # standard VPI callbacks; the portable route
```

| Timing backend | Status | Timing contract | Portability |
|---|---|---|---|
| `verilator-direct` | Supported; conformance-checked by every `make test` | Complete | Verilator-specific |
| `vpi` | Supported; conformance-checked by every `make test` | Complete | Standard simulator API; cross-simulator validation is roadmap milestone 6 |
| Generated SV-DPI calendar | Experimental; not selectable through `timing_backend` | Complete for generated and observed events | Cross-simulator validation pending |

The generated calendar owns framework clocks and timers and observes selected
external signals, but standard DPI cannot query the simulator's complete event
queue. Its `NextTimeStep` therefore cannot yet promise to wake for an arbitrary
unobserved internal DUT event. Direct Verilator dispatch and standard VPI are
the supported contract-complete choices. Both own the host loop through
`src/verilator_timing_main.cpp`, and `timing_backend` emits that link; the two
build identically apart from one define. See [Performance](performance.md) for
the exact backend comparison.

The two backends are held to more than passing the same tests. Every
`make test` runs three equivalence layers between them: the 292-check
scheduler conformance contract on each; the deferred-write contract on
each; and `make backend-equivalence-test`, which builds the same
examples once per backend with `--wave` and requires the runs to come
out **identical** — result records field for field (wall time excepted,
simulation time included) and wave dumps byte for byte. Choosing a
backend changes how fast the simulation runs and nothing else; see
[Waveforms](waveforms.md#backend-identity) for what the dump comparison
covers.

:::{note}
**There is no build without a timing backend.** `timing_backend` defaults to
`"verilator-direct"`, so every build links a backend that dispatches the full
phase contract without any configuration. Hand-assembled alternatives are
rejected rather than left to answer wrongly: `verilator_args = ["--vpi"]`
used to build a bridge that placed writes on the right edge while failing two
of the five contract checks silently -- `ReadOnly` did not observe a write
settled in `ReadWrite` -- and the build tool now refuses it, naming the key.
The timing defines (`CPPTB_SV_DPI_TIMING` and friends) are likewise owned by
the key: setting them in `design.defines` or `build.cxx_flags` is an error,
and the remaining SV-DPI pump and calendar builds are reachable only through
the conformance runner, as experiments.
:::

## The write model

cpptb's documented write model is cocotb's. `set()` **queues** the write, and
the queue flushes at the ReadWrite point of the current timestep. A write made
straight after an awaited edge therefore lands for the *next* edge — exactly
what `dut.sig.value = x` does in cocotb.

This is the default. A project gets it without configuring anything:
`timing_backend` defaults to `"verilator-direct"` and `deferred_writes`
defaults to `true`. Every code sample in this documentation assumes them.

Name them explicitly to pick the other backend, as every example project in
this repository does:

```toml
[build]
timing_backend = "verilator-direct"   # or "vpi"
deferred_writes = true
```

The two keys travel together: a queued write is applied at a simulator phase,
so `deferred_writes` needs a timing backend, and there is no supported way to
build without one.

### What the model guarantees

Pinned by `tests/integration/deferred_writes` on both backends in every
`make test`:

- a `get()` between `set()` and the flush returns the simulator's value, not
  the queued one, matching cocotb's caching;
- by `ReadOnly` of the same timestep the write is applied and settled;
- `set_now()` is the immediate deposit -- cocotb's `setimmediatevalue()` --
  used for initialization and the rare intentional same-edge write;
- writing from `ReadOnly` still fails at the offending line, because write
  legality is checked at the call, not at the flush.

Queueing alone arms the phase: writes flush whether or not anything awaits
`ReadWrite{}`.

### Immediate writes are legacy

A build with `deferred_writes = false` applies `set()` immediately. That behavior
predates the timing backends and is intended for deprecation, so that cpptb
matches cocotb's semantics without a per-project switch. It is not a supported
authoring style: testbenches written against it need a drive-point convention
of their own to keep writes off the edge being awaited, and nothing in the API
states or checks that convention. Set both keys and the question does not
arise.

## Composition

Use an ordinary `co_await task()` when the next operation is sequential. The
composition primitives cover the cases where work must overlap:

- `Join{...}` owns a fixed group of child tasks and resumes after all finish.
- `First{...}` races two or more triggers and returns the winner's zero-based
  index.
- `with_timeout(...)` adds a checked deadline to one trigger or task.
- `spawn()` returns a `Process` handle for work whose lifetime is controlled
  dynamically.
- `Event` broadcasts state changes; `Queue<T>` transfers FIFO data between
  tasks.

No composition primitive advances time by itself. Time advances only when a
child suspends on an edge, cycle count, or `Delay`.

### Driver, monitor, and scoreboard

`Join` is the natural shape when every component must complete before the test
can finish. The runnable [FIFO scoreboard example](examples/fifo-scoreboard.md)
joins reset, two drivers, a monitor, and a scoreboard:

```cpp
Task<void> fifo_test(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
    test.start_clock(dut.clk, 10_ns);

    Event reset_done;
    Queue<uint32_t> expected_words;
    Queue<uint32_t> observed_words;
    uint32_t input_stalls = 0;

    co_await Join{reset_dut(dut, reset_done),
                  input_driver(dut, reset_done, expected_words, input_stalls),
                  output_ready_driver(dut, reset_done),
                  output_monitor(dut, reset_done, observed_words),
                  scoreboard(test, expected_words, observed_words)};
}
```

The parent owns the synchronization objects, and `Join` guarantees that every
child is finished before those objects leave scope. `Event` is sticky, so a
driver waiting after reset has already completed still resumes immediately:

```cpp
Task<void> reset_dut(Dut dut, Event& reset_done) {
    dut.rst_n.set(0);
    dut.in_valid.set(0);
    dut.in_data.set(0);
    dut.out_ready.set(0);

    co_await clock_cycles(dut.clk, 2);
    dut.rst_n.set(1);
    reset_done.set();
}

Task<void> input_driver(Dut dut, Event& reset_done,
                        Queue<uint32_t>& expected_words,
                        uint32_t& input_stalls) {
    co_await reset_done;

    // RisingEdge resumes before the design evaluates that edge, so a get()
    // here reads the value the DUT is about to sample, and a set() applies
    // after this edge's own updates -- in time for the next one.
    co_await RisingEdge{dut.clk};
    for (uint32_t index = 0; index < kWordCount; ++index) {
        const uint32_t word = next_word(state);
        dut.in_data.set(word);
        dut.in_valid.set(1);
        while (true) {
            co_await RisingEdge{dut.clk};
            if (dut.in_ready.get() != 0) break;
            ++input_stalls;
        }
        expected_words.put_nowait(word);
    }
    dut.in_valid.set(0);
}
```

Queues decouple production from checking. `get()` suspends only while the
FIFO is empty, allowing the driver and monitor to run at different rates:

```cpp
Task<void> scoreboard(TestContext& test,
                      Queue<uint32_t>& expected_words,
                      Queue<uint32_t>& observed_words) {
    for (uint32_t index = 0; index < kWordCount; ++index) {
        const uint32_t expected = co_await expected_words.get();
        const uint32_t actual = co_await observed_words.get();
        test.expect_eq("FIFO payload", actual, expected);
    }
}
```

### Coming from cocotb

The trigger vocabulary is deliberately the same: `RisingEdge`, `FallingEdge`,
`ReadOnly`, `ReadWrite`, `NextTimeStep`, and drivers, monitors and scoreboards
compose the same way. One difference changes how a driver must be written, and
translating a testbench line for line will produce a driver that acts a cycle
early.

#### Writes translate directly

In cocotb, assigning to a signal queues the write and applies it at the next
`ReadWrite` point. Writing straight after `await RisingEdge(clk)` therefore
cannot affect the edge just awaited; it behaves like a non-blocking assignment.
[The write model](#the-write-model) makes `set()` do exactly the same thing,
so a cocotb driver translates line for line:

```python
# cocotb: the write is queued, so this edge is already over for it.
async def driver(dut):
    while True:
        await RisingEdge(dut.clk)
        dut.wdata.value = next_word()      # lands for the *next* edge
        dut.wvalid.value = 1
```

```cpp
// cpptb: identical shape, identical semantics.
Task<void> driver(Dut dut) {
    while (true) {
        co_await RisingEdge{dut.clk};
        dut.wdata.set(next_word());        // queued; lands for the *next* edge
        dut.wvalid.set(1);
    }
}
```

Only a legacy `deferred_writes = false` build behaves differently — there
`set()` applies at once and this shape drives into the edge being awaited.
That mode needs a drive-point convention of its own and is
[not a supported authoring style](#immediate-writes-are-legacy).

#### Monitors

Sampling needs no change of shape, and the reason differs from cocotb's.
cocotb reads after `await ReadOnly()` because a read taken directly on an edge
trigger is not guaranteed to see settled values. `co_await RisingEdge{}` here
resumes before the design evaluates the edge, so it deterministically yields
the values a signal held during the cycle just ending, which is what
`always_ff @(posedge clk)` samples.

```python
# cocotb
async def monitor(dut, queue):
    while True:
        await RisingEdge(dut.clk)
        await ReadOnly()
        if dut.valid.value:
            queue.append(dut.data.value)
```

```cpp
// cpptb
Task<void> monitor(Dut dut, Queue<uint32_t>& queue) {
    while (true) {
        co_await RisingEdge{dut.clk};
        if (dut.valid.get()) co_await queue.put(dut.data.get());
    }
}
```

#### A driver and monitor together

Because writes queue, one loop can sample and drive at the same anchor
without racing itself: the reads see the cycle just ending, and the writes
land for the next one.

```cpp
Task<void> agent(Dut dut, RegisterModel& model) {
    while (true) {
        co_await RisingEdge{dut.clk};
        if (dut.access.get()) model.check(dut.addr.get(), dut.rdata.get());

        const auto next = stimulus.next();
        dut.addr.set(next.addr);           // queued; lands for the next edge
        dut.wdata.set(next.wdata);
        dut.access.set(next.valid);
    }
}
```

#### What to change when translating

With the standard configuration, writes carry over unchanged. The one thing to
watch is where a *read* is anchored, because `co_await RisingEdge{}` resumes
before the design evaluates the edge while cocotb's resumes after it.

| cocotb | here |
|---|---|
| `await RisingEdge(clk)` then assign | `co_await RisingEdge{clk}` then `set()` — the queued write lands on the next edge, as in cocotb |
| `await RisingEdge(clk)`, `await ReadOnly()`, then read | `co_await RisingEdge{clk}`, `co_await ReadOnly{}`, then `get()` |
| `await ReadWrite()` before driving | `co_await ReadWrite{}` |
| `await RisingEdge(clk)` then read protocol pins | `co_await RisingEdge{clk}`, `co_await ReadWrite{}`, then `get()` — a bare edge reads pre-evaluation values |

A bare `co_await RisingEdge{}` followed by `get()` is exactly right for a
monitor: it yields the values the design is about to sample. It is the wrong
anchor for a driver that reads protocol pins, which is
[trap 1](coming-from-cocotb.md#1-risingedge-resumes-before-the-edge-evaluates).

The three translation traps — where a bare edge read is right and where it is
not — are worked through with symptoms in
[Coming from cocotb](coming-from-cocotb.md#the-traps-in-one-worked-example).

### Bound producer pressure and shared resources

A capacity makes queue pressure part of the schedule. `put()` suspends only
while all slots are occupied or reserved for earlier producers; `get()` frees
one slot and hands it directly to the oldest producer waiter:

```cpp
Task<void> packet_source(Queue<Packet>& packets) {
    for (uint32_t index = 0; index < kPacketCount; ++index) {
        co_await packets.put(make_packet(index));
    }
}

Task<void> packet_driver(Dut dut, Queue<Packet>& packets) {
    for (uint32_t index = 0; index < kPacketCount; ++index) {
        Packet packet = co_await packets.get();
        co_await drive_packet(dut, packet);
    }
}

Queue<Packet> packets{4};
co_await Join{packet_source(packets), packet_driver(dut, packets)};
```

`Semaphore` is useful for a protocol with a bounded number of outstanding
transactions. `Lock` serializes short updates to a shared reference model:

```cpp
co_await outstanding.acquire();
co_await request_queue.put(request);

co_await model_lock.acquire();
model.apply(response);
model_lock.release();
outstanding.release();
```

Neither primitive advances simulation time. A task that has acquired a lock
or permit must release it explicitly; cancellation automatically cleans up
only tasks that are still waiting.

### Race an event against a deadline

`First` is useful when the caller needs to know which trigger won. This pattern
from the [multiple-clocks example](examples/multiclock.md) gives the clock edge
index `0` and the deadline index `1`:

```cpp
const auto winner =
    co_await First{RisingEdge{dut.read_clk}, Delay{100_ns}};

test.expect_eq("read clock beat deadline",
               static_cast<uint32_t>(winner), 0u);
```

The losing trigger registration is removed. For the common two-way deadline
case, `with_timeout()` gives the outcome a name instead of exposing an index:

```cpp
const auto outcome =
    co_await with_timeout(RisingEdge{dut.response_valid}, 100_ns);

test.expect_eq("response arrived",
               outcome == TimeoutOutcome::Triggered, true);
```

### Put a deadline around a transaction

`with_timeout()` also accepts a typed task. Completion returns its value;
timeout recursively cancels the transaction and any nested waits. The
[watchdog example](examples/watchdog-timeout.md) uses both outcomes:

```cpp
auto response = co_await with_timeout(
    transaction(dut, request_word, 3, false), 200_ns);

test.expect_eq("transaction completed", response.has_value(), true);
if (response) {
    test.expect_eq("response payload", response.value(), expected_word);
}

auto stalled = co_await with_timeout(
    transaction(dut, stalled_word, 2, true), 60_ns);
test.expect_eq("stalled transaction timed out", stalled.timed_out(), true);
```

Use `First` when multiple heterogeneous triggers are meaningful to the caller.
Use `with_timeout` when one operation either completes or exceeds a deadline.

### Control a long-lived process

Use `spawn()` when a monitor or service should outlive the immediate sequence
and needs an explicit handle. A common verification shape is a background
monitor feeding a queue while the foreground sequence drives and checks
transactions:

```cpp
Task<void> response_monitor(Dut dut, Queue<uint32_t>& observed) {
    while (true) {
        co_await RisingEdge{dut.response_valid};
        co_await ReadOnly{};
        co_await observed.put(dut.response_data.get());
    }
}

Task<void> request_test(Dut dut, TestContext& test) {
    Queue<uint32_t> observed{8};
    auto monitor = test.spawn(response_monitor(dut, observed));

    for (uint32_t request = 0; request < 32; ++request) {
        co_await drive_request(dut, request);
        const uint32_t actual = co_await observed.get();
        test.expect_eq("response payload", actual,
                       expected_response(request));
    }

    monitor.cancel();
    co_await monitor;
    test.expect_eq("monitor cancelled", monitor.cancelled(), true);
}
```

The queue, monitor, and sequence overlap in simulation time. Cancelling and
awaiting the monitor before `observed` leaves scope makes the ownership
boundary explicit. An uncaught monitor exception is attributed to this test
and to the monitor's spawn location.

Use `Join` when the child set and lifetime are lexical. Use `spawn` when code
must query, await, or cancel a process later. `spawn_detached()` is reserved for
roots that truly need no handle; pass `TestContext` by value if such a root may
outlive the registered test coroutine.

### Coordinate independent clocks

Composition is clock-agnostic. Each child can wait on its own input or
DUT-produced clock while one `Join` defines the overall test lifetime:

```cpp
dut.write_clk.set_now(0);
dut.read_clk.set_now(0);
test.start_clock(dut.write_clk, 4_ns);
test.start_clock(dut.read_clk, 6_ns, 1_ns);

co_await Join{reset_dut(dut, test),
              producer(dut),
              consumer(dut, test),
              output_clock_probe(dut, test)};
```

The producer awaits `write_clk`, the consumer awaits `read_clk`, and the probe
awaits a clock generated by the DUT. The scheduler does not require a single
global testbench clock. The [clockless timers example](examples/timer-only.md)
shows the same `Join` composition with two independent `Delay` cadences and no
clock at all.

Waiters sharing an edge or deadline resume in registration order. Equal-time
events from different simulator processes retain simulator process ordering;
testbenches should not infer hardware priority from that ordering.

## Wait graphs and deadlock diagnostics

`TestContext::wait_graph()` returns an allocation-owning snapshot of every
active coroutine in the current test. `Testbench::wait_graph()` provides the
same scheduler-level view for custom harnesses. Snapshotting is read-only and
does not resume, cancel, or otherwise alter a process.

Give synchronization resources short names when their role is not obvious
from the declaration:

```cpp
Event reset_done{"reset_done"};
Queue<uint32_t> observed_words{16, "observed_words"};
Semaphore credits{0, "response_credits"};
Lock bus_lock{"bus_lock"};

auto monitor = test.spawn(response_monitor(dut, observed_words));
auto graph = test.wait_graph();

for (const auto& node : graph.nodes) {
    test.logger("diagnostics").debug([&] {
        return "process " + std::to_string(node.process_id) +
               " waits on " + std::string(wait_reason_name(node.reason));
    });
}
```

Use `co_await reset_done.wait()` when the exact wait call site matters. Direct
`co_await reset_done` still reports the named event and its declaration, but it
cannot capture the operator call site. Blocked queue operations currently
report the queue declaration; the `get()` and `put(value)` signatures remain
endpoint-compatible with verification components.

The snapshot contains stable coroutine IDs, logical process IDs and parent
process IDs, spawn provenance, outstanding trigger or synchronization reason,
resource name, wait start time, optional deadline, and child dependencies.
`format_wait_graph(graph)` produces deterministic line-oriented text suitable
for a terminal or CI artifact:

```text
cpptb wait graph at 2000000 fs: deadlocked (3 active coroutines)
  [1] root process #1 spawned at testbench.cpp:42
      waiting on task since 0 fs
      depends on [2]
  [2] root process #1 child-of [1] spawned at testbench.cpp:42
      waiting on task since 0 fs
      depends on [3]
  [3] root process #1 child-of [2] spawned at testbench.cpp:42
      waiting on Event (Event response_ready) since 0 fs at testbench.cpp:45
```

`WaitGraphSnapshot::status` explains what can make progress:

| Status | Meaning |
|---|---|
| `Complete` | No active coroutine remains |
| `Runnable` | At least one leaf coroutine is running or ready |
| `WaitingForTime` | A live delay or timeout can wake the scheduler |
| `WaitingForSimulator` | A signal edge or simulator phase can resume work |
| `Deadlocked` | Active leaf coroutines remain, but all are blocked on internal synchronization |

The deadlock classification is deliberately conservative. A process waiting on
`RisingEdge`, `ReadOnly`, or a future `Delay` is not called deadlocked merely
because it has not resumed yet. An `Event`, empty `Queue`, exhausted
`Semaphore`, or held `Lock` is classified as deadlocked only when no runnable,
timed, or simulator-driven leaf can release it.

Registered-test simulation timeouts automatically capture the graph before
the timed task is cancelled. A deadlocked timeout says so in `status_reason`,
prints the graph, and stores the structured snapshot in the result JSON. The
global generated-wrapper watchdog and end-of-simulation starvation hook use
the same formatter. This ordering is important: taking the snapshot after
cancellation would erase the wait that caused the timeout.

```cpp
CPPTB_REGISTER_TEST_WITH_OPTIONS(
    response_test,
    TestOptions{.simulation_timeout = 2_us});
```

The full integration regression is the `wait_graph_deadlock` negative case in
`tests/conformance/runtime/testbench.cpp`. It parks on a named event, lets the
real simulator watchdog expire, and requires the emitted report to identify
`Event response_ready`.

The clockless `timer_only_deadlock` example in
`examples/timer_only/testbench.cpp` exercises the separate no-future-event
path through the generated wrapper's SystemVerilog `final` hook. Its matching
pure-SystemVerilog negative test waits on the same logical event.

Projects with a handwritten transport that use
`CPPTB_DEFINE_NAMED_DPI_RUNTIME` keep the legacy six-argument API. After
regenerating wrappers with starvation reporting, migrate that transport to
`CPPTB_DEFINE_NAMED_DPI_RUNTIME_WITH_STARVATION` and pass the generated
`*_report_starvation` function name as its final argument.

## Scheduler ordering and cleanup

Waiters registered on the same edge resume in registration order. Timers with
the same deadline also resume FIFO by registration order. Registrations made
stale by `First`, cancellation, or completion are removed without resuming the
coroutine; this cleanup includes edge queues and falling-edge interest counts,
not only the timer heap.

C++-owned clocks are registered as static edge sources before the first wait.
The first `start_clock()` call selects the primary cycle counter; later calls
add independent domains and may specify a phase. Registered input-clock rising
edges are delivered on every cycle; falling edges are delivered while the
scheduler has a falling- or either-edge waiter. Waits on clock IDs do not
publish dynamic edge-interest masks. DUT-produced and manually driven edges use
interest-gated observers. See [clocking](clocking.md) for the concise user
contract.

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

Scheduler-facing objects are simulation-thread confined. Do not access a
`Testbench`, `TestContext`, signal handle, synchronization primitive, or
`Process` from a worker OS thread. Authored coroutine processes remain
concurrent in simulation time while executing through the one scheduler that
owns deterministic ordering.

## Related APIs

- [Awaitables reference](library/awaitables.md) — exact signatures and
  resume guarantees for every trigger and phase wait.
- [Signals reference](library/signals.md) — the write operations and the
  timing summary contrasting queued and immediate paths.
- [API reference](refcard.md) — the waiting and driving idioms, one line
  each.
- [Glossary](glossary.md) — settle point, drive anchor, phase contract, and
  the write model, defined in one place.
- [Troubleshooting](troubleshooting.md) — the phase-order and watchdog
  diagnostics this page's rules produce.
