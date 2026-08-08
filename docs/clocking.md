# Clocking

The testbench owns clock timing. You declare a period once in C++ and the
simulator drives the waveform from then on — there is nothing to configure at
generation time, and no process of yours toggling the pin every half period.

This page covers registering input clocks, running several domains at once,
waiting on clocks the DUT produces, and how the scheduler dispatches edges.

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

The optional third argument is a phase applied before the usual first
half-period. Each clock has an independent period and phase. The first
registered clock is the primary clock used for the result's cycle count; it
does not restrict which clock a coroutine may await.

`start_clock()` registers the clock with the runtime during test
initialization; the generated SystemVerilog wrapper carries a driver task
for every writable one-bit signal (including unpacked-array elements, which
is how interface-member clocks arrive), asks the runtime after
initialization which were registered, and those toggle in simulator time.
C++ does not cross DPI merely to write each clock level, and nothing about
the clocks is decided at build time. `set_now()` initializes the pin
because it is cocotb's `setimmediatevalue()` -- correct under either write
model, before the clock exists.

## Scheduler callbacks

Clock generation and scheduler notification are separate. The wrapper calls
the C++ scheduler on every rising edge of a registered clock. Falling edges
call the scheduler only while a falling- or either-edge wait is active. This
static-clock fast path avoids publishing a new interest mask each time a
coroutine arms its next clock wait.

The callback resumes tasks waiting on that edge and updates scheduler time. It
does not generate the clock waveform. Most cycle-oriented testbenches await
nearly every rising edge, so these callbacks normally perform useful work.

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
clock. Neither signal writes nor backdoor operations insert an implicit delay.

See [scheduling](scheduling.md) for trigger ordering and composition, and the
[multiple-clock example](examples/multiclock.md) for a complete testbench.
