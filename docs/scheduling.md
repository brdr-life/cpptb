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

| Timing backend | Status | Timing contract | Portability |
|---|---|---|---|
| Direct Verilator dispatch | Supported default | Complete | Verilator-specific |
| Standard VPI callbacks | Supported fallback | Complete | Standard simulator API |
| Generated SV-DPI calendar | Experimental | Complete for generated and observed events | Cross-simulator validation pending |

The generated calendar owns framework clocks and timers and observes selected
external signals, but standard DPI cannot query the simulator's complete event
queue. Its `NextTimeStep` therefore cannot yet promise to wake for an arbitrary
unobserved internal DUT event. Direct Verilator dispatch and standard VPI are
the supported contract-complete choices. See [Performance](performance.md) for
the exact backend comparison.

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
and needs an explicit handle. Awaiting the handle waits for normal completion
or completed cancellation:

```cpp
auto monitor = test.spawn(dormant_monitor(dut));

co_await Delay{3_ns};
monitor.cancel();
co_await monitor;

test.expect_eq("monitor was cancelled", monitor.cancelled(), true);
```

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
