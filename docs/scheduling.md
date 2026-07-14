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

## Composition

Use an ordinary `co_await task()` when the next operation is sequential. The
composition primitives cover the cases where work must overlap:

- `Join{...}` owns a fixed group of child tasks and resumes after all finish.
- `First{...}` races two or more triggers and returns the winner's zero-based
  index.
- `with_timeout(...)` adds a checked deadline to one trigger or task.
- `spawn()` returns a `Process` handle for work whose lifetime is controlled
  dynamically.
- `Event` broadcasts state changes; `Channel<T>` transfers FIFO data between
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
    Channel<uint32_t> expected_words;
    Channel<uint32_t> observed_words;
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
                        Channel<uint32_t>& expected_words) {
    co_await reset_done;

    const uint32_t word = 0x1234;
    dut.in_data.set(word);
    dut.in_valid.set(1);
    expected_words.put_nowait(word);
    co_await RisingEdge{dut.clk};
    dut.in_valid.set(0);
}
```

Channels decouple production from checking. `get()` suspends only while the
FIFO is empty, allowing the driver and monitor to run at different rates:

```cpp
Task<void> scoreboard(TestContext& test,
                      Channel<uint32_t>& expected_words,
                      Channel<uint32_t>& observed_words) {
    for (uint32_t index = 0; index < kWordCount; ++index) {
        const uint32_t expected = co_await expected_words.get();
        const uint32_t actual = co_await observed_words.get();
        test.expect_eq("FIFO payload", actual, expected);
    }
}
```

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
