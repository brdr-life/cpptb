# Fault injection

This example accesses three objects through their natural elaborated paths: a
resolved net, a clocked variable, and a four-element memory. Slang infers all
three from the RTL; there is no probe list or user-authored hierarchy file.

```sh
make cpp-dpi-fault-injection-run
make cpp-dpi-fault-injection-sv-run
```

## Framework shape

The hand-written file imports the generated `Dut`, defines one coroutine, and
registers it. Clock generation, reset stimulus, hierarchy operations, and
checks are all visible in that user-facing sequence; generated DPI scheduling
and transport stay under the target's ignored build directory.

```cpp
#include "cpptb/cpptb.hpp"
#include "dut.hpp"

using cpptb::Dut;
using coro::Delay;
using coro::FallingEdge;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

Task<void> fault_injection_sequence(Dut dut, TestContext& test) {
    dut.clk.set(0);
    test.start_clock(dut.clk, 10_ns);

    dut.rst_n.set(0);
    dut.source_i.set(0);
    co_await clock_cycles(dut.clk, 2);
    co_await FallingEdge{dut.clk};
    dut.rst_n.set(1);

    // The sections below contain the hierarchy stimulus and checks.
}

CPPTB_REGISTER_TEST(fault_injection_sequence);
```

The complete sequence is kept in the repository as
`examples/fault_injection/testbench.cpp`. The snippets below are copied from
its hierarchy-focused portions.

## Force a resolved net

`force()` and `release()` act immediately. The explicit delay is where the
test asks downstream combinational logic to settle:

```cpp
dut.source_i.set(0x12);
co_await Delay{1_ps};
test.expect_eq("resolved net baseline", dut.resolved_o.get(), 0x48u);

dut.resolved_value.force(0xa5);
test.expect_eq("force is immediately readable",
               dut.resolved_value.get(), 0xa5u);
co_await Delay{1_ps};
test.expect_eq("forced net reaches output", dut.resolved_o.get(), 0xa5u);

dut.source_i.set(0x34);
co_await Delay{1_ps};
test.expect_eq("force overrides changing driver", dut.resolved_o.get(),
               0xa5u);

dut.resolved_value.release();
co_await Delay{1_ps};
test.expect_eq("release restores resolved driver", dut.resolved_o.get(),
               0x6eu);
```

The matching pure-SV operations are `force i_dut.resolved_value = 8'ha5;`
and `release i_dut.resolved_value;`.

## Clocked state and memory

A force on a variable remains effective while the RTL attempts to write it:

```cpp
dut.counter.force(0x55);
co_await clock_cycles(dut.clk, 2);
co_await Delay{1_ps};
test.expect_eq("RTL writes do not override force", dut.counter_o.get(),
               0x55u);

dut.counter.release();
co_await RisingEdge{dut.clk};
co_await Delay{1_ps};
test.expect_eq("RTL writes resume after release", dut.counter_o.get(),
               0x56u);
```

Memory elements use the same primitive interface. A deposit is a one-time
blocking assignment; a force continues to override other writers:

```cpp
dut.memory[2].deposit(0xbeef);
test.expect_eq("deposit is immediately readable",
               dut.memory[2].get(), 0xbeefu);

dut.memory[2].force(0xcafe);
co_await Delay{1_ps};
test.expect_eq("memory force reaches output", dut.memory_read_data.get(),
               0xcafeu);
dut.memory[2].release();
```

The complete C++ and pure-SV benches perform the same 13 checks over the same
7 rising clock edges.

## Clock force limitation

Force is available on inferred hierarchical objects; ordinary port signals do
not have `.force()`. Do not force a
registered input clock as a workaround: `start_clock()` installs a generated
simulator-side driver and scheduler edge source, so overriding only the RTL
value would make observed scheduler edges disagree with the DUT clock.

Input clocks should be driven with `start_clock()`. DUT-produced output clocks
are observed as ordinary signals. A coherent clock pause/override API would
need to control both the simulator driver and scheduler registration together.

Attempting `dut.clk.force(0)` or `dut.clk.release()` fails at compile time with
an explicit diagnostic explaining that ordinary DUT ports cannot be forced,
that scheduler-owned clocks must remain under `TestContext::start_clock()`,
and that coherent clock pause/override is not yet supported. The same
diagnostic directs non-clock use cases to inferred hierarchy paths.
