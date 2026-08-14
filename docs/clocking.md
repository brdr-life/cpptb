# Clocking

The testbench owns clock timing. You declare a period once in C++ and the
simulator drives the waveform from then on — there is nothing to configure at
generation time, and no process of yours toggling the pin every half period.

This page covers registering input clocks, the reset sequence that usually
travels with them, running several domains at once, and waiting on clocks the
DUT produces.

## Input clocks

Initialize each DUT input clock and register its full period before the first
`co_await`:

```cpp
Task<void> test_sequence(Dut dut, TestContext& test) {
    dut.core_clk.set_now(0);
    dut.bus_clk.set_now(0);

    test.start_clock(dut.core_clk, 4_ns);
    test.start_clock(dut.bus_clk, 10_ns, 1_ns);

    co_await clock_cycles(dut.core_clk, 2);
    co_await RisingEdge{dut.bus_clk};
}
```

The optional third argument is a phase: the first rising edge lands at
`phase + period / 2`. Each clock has an independent period and phase, the
period must be even and — like the phase — a whole multiple of the simulator
precision, and the generated waveform is a fixed 50% duty cycle. Call
`start_clock()` once per clock. The first registered clock is the primary
clock used for the result's cycle count; it does not restrict which clock a
coroutine may await. The signature is in the
[TestContext reference](library/test-context.md).

`start_clock()` registers the clock with the runtime during test
initialization; the generated SystemVerilog wrapper carries a driver task
for every writable one-bit signal (including unpacked-array elements, which
is how interface-member clocks arrive), asks the runtime after
initialization which were registered, and those toggle in simulator time.
C++ does not cross DPI merely to write each clock level, and nothing about
the clocks is decided at build time. `set_now()` initializes the pin
because it is cocotb's `setimmediatevalue()` -- correct under either write
model, before the clock exists.

Registration is final: there is no supported way today to stop, pause,
restart, or change the period or duty cycle of a scheduler-owned clock.
Coherent runtime clock control is
[roadmap milestone 8](roadmap.md#8-coherent-clock-and-reset-control); until
it lands, a gated or reconfigurable clock is modeled in RTL and observed as
a DUT-produced clock (below).

## Reset

A clock and a reset usually travel together, and the standard
[write model](scheduling.md#the-write-model) makes the ordinary shape
correct: initialize the clock pin with `set_now()`, start the clock, assert
reset with a plain `set()` — the queued write flushes in the first timestep,
well before the first rising edge at half a period — hold it for a couple of
cycles, and release it:

```cpp
dut.clk.set_now(0);
test.start_clock(dut.clk, 10_ns);

dut.rst_n.set(0);
co_await clock_cycles(dut.clk, 2);
dut.rst_n.set(1);
```

One caution for edge-sensitive resets: a design that resets on the reset
transition itself (`negedge rst_ni`) must actually receive that edge. In a
two-state simulator a pin held at `0` from time zero never transitions, the
async reset never fires, and the design starts in an unreset state — so
drive the pin to its inactive level first, or assert and then release, rather
than holding it asserted from the start of time.

## DUT-produced clocks

Do not call `start_clock()` for a divided, gated, recovered, or otherwise
DUT-produced output clock. Await the generated signal directly:

```cpp
co_await RisingEdge{dut.output_clk};
co_await FallingEdge{dut.output_clk};
```

One-bit DUT outputs use interest-gated observers. Their transitions call DPI
only while C++ has a matching edge wait.

## Delays and clock cycles

`clock_cycles(clock, count)` waits for rising edges of the selected signal.
`Delay{10_ns}` waits for absolute simulator time and does not depend on any
clock. Neither signal writes nor backdoor operations insert an implicit
delay.

Clock generation and scheduler notification are separate concerns: the
wrapper calls the C++ scheduler on every rising edge of a registered clock,
and on falling edges only while a falling- or either-edge wait is active.
[Scheduling](scheduling.md) covers the dispatch machinery and trigger
composition.

## Related pages

- [Multiple-clock example](examples/multiclock.md) — a complete two-domain
  testbench.
- [TestContext reference](library/test-context.md) — `start_clock`'s
  signature and preconditions.
- [API reference](refcard.md) — the clock operations at a glance.
- [Roadmap milestone 8](roadmap.md#8-coherent-clock-and-reset-control) —
  planned runtime clock control and reusable reset components.
