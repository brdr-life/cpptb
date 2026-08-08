# Core ideas

[Getting started](getting-started.md) got a test running. This page explains
the model underneath it, so the rest of the documentation reads as variations
on a few ideas rather than a list of features. It should take about ten
minutes and assumes nothing beyond the counter you already ran.

## A test is a registered coroutine

There is no test class to inherit and no framework object to construct. A test
is a `Task<void>` coroutine that receives the generated DUT and a
`TestContext`, and one macro that registers it:

```cpp
Task<void> register_sequence(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
    test.start_clock(dut.clk, 10_ns);

    co_await reset_dut(dut);                 // a helper you wrote

    ApbMaster apb{ApbBus{/* the DUT's APB pins */}};

    const auto write = co_await apb.write(0x04, 0x1234'5678);
    test.require_eq("register write", write.status, MemoryStatus::Okay);

    const auto read = co_await apb.read(0x04);
    test.expect_eq("register readback", read.data, 0x1234'5678u);
}

CPPTB_REGISTER_TEST(register_sequence);
```

`CPPTB_REGISTER_TEST` installs the test through a translation-unit
initializer. Register as many as you like in the same binary; each simulator
invocation selects and runs exactly one of them, in fresh simulation state, so
no test can inherit another's DUT.

Because registration happens in an initializer, link the testbench translation
unit directly into the simulator executable. If it is packaged in a static
archive, link that archive with whole-archive semantics so the linker does not
discard an otherwise unreferenced initializer.

## Signal access is explicit, and never moves time

`get()` and `set()` are ordinary function calls. Neither one advances the
simulation:

```cpp
dut.enable.set(1);                       // no time passes
const auto count = dut.count.get();      // no time passes
```

Time advances at exactly one kind of place — a `co_await` on a scheduling
primitive:

```cpp
co_await RisingEdge{dut.clk};            // time passes here
co_await Delay{10_ns};                   // and here
co_await clock_cycles(dut.clk, 8);       // and here
```

This is the single most useful property of a cpptb testbench: you can find
every point where simulation time moves by searching for `co_await`. Nothing
is hidden in a driver or a helper. `start_clock()` looks like an exception but
is not — it registers a period with the simulator and returns immediately,
without suspending.

`set()` takes effect at the *next* settle point, not the instant you call it —
cocotb's write model, selected by `deferred_writes = true` in `cpptb.toml`. So
a write made right after an awaited edge is seen by the following edge, not the
one you just waited for. `set_now()` is the escape hatch when you really do
need an immediate deposit, as when initializing a clock pin before
`start_clock()`. See [The write model](scheduling.md#the-write-model).

Which values you observe when a coroutine resumes is a genuinely subtle topic,
and the one most likely to bite you. [Scheduling](scheduling.md) is the
precise answer; if you are converting a cocotb bench, read
[Coming from cocotb](coming-from-cocotb.md) first, because the resume point
differs.

## Checks are values, not exceptions

Two flavors, differing only in what happens after a failure:

```cpp
test.expect_eq("register readback", read.data, 0x1234'5678u);  // records, continues
test.require_eq("register write", write.status, MemoryStatus::Okay);  // records, stops
```

Use `expect` for a comparison whose failure the rest of the test can survive —
you will see every mismatch in one run instead of only the first. Use
`require` when continuing would be meaningless or misleading. Both record the
label, the source location, and both values into the structured result, which
is what a CI system consumes. [Framework test lifecycle](test-lifecycle.md)
covers the full model.

## Concurrency is explicit too

Real benches need a driver, a monitor, and a scoreboard running at once.
`spawn()` starts a coroutine as a concurrent process owned by the test:

```cpp
auto monitor = test.spawn(monitor_bus(dut));
auto driver  = test.spawn(drive_packets(dut));

co_await driver;     // wait for one
monitor.cancel();    // stop another
```

Test-owned means you do not have to clean up: when the test ends, for any
reason, its remaining processes are cancelled for you, and a failure inside a
spawned process is attributed to the right place.

## The primitive set is deliberately small

Nearly every cpptb testbench is built from these:

- `test.start_clock(dut.clk, 10_ns)` — register a periodic input clock, before
  the first `co_await`.
- `test.spawn(task)` — run a coroutine concurrently and get a handle back.
- `co_await RisingEdge{dut.clk}`, `FallingEdge`, `Edge` — simulator triggers.
- `co_await Delay{10_ns}`, `co_await clock_cycles(clk, n)` — advance time.
- `co_await ReadOnly{}`, `ReadWrite{}`, `NextTimeStep{}` — settle to a specific
  point in the current or next timestep, as the counter example does before it
  samples.
- `dut.signal.set(v)` / `dut.signal.get()` — explicit signal access.
- `dut.block.sub.name.get()`, `deposit()`, `force()`, `release()` — anything in
  the inferred RTL hierarchy, not just ports.
- `test.expect_eq(...)` / `test.require_eq(...)` — checks.
- `co_await helper_task(...)` — your own reusable bus operations.

Everything else in this documentation builds on that list.
[Tasks and concurrency](testbench-authoring.md) adds `Join`, `First`,
timeouts, events, queues, locks, and semaphores.

## What you can add when you need it

None of this is required to write a working test, and none of it changes the
model above:

| When you need | Reach for |
|---|---|
| Random stimulus that replays from a seed | [Randomization](random-stimulus.md) |
| To record which cases a test actually hit | [Functional coverage](randomization/functional-coverage.md) |
| Drivers, monitors, and scoreboards you don't write yourself | [Verification components](verification-components.md) |
| Typed handles for a register map | [Register abstraction layer](memory-register-models.md) |
| To read or force signals inside the DUT | [Hierarchical DUT access](hierarchy.md) |
| Readable diagnostics from concurrent processes | [Structured logging](logging.md) |

## Next

Read [Coming from cocotb](coming-from-cocotb.md) if you have written cocotb
benches — it is the fastest route from what you already know, and it names the
three traps that cost real debugging time. Otherwise, browse the
[examples](examples.md) for a DUT shaped like yours, or go straight to
[Tasks and concurrency](testbench-authoring.md).
