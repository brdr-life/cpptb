# Scheduling

cpptb maps coroutine suspension points to simulator events. Ordinary C++ runs
inside the current DPI callback until the coroutine reaches `co_await`.
See [clocking](clocking.md) for clock registration, waveform ownership, and
the clock-edge callback policy.

## Time and edges

- `test.start_clock(dut.clk, 10_ns)` starts a periodic DUT input clock. Call it
  before the test's first `co_await`; it does not itself advance time.
- `co_await RisingEdge{signal}` resumes on a low-to-high transition.
- `co_await FallingEdge{signal}` resumes on a high-to-low transition.
- `co_await Edge{signal}` resumes on either transition.
- `co_await clock_cycles(clock, count)` counts rising edges of the selected
  clock. It is not tied to a primary or generated clock.
- `co_await Delay{10_ns}` resumes after an absolute simulator-time delay and
  works in designs with no clocks.

For multiple domains, initialize and register each input separately. The
optional phase argument offsets the first edge by `phase + period / 2`:

```cpp
dut.core_clk.set(0);
dut.peripheral_clk.set(0);
test.start_clock(dut.core_clk, 4_ns);
test.start_clock(dut.peripheral_clk, 10_ns, 1_ns);
```

Do not call `start_clock()` for an output clock. A divided, recovered, or gated
DUT output is observed with the same `RisingEdge`, `FallingEdge`, and `Edge`
primitives as any other one-bit signal.

An edge wait resumes in the simulator callback associated with that edge. Use
an explicit delay such as `Delay{1_ps}` when the testbench intends to observe
logic after a later simulator time slot. Signal writes and backdoor operations
never add that delay automatically.

## Simulator phases

Use phase waits when a test needs a specific point within the current or next
simulator timestep:

- `co_await ReadWrite{}` resumes after the current evaluation has settled.
  The coroutine may read and drive signals, and any writes are evaluated before
  a later `ReadOnly` waiter resumes.
- `co_await ReadOnly{}` resumes at the stable end of the current timestep. It
  is intended for observation and checking; `set()`, `deposit()`, `force()`,
  and `release()` report an error in this phase.
- `co_await NextTimeStep{}` resumes at the beginning of the next scheduled
  simulator timestep, before that timestep's HDL evaluation.

```cpp
dut.request.set(request);
co_await ReadWrite{};
test.expect_eq("combinational request", dut.request_seen.get(), request);

dut.request.set(next_request);
co_await ReadOnly{};
test.expect_eq("stable response", dut.response.get(), expected_response);

co_await NextTimeStep{};
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

!!! warning "Phase waits need a named backend"

    A default `cpptb build` links Verilator's own `--binary` main, which owns
    clocks and timers but dispatches no phases, so `ReadWrite`, `ReadOnly` and
    `NextTimeStep` fail at run time with a message naming the
    `timing_backend` key. Set the key and the same testbench passes; nothing
    in the source changes between backends.

    Hand-assembled alternatives are rejected rather than left to answer
    wrongly. `verilator_args = ["--vpi"]` used to build a bridge that placed
    writes on the right edge while failing two of the five contract checks
    silently -- `ReadOnly` did not observe a write settled in `ReadWrite` --
    and the build tool now refuses it, naming the key. The timing defines
    (`CPPTB_SV_DPI_TIMING` and friends) are likewise owned by the key: setting
    them in `design.defines` or `build.cxx_flags` is an error, and the
    remaining SV-DPI pump and calendar builds are reachable only through the
    conformance runner, as experiments.

    The edge-phase convention in
    [Sample on the edge, drive off it](#sample-on-the-edge-drive-off-it) needs
    no backend and stays correct in a default build; it is what the ports in
    `experiments/open_core_ports` use. [Roadmap](roadmap.md#candidate-directions)
    records the worked examples behind this design and the deferred-write work
    that builds on it.

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
    dut.clk.set(0);
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
    co_await clock_cycles(dut.clk, 2);
    co_await FallingEdge{dut.clk};
    dut.rst_n.set(1);
    reset_done.set();
}

Task<void> input_driver(Dut dut, Event& reset_done,
                        Queue<uint32_t>& expected_words) {
    co_await reset_done;

    const uint32_t word = 0x1234;
    dut.in_data.set(word);
    dut.in_valid.set(1);
    expected_words.put_nowait(word);
    co_await RisingEdge{dut.clk};
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

### Sample on the edge, drive off it

The example above drives reset and payload from `FallingEdge` and reads on
`RisingEdge`. That is a convention, not an accident, and it is worth stating
plainly because the alternative fails quietly.

`co_await RisingEdge{}` resumes *before* the design evaluates that edge. Every
signal still holds the value it had during the cycle just ending, which is what
`always_ff @(posedge clk)` samples, so it is the correct place to observe. It is
the wrong place to drive: `set()` takes effect immediately, so a value written
there is picked up by the very edge being awaited, and a write lands a cycle
earlier than intended.

```cpp
// Wrong: the value reaches the design in time for this edge.
co_await RisingEdge{dut.clk};
dut.wdata.set(value);

// Right: sample where the design has not yet moved, drive where it cannot
// be captured until the next edge.
co_await RisingEdge{dut.clk};
const uint32_t observed = dut.rdata.get();
co_await FallingEdge{dut.clk};
dut.wdata.set(value);
```

The failure mode is a register read-back returning the value just written
rather than the previous one, which reads as a design bug rather than a
testbench one. SystemVerilog testbenches avoid it by driving through
non-blocking assignments from inside `always_ff`; there is no non-blocking
assignment to reach for here, so the clock phase does the same job.

#### Or: the cocotb write model, `deferred_writes = true`

The convention exists because `set()` is immediate. Projects that select a
timing backend can select cocotb's write model instead:

```toml
[build]
timing_backend = "verilator-direct"   # or "vpi"; the mode requires one
deferred_writes = true
```

Under the mode, `set()` queues and the queue flushes at the start of the
ReadWrite phase -- after the awaited edge's own updates -- so the "wrong"
example above becomes correct: the write lands on the next edge, exactly as a
cocotb `dut.sig.value = x` does. The pinned semantics, checked on both
backends by `tests/integration/deferred_writes`:

- a `get()` between `set()` and the flush returns the simulator's value, not
  the queued one, matching cocotb's caching;
- by `ReadOnly` of the same timestep the write is applied and settled;
- `set_now()` is the immediate deposit -- cocotb's `setimmediatevalue()` --
  and keeps the old semantics for initialization and the rare intentional
  same-edge write;
- writing from `ReadOnly` still fails at the offending line, because write
  legality is checked at the call, not at the flush.

Queueing alone arms the phase: writes flush whether or not anything awaits
`ReadWrite{}`. The mode is per-project and off by default, so existing
testbenches keep the drive-point convention unchanged.

#### Holding the convention across a whole testbench

One driver is easy. A set of drivers that call each other has a second half to
the rule, and it is the half that is easy to lose. Name the instant just after
the falling edge the *drive point*. Then every task that drives a pin must be
entered at a drive point, must return at a drive point, and must not open with
a wait. A task that opens with a wait re-anchors itself and places its first
write one clock later than the SystemVerilog it replaces; a task that returns
part way through a cycle moves its caller's next write instead.

That rule is a convention, not a mechanism: nothing in the API states it and
nothing checks it. Ibex's icache testbench ported to cpptb
(`experiments/open_core_ports/ports/ibex_icache_cpptb`) states it in a comment
at the top of the file and holds every one of its driving tasks to it, and each
one had to be traced edge by edge against the `default output negedge` clocking
block it replaces. It was the largest single cost of writing those drivers.

`ReadOnly` and `ReadWrite` express the whole rule directly: sample after
`co_await ReadOnly{}`, drive after `co_await ReadWrite{}`. A phase is named
rather than implied by a clock, so a task that starts with a phase wait is not
re-anchoring itself. No supported `cpptb build` configuration supplies those
phases to their documented contract today, which is why this convention is
what the examples use. See
[Timing backend support](#timing-backend-support).

### Coming from cocotb

The trigger vocabulary is deliberately the same: `RisingEdge`, `FallingEdge`,
`ReadOnly`, `ReadWrite`, `NextTimeStep`, and drivers, monitors and scoreboards
compose the same way. One difference changes how a driver must be written, and
translating a testbench line for line will produce a driver that acts a cycle
early.

#### Writes are immediate, not deferred

In cocotb, assigning to a signal queues the write and applies it at the next
`ReadWrite` point. Writing straight after `await RisingEdge(clk)` therefore
cannot affect the edge just awaited; it behaves like a non-blocking assignment.

```python
# cocotb: safe. The write is queued, so this edge is already over for it.
async def driver(dut):
    while True:
        await RisingEdge(dut.clk)
        dut.wdata.value = next_word()      # lands for the *next* edge
        dut.wvalid.value = 1
```

`set()` here applies at once, and `co_await RisingEdge{}` resumes before the
design has evaluated that edge, so the same shape drives into the edge being
awaited:

```cpp
// cpptb: wrong. The value is in place in time for this edge.
Task<void> driver(Dut dut) {
    while (true) {
        co_await RisingEdge{dut.clk};
        dut.wdata.set(next_word());        // lands for *this* edge
        dut.wvalid.set(1);
    }
}
```

```cpp
// cpptb: right. Drive off the edge, half a cycle before the next one.
Task<void> driver(Dut dut) {
    while (true) {
        co_await FallingEdge{dut.clk};
        dut.wdata.set(next_word());
        dut.wvalid.set(1);
    }
}
```

The symptom when this is wrong is a register or memory read-back returning the
value just written rather than the previous one, which reads as a design bug
rather than a testbench one.

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

Sampling on the rising edge and driving on the falling one lets both live in
one loop without racing each other:

```cpp
Task<void> agent(Dut dut, RegisterModel& model) {
    while (true) {
        co_await RisingEdge{dut.clk};
        if (dut.access.get()) model.check(dut.addr.get(), dut.rdata.get());

        co_await FallingEdge{dut.clk};
        const auto next = stimulus.next();
        dut.addr.set(next.addr);
        dut.wdata.set(next.wdata);
        dut.access.set(next.valid);
    }
}
```

#### What to change when translating

Only the placement of reads and writes relative to the clock differs. The rest
of a cocotb testbench carries over unchanged.

| cocotb | here |
|---|---|
| `await RisingEdge(clk)` then assign | `co_await FallingEdge{clk}` then `set()` |
| `await RisingEdge(clk)`, `await ReadOnly()`, then read | `co_await RisingEdge{clk}` then `get()` |
| `await ReadWrite()` before driving | `co_await FallingEdge{clk}`, or `co_await ReadWrite{}` where the backend provides it |

#### Phase waits depend on the timing backend

cocotb always has `ReadOnly` and `ReadWrite`. Here they are provided by the
timing backend, and a default `cpptb build` has none, so a testbench that awaits
a phase compiles and then reports this at run time:

```
ReadWrite, ReadOnly, and NextTimeStep need a timing backend that dispatches
simulator phases. This build has none: a default `cpptb build` links
Verilator's own --binary main, which owns clocks and timers but dispatches no
phases.
```

Adding `--vpi` to `build.verilator_args` makes the phase waits run, but not to
their documented contract, and no `cpptb.toml` key names a backend. The
edge-phase convention above needs no backend support, which is why the examples
use it. See [Timing backend support](#timing-backend-support).

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
dut.write_clk.set(0);
dut.read_clk.set(0);
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
CPPTB_REGISTER_TEST(
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
