# Coming from cocotb

cpptb reuses cocotb's names where the behavior matches, so most of a cocotb
testbench translates line for line. This page is the honest map: what carries
over unchanged, what needs one project setting, and the three translation
traps that were each found the measured way -- converting a real Ibex
testbench and counting its failures.

## The trigger vocabulary

| cocotb | cpptb | Notes |
|---|---|---|
| `await RisingEdge(dut.clk)` | `co_await RisingEdge{dut.clk}` | [resume point differs](#1-risingedge-resumes-before-the-edge-evaluates) |
| `await FallingEdge(dut.clk)` | `co_await FallingEdge{dut.clk}` | Same |
| `await Timer(5, "ns")` | `co_await Delay{5_ns}` | Same |
| `await ClockCycles(dut.clk, n)` | `co_await clock_cycles(dut.clk, n)` | Same |
| `await ReadOnly()` | `co_await ReadOnly{}` | Same |
| `await ReadWrite()` | `co_await ReadWrite{}` | Same |
| `await NextTimeStep()` | `co_await NextTimeStep{}` | Same |
| `cocotb.start_soon(coro())` | `test.spawn(task(...))` | Same model |
| `await First(a, b)` | `co_await First{a, b}` | Same |
| `Event`, `Queue`, `Lock` | `Event`, `Queue`, `Lock` | Same |

The phase waits come from the timing backend, which every cpptb project
selects -- see [The write model](#the-write-model) below for the two lines
of `cpptb.toml` that every example sets.

If you also maintain SystemVerilog benches, the same concepts line up three
ways:

| Concept | cocotb | cpptb | Pure-SV twin |
|---|---|---|---|
| Timed wait | `await Timer(1, unit="ns")` | `co_await Delay{1_ns}` | `#1ns` |
| Signal edge | `await RisingEdge(dut.clk)` | `co_await RisingEdge{dut.clk}` | `@(posedge clk)` |
| Concurrent work | `start_soon()` / task groups | `spawn()` or `Join{...}` | `fork ... join` |
| FIFO communication | `Queue` | `Queue<T>` | `mailbox` |
| Notification | `Event` | `Event` | `event` |
| Deadline | `with_timeout()` | `with_timeout()` | explicit event/deadline race |

## The write model

cocotb's `dut.sig.value = x` is a cached write, applied at the next ReadWrite
region. cpptb's write model is the same one, and it is the default — a new
project gets it with no configuration. The examples state it explicitly:

```toml
[build]
timing_backend = "verilator-direct"   # or "vpi"
deferred_writes = true
```

`set()` therefore carries cocotb's semantics exactly: the write queues
and flushes at the ReadWrite settle point, a `get()` between the two returns
the simulator's value (your own queued write is invisible to you, as in
cocotb), and `set_now()` is the escape hatch, mirroring
`setimmediatevalue()`. The contract is pinned by
`tests/integration/deferred_writes` on both backends in every `make test`.

A build without `deferred_writes` falls back to an immediate deposit. That is
legacy behavior, intended for deprecation, and not an authoring style cpptb
documents.

Both supported backends are interchangeable: with the model enabled, the Ibex icache
testbench produces byte-identical per-test check counts on
`verilator-direct` and `vpi` across all ten of its tests.

## The three traps

Each of these cost real debugging time converting the Ibex icache testbench
(`experiments/open_core_ports/ports/ibex_icache_cpptb`); the failure counts
below are what they produced.

### 1. `RisingEdge` resumes *before* the edge evaluates

cocotb's `RisingEdge` fires from a value-change callback: your coroutine runs
*after* the design has evaluated the edge, so reads see post-edge state. In
cpptb, `co_await RisingEdge{}` resumes in the Active region, *before* the
design evaluates -- reads see the values `always_ff` is about to sample.
That is the right instant for a monitor and the wrong one for a driver that
reads protocol pins.

The cocotb-equivalent anchor for a driver is the edge *and then the settle
point*:

```cpp
co_await RisingEdge{dut.clk};
co_await ReadWrite{};   // post-eval: what cocotb's RisingEdge delivers
```

A driver anchored on the bare edge read the *previous* cycle's requests and
failed every scoreboard comparison downstream.

### 2. Advancing to the next anchor and settling to this cycle's anchor are different operations

A driver mid-cycle (just after an edge wait) that needs the drive anchor of
the *same* cycle must await only `ReadWrite{}`. Using the full
edge-plus-settle advance there consumes an extra edge: in the icache
conversion it held `branch_i` asserted for two edges instead of one, and 52
scoreboard failures with a branch-shaped signature paid for the distinction.
Keep two helpers and name them honestly:

```cpp
Task<void> drive_point(Dut dut) {      // anchor to anchor: one full cycle
    co_await RisingEdge{dut.clk};
    co_await ReadWrite{};
}
Task<void> settle_to_drive(Dut dut) {  // mid-cycle to this cycle's anchor
    co_await ReadWrite{};
}
```

### 3. Classify anchor sites by their predecessor *await*, never by the preceding line

The subtlest of the three. A wait-for-condition loop exits at an edge:

```cpp
while (true) {
    co_await RisingEdge{dut.clk};
    if (dut.valid_o.get() != 0) break;
}
co_await /* settle, NOT advance */;
dut.ready_i.set(0);
```

Textually, the line before the anchor is the loop's closing brace; in control
flow, the predecessor is the `RisingEdge` inside the loop, so the site is a
*settle*. A conversion that classified it by adjacent text used the full
advance, held `ready_i` one extra edge, and the DUT handed over one beat the
driver never counted -- which surfaced 30,000 checks later as an
unexplainable fetch address, only after errored fetches. Fifty-two failures
out of fifty thousand, all one signature, from one line.

## The traps in one worked example

A *reactive* agent — a grant driver sampling a request line, a ready/valid
responder — is where trap 1 bites in practice, because it reads DUT outputs
at the edge before deciding what to drive:

```python
# cocotb: reads req AFTER the edge evaluated, then queues gnt for the next edge
async def grant_driver(dut):
    while True:
        await RisingEdge(dut.clk)
        if dut.req.value:
            dut.gnt.value = 1
```

```cpp
// cpptb: RisingEdge alone would read the PREVIOUS cycle's req (trap 1).
Task<void> grant_driver(Dut dut) {
    while (true) {
        co_await drive_point(dut);       // RisingEdge + ReadWrite, trap 2's helper
        if (dut.req.get() != 0) {
            dut.gnt.set(1);              // queues; lands for the next edge
        }
    }
}
```

If your driver never reads DUT outputs, the plain `RisingEdge` + `set()`
shape is already correct and the extra settle buys nothing. For a full
conversion at scale, both Ibex ports
(`experiments/open_core_ports/ports/ibex_icache_cpptb` and
`.../core_ibex_cpptb`) carry the helper pair with rationale comments, and
every one of their drive sites is classified per trap 3.

## What has no cocotb equivalent, and the reverse

- `co_await RisingEdge{}` resumes *before* the design evaluates the edge,
  which cocotb has no counterpart for -- cocotb's fires from a value-change
  callback, after evaluation. That is trap 1 above, and it is the one genuine
  semantic difference left once the standard configuration is in place.
- `force()`/`release()` are immediate in both worlds; the deferred mode does
  not touch them.
- Replay-style pin comparison against recordings is drive-point-sensitive;
  recordings made under one anchor convention do not replay under the other.

## Next

Pick a runnable [example](examples.md) shaped like the bench you are
converting, or go to [Tasks and concurrency](testbench-authoring.md) for the
coordination primitives the trigger table only names. The precise resume
semantics behind trap 1 live in [Scheduling](scheduling.md).
