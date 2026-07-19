# SystemVerilog interfaces and bidirectional signals

`cpptb build` elaborates top-level SystemVerilog interfaces with Slang and
generates the same named shape in C++. No interface manifest, port list, or
binding file is required.

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

## Naming and direction rules

| SystemVerilog object | Generated C++ form | Operations |
|---|---|---|
| Top-level `input request` | `dut.request` | `set()`, `get()` |
| Top-level `output response` | `dut.response` | `get()` |
| Top-level `inout gpio` | `dut.gpio` | `drive()`, `high_z()`, `get()` |
| Modport input `bus.valid` | `dut.bus.valid` | `set()`, `get()` |
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

## Clocks inside interfaces

Interface clocks are ordinary named members. The C++ testbench owns input
clocks exactly as it does top-level input clocks:

```cpp
dut.links[0].clk.set(0);
dut.links[1].clk.set(0);
test.start_clock(dut.links[0].clk, 10_ns);
test.start_clock(dut.links[1].clk, 14_ns);
```

Clock discovery records the generated signal identity as well as its display
path. Two elements can therefore both be displayed as `links.clk` while
retaining independent periods, phases, values, and edge queues. A clock
produced by the DUT is sampled with `RisingEdge`, `FallingEdge`, or `Edge` and
is not passed to `start_clock()`.

Clock ownership is per element, not per interface-member array. It is valid to
start clocks on `links[0].clk` and `links[1].clk` while continuing to drive
`links[2].clk.set(...)` directly. The generated transport samples the
scheduler-owned elements and applies ordinary testbench writes only to the
unscheduled elements.

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

Neither operation advances time. The delays above are explicit requests for
dependent RTL to settle, not behavior hidden inside the inout API.

## Simulator capabilities

The Verilator backend validates interface elaboration, modport direction,
interface arrays, independent interface clocks, and two-state inout drive and
release intent. Verilator does not preserve arbitrary X/Z values as a
four-state simulator would. A `deposit_logic()` or `force_logic()` containing
X or Z therefore aborts with the operation, hierarchy path, simulator name,
and a recommendation to use a four-state backend rather than silently
coercing the value.

End-to-end four-state propagation, resolved contention, and the complete
conformance suite on another standards-compliant simulator remain portability
work. Known 0/1 `LogicBits` values continue to work on Verilator.

See the runnable [interfaces example](examples/interfaces.md) and the
[hierarchy guide](hierarchy.md) for adjacent internal and array access.
