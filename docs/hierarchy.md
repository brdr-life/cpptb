# Hierarchical DUT access

Verification often needs to reach past the ports: to check an internal counter,
preload a memory, or force a net to inject a fault. In cpptb you reach these
objects the same way you reach a port — by naming the path the RTL already
uses:

```cpp
const auto status = dut.block1.block2.status.get();
dut.block1.block2.control.deposit(0x12);
dut.lanes[1].state.force(3);
dut.lanes[1].state.release();
```

The C++ path mirrors the elaborated SystemVerilog instance, generate-block,
and object path. A misspelled path, invalid array index, or unsupported
operation is a C++ compile error.

## Runnable examples

Start with the [fault-injection example](examples/fault-injection.md). It is a
complete framework testbench that starts a clock, resets the DUT, reads and
forces an internal resolved net, forces clocked state, deposits and forces a
memory element, and checks the resulting top-level outputs. Its C++ and pure
SystemVerilog forms execute the same 13 checks over the same 7 clock edges:

```sh
make cpp-dpi-fault-injection-run
make cpp-dpi-fault-injection-sv-run
```

The central sequence uses the generated `Dut` directly and is registered like
any other cpptb coroutine:

```cpp
Task<void> fault_injection_sequence(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
    test.start_clock(dut.clk, 10_ns);

    dut.rst_n.set(0);
    co_await clock_cycles(dut.clk, 2);
    dut.rst_n.set(1);

    dut.resolved_value.force(0xa5);
    test.expect_eq("force is immediately readable",
                   dut.resolved_value.get(), 0xa5u);

    co_await ReadOnly{};
    test.expect_eq("forced net reaches output", dut.resolved_o.get(), 0xa5u);
    co_await NextTimeStep{};
    dut.resolved_value.release();
}

CPPTB_REGISTER_TEST(fault_injection_sequence);
```

For adjacent typed access patterns, see [rich data](examples/rich-data.md) for
wide packed values, fixed point, multidimensional arrays, packed structs, and
enums. [Fault injection](examples/fault-injection.md) is the smallest complete
bench built around hierarchy access, and the [examples index](examples.md)
routes to every complete C++/pure-SV pair in the standard regression.

## Operations

Top-level input ports use `set()` because they are normal testbench drives.
Like every port drive under [the write model](scheduling.md#the-write-model),
`set()` **queues** and flushes at the timestep's ReadWrite point:

```cpp
dut.request.set(1);
const auto response = dut.response.get();
```

Objects below the DUT hierarchy use explicit SystemVerilog backdoor
operations:

```cpp
const auto before = dut.core.pending.get();
dut.core.pending.deposit(0x2a);
dut.core.pending.force(0x3f);
dut.core.pending.release();
```

- `get()` reads the current value immediately.
- `deposit(value)` performs one blocking assignment immediately.
- `force(value)` overrides normal HDL drivers immediately and remains active.
- `release()` removes that force immediately.

Unlike a port `set()`, none of these queues: hierarchy operations apply the
instant they are called, which is what makes them backdoors. `deposit()` is
available on storage the simulator can assign directly; a resolved net
accepts `force()` and `release()` but not `deposit()`. The
[timing summary](library/signals.md#timing-summary) puts the port and
hierarchy operations side by side.

None of these operations advances simulation time or adds an evaluation
phase. An immediate `get()` of the same object sees a deposit or force. When
dependent RTL must execute before a check, settle first — `co_await
ReadOnly{}` in a clocked bench, or an explicit `Delay` where nothing else
creates timesteps:

```cpp
dut.core.pending.deposit(0x2a);
test.expect_eq("immediate backdoor read", dut.core.pending.get(), 0x2au);

co_await ReadOnly{};
test.expect_eq("dependent output", dut.pending_o.get(), 0x2au);
```

After `release()`, a variable retains its last forced value until RTL writes it
again. A net returns to its resolved drivers. These are the corresponding
SystemVerilog semantics.

## Arrays and generated scopes

Fixed unpacked memories preserve their declared index range:

```cpp
dut.memory[5].deposit(0xbeef);
const auto value = dut.memory[5].get();
```

Multidimensional arrays take one index per dimension:

```cpp
dut.coefficients[1][3].deposit(7);
```

Elaborated instance and generate arrays use the same syntax and accept either
a literal or runtime index:

```cpp
const auto lane_state = dut.lanes[2].state.get();
const auto selected = dut.lanes[index].state.get();
const auto width = dut.lanes[index].block.WIDTH;
```

Homogeneous array elements return one generated typed view. An out-of-range
runtime index reports the complete scope-array path and valid indices.
Elaborated parameters remain read-only values on that selected view. Because a
runtime index may select any element, using an operation on an array field
generates that operation for the corresponding field in every homogeneous
element. Unused fields and unused arrays still emit no DPI transport.

## Packed and typed values

Signals up to 64 bits use native integer values. Wider packed values use
`Bits<W>`:

```cpp
Bits<137> command;
command.set_word(0, 0x1234'5678u);
command.set_word(4, 0x1ffu);
dut.core.command.deposit(command);
const Bits<137> observed = dut.core.command.get();
```

Generated packed enum and struct value types preserve names and fields. The
same signal API accepts and returns those generated types. `get_as<T>()` and
`deposit_as(value)` provide the explicit conversion point for fixed-point or
other user value types that expose the required bit conversion.

For four-state objects, `get_logic()`, `deposit_logic()`, and `force_logic()`
use `LogicBits<W>` with separate value and X/Z planes:

```cpp
const auto stimulus = LogicBits<4>::from_string("10xz");
dut.core.bus.deposit_logic(stimulus);
const auto sampled = dut.core.bus.get_logic();
```

The ordinary `get()`, `deposit()`, and `force()` operations remain available
as two-state operations. Four-state behavior depends on simulator support;
Verilator, the current end-to-end reference backend, does not yet preserve X/Z
semantics. Known `LogicBits` values work, while unknown writes fail before
transport. See [four-state values](four-state.md) for the complete value API,
capability gate, diagnostics, and current upstream limitation.

## Hierarchical triggers

One-bit hierarchical objects support the same trigger vocabulary as ports:

```cpp
co_await RisingEdge{dut.core.done};
co_await FallingEdge{dut.core.busy};
co_await Edge{dut.core.phase};
```

Only paths used by the compiled testbench receive generated edge observers.

## Generation cost

The whole elaborated hierarchy is represented in the generated C++ type, but
the proxy objects carry no runtime state. A discovery compile records the
operations actually instantiated by the testbench; the final wrapper emits
DPI exports and edge observers only for those path/operation pairs. An unused
hierarchical object therefore adds no simulator process, callback, or runtime
transport cost.

Inspect the inferred catalog without creating a user configuration file:

```sh
uv run --frozen cpptb-codegen rtl/design.sv --inspect-hierarchy
uv run --frozen cpptb-codegen rtl/design.sv \
  --hierarchy-json build/design-hierarchy.json
uv run --frozen cpptb-codegen rtl/design.sv \
  --check-hierarchy build/design-hierarchy.json
```

The JSON form is an optional review or CI snapshot of elaboration. It is
generator output, never testbench input.

## What can be forced

`force()` is generated for hierarchical objects used by the compiled testbench,
not for ordinary DUT ports. Registered input clocks stay owned by
`start_clock()` and cannot safely be forced or paused through the public API.

Calling `force()` or `release()` on an ordinary port produces an intentional
compile-time diagnostic rather than a generic missing-member error. For a
scheduler-owned clock, the diagnostic directs you back to
`TestContext::start_clock()` and states that coherent clock pause and override
are not yet supported.
