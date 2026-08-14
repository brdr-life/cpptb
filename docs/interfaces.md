# Interfaces and inouts

A DUT that presents SystemVerilog interfaces instead of flat ports needs no
special handling: the generated `Dut` mirrors the interface's own shape, so
`bus.valid` in RTL is `dut.bus.valid` in C++. Parameterized interfaces,
modports, interface arrays, and bidirectional pins all carry across, and there
is no manifest, port list, or binding file to maintain.

```systemverilog
interface stream_if #(parameter int WIDTH = 8) (input logic clk);
  logic valid;
  logic ready;
  logic [WIDTH-1:0] data;
  wire sideband;
  modport target(input clk, valid, data, output ready, inout sideband);
endinterface

module design(stream_if.target links [2]);
  // ...
endmodule
```

The generated API mirrors those names:

```cpp
dut.links[0].valid.set(1);
dut.links[0].data.set(0x24);
const auto ready = dut.links[0].ready.get();

dut.links[0].sideband.drive(1);
dut.links[0].sideband.high_z();
```

One timing note up front: `set()` on a port or modport input **queues** and
flushes at the timestep's ReadWrite point, like every port drive under
[the write model](scheduling.md#the-write-model). The inout operations
`drive()` and `high_z()` are **immediate** — they express drive intent, not a
value queued for the next edge. The
[timing summary](library/signals.md#timing-summary) lists every operation.

## Naming and direction rules

| SystemVerilog object | Generated C++ form | Operations |
|---|---|---|
| Top-level `input request` | `dut.request` | `set()`, `set_now()`, `get()` |
| Top-level `output response` | `dut.response` | `get()` |
| Top-level `inout gpio` | `dut.gpio` | `drive()`, `high_z()`, `get()` |
| Modport input `bus.valid` | `dut.bus.valid` | `set()`, `set_now()`, `get()` |
| Modport output `bus.ready` | `dut.bus.ready` | `get()` |
| Modport inout `bus.pin` | `dut.bus.pin` | `drive()`, `high_z()`, `get()` |
| Interface array member | `dut.links[index].member` | Follows the modport direction |
| Internal hierarchy | `dut.block1.block2.signal` | Backdoor operations |

There are no generated `inputs`, `outputs`, or `internal` grouping objects.
Names remain the primary interface, and every fixed unpacked dimension uses
ordinary `[]` syntax.

An interface port must select a modport. Without one, the C++ side cannot
infer which members the testbench drives and which it samples, so generation
fails with an error naming the interface port and asking for an explicit
modport. Integral interface parameter values and constructor connections are
inferred from the elaborated design.

## Multidimensional interface and member arrays

Each unpacked SystemVerilog dimension becomes one C++ `[]`, in source order.
This applies independently to dimensions on the interface port and dimensions
on a member inside the interface:

```systemverilog
interface grid_if(input logic clk);
  logic [3:0] payload  [1:0];
  logic [3:0] observed [1:0];
  modport target(input clk, payload, output observed);
endinterface

module design(grid_if.target grids [1:0][2:4]);
  // ...
endmodule
```

The first two indices select an interface instance. The final index selects a
member-array element:

```cpp
dut.grids[1][3].clk.set_now(0);
dut.grids[1][3].payload[0].set(0xa);
dut.grids[1][3].payload[1].set(0x5);

const auto first = dut.grids[1][3].observed[0].get();
const auto second = dut.grids[0][4].observed[1].get();
```

Declared bounds are preserved. In this example, the first interface index is
`0..1`, the second is `2..4`, and the member index is `0..1`, even though the
source declarations mix descending and ascending ranges. An out-of-range
selection fails with the declared bounds. Direction still comes exclusively
from the modport: `payload[word]` has `set()` and `get()`, while
`observed[word]` has only `get()`.

The same rule scales to more dimensions without introducing index-number APIs
or generated `inputs` and `outputs` containers. The expression remains the
elaborated HDL path written with ordinary C++ indexing.

## Clocks inside interfaces

Interface clocks are ordinary named members. The C++ testbench owns input
clocks exactly as it does top-level input clocks:

```cpp
dut.links[0].clk.set_now(0);
dut.links[1].clk.set_now(0);
test.start_clock(dut.links[0].clk, 10_ns);
test.start_clock(dut.links[1].clk, 14_ns);
```

Initialization uses `set_now()` — the immediate write — because the pin must
hold its level before the clock exists, exactly as with a top-level clock;
see [Clocking](clocking.md).

Clock discovery records the generated signal identity as well as its display
path. Two elements can therefore both be displayed as `links.clk` while
retaining independent periods, phases, values, and edge queues. A clock
produced by the DUT is sampled with `RisingEdge`, `FallingEdge`, or `Edge` and
is not passed to `start_clock()`.

Clock ownership is per element, not per interface-member array. In a wider
array — say `links [4]` — it is valid to start clocks on `links[0].clk` and
`links[1].clk` while continuing to drive `links[2].clk` and `links[3].clk`
with ordinary writes. The generated transport samples the scheduler-owned
elements and applies ordinary testbench writes only to the unscheduled
elements.

## Inout drive intent

`drive(value)` enables the testbench driver and supplies a known packed value.
`high_z()` disables that driver so the DUT or another HDL driver can determine
the observed value. `get()` samples the resulting port or interface member.

```cpp
dut.gpio.drive(0x5);
co_await Delay{1_ns};
test.expect_eq("testbench drive", dut.gpio.get(), 0x5u);

dut.gpio.high_z();
co_await Delay{1_ns};
test.expect_eq("released bus", dut.gpio.get(), expected_from_dut);
```

Both operations are immediate — unlike `set()`, nothing queues — and neither
advances time. The delays above are explicit requests for dependent RTL to
settle, not behavior hidden inside the inout API.

## Simulator capabilities

The Verilator backend validates interface elaboration, modport direction,
interface arrays, independent interface clocks, and two-state inout drive and
release intent. Verilator does not preserve arbitrary X/Z values as a
four-state simulator would. A `deposit_logic()` or `force_logic()` containing
X or Z therefore aborts with the operation, hierarchy path, simulator name,
and a recommendation to use a four-state backend rather than silently
coercing the value.

Verilator's experimental `--fourstate` option does not currently preserve the
required semantics. CPPTB therefore rejects raw use of that option and offers
an explicit, fail-closed capability probe instead. End-to-end four-state
propagation, resolved contention, and the complete conformance suite on another
standards-compliant simulator remain portability work. Known 0/1 `LogicBits`
values continue to work on Verilator.

See the runnable [interfaces example](examples/interfaces.md) and the
[hierarchy guide](hierarchy.md) for adjacent internal and array access. The
[four-state guide](four-state.md) documents `LogicBits`, logic signal APIs, the
experimental gate, and its enablement criteria.

## Related APIs

- [Signals reference](library/signals.md) — signatures for `set()`,
  `set_now()`, `drive()`, `high_z()`, and the timing summary.
- [Clocking](clocking.md) — interface-member clocks follow the same
  ownership and initialization rules as top-level clocks.
- [API reference](refcard.md) — the signal operations at a glance.
