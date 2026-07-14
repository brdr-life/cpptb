# C++ DPI binding generator

`cpptb-codegen` turns SystemVerilog sources into the design-specific part of
the C++ DPI framework. Ordinary targets use the source-first CLI; JSON
manifests remain an advanced compatibility path:

- a typed C++ DUT hierarchy containing `coro::Signal` members;
- stable signal IDs, driven-input metadata, and the C++ binding function;
- a complete SystemVerilog DPI wrapper with batched input/output transport,
  multiple clock generators, edge and delay callbacks, parameter wiring, and
  DUT instantiation;
- a C++ DPI adapter that selects the registered test and owns result policy.

The generator elaborates RTL through Slang's typed Python API. Port order,
directions, parameter-resolved widths, signedness, and two-state/four-state
metadata therefore come from a SystemVerilog compiler rather than a text
parser. Slang produces a simulator-independent `DesignIR`; renderers consume
that IR and do not depend on Slang or Verilator data structures.

The legacy Verilator JSON frontend remains available as a migration oracle.
`--compare-frontend verilator_json` elaborates with both compilers and rejects
any difference in the ordered name/direction/width contract used by the DPI
transport.

## Generate

```sh
uv run --frozen cpptb-codegen \
  examples/counter/counter.sv
```

Source root inference succeeds only when exactly one module is unambiguous;
otherwise pass `--top`. The source-first generator does not assign clock roles
or timing. C++ test code registers any input clocks with
`TestContext::start_clock()` and directly awaits DUT-produced clocks.

Use `--check` in CI to verify that checked-in generated files match their
inputs:

```sh
uv run --frozen cpptb-codegen \
  examples/counter/counter.sv --check
```

To run the frontend tests and compare both checked-in designs against
Verilator:

```sh
make cpptb-codegen-test
make cpptb-codegen-frontend-check
```

## Advanced manifest compatibility

Version-1 manifests remain supported for targets that need explicit source,
build, port-grouping, or frontend metadata. They are not the normal onboarding
workflow and are never required to expose internal RTL objects. Compilation
inputs are simulator-neutral manifest fields: `sources`,
`include_dirs`, `defines`, and `parameters`. `frontend` selects the compiler;
backend-specific dialect or diagnostic switches live under
`frontend_options`, keeping them out of the shared project description:

```json
{
  "frontend": "slang",
  "frontend_options": {
    "slang": {
      "standard": "1800-2023",
      "args": ["--timescale=1ns/1ns"]
    },
    "verilator_json": {
      "args": ["-Wno-TIMESCALEMOD"]
    }
  }
}
```

The locked `pyslang` dependency is installed by `uv` on first use. A command
line `--frontend slang|verilator_json` override is also available for focused
debugging and migration checks.

## Source-inferred hierarchy

Slang supplies the complete elaborated instance, generate, signal, memory,
packed-type, and parameter catalog. The generated `Dut` mirrors that hierarchy
directly; users do not name internal probes in a manifest:

```cpp
check(dut.core.status.get(), expected_status);
dut.core.pending_data.deposit(0x1234'5678u);
dut.core.memory.at(address).force(expected);
dut.core.memory.at(address).release();
```

Values up to 64 bits use native integer transport. Wider packed values use
`Bits<W>`, while four-state operations use `LogicBits<W>`. Generated packed
enum and struct value types preserve the elaborated type shape. Fixed memories,
multidimensional arrays, instance arrays, and generate arrays preserve their
SystemVerilog ranges.

`deposit()` performs an immediate blocking assignment. `force()` and
`release()` call immediate generated DPI exports and add no delay or evaluation
phase. A same-callback `get()` of the same object sees the new value. The
testbench explicitly awaits an edge or `Delay` only when dependent RTL must
execute before observation. A deposit that races an RTL assignment in the same
time slot has the usual SystemVerilog ordering ambiguity.

The whole hierarchy is present in the stateless C++ type. A discovery compile
records which path/operation pairs the testbench uses, and the final wrapper
emits only those DPI exports and edge observers. Unused hierarchy therefore
adds no simulator callbacks or runtime transport. See the
[hierarchy guide](../../docs/hierarchy.md) for API examples and inspection
commands.

The old `internals` manifest field remains accepted solely to compile existing
projects during migration. New projects should use the source-inferred natural
path and should not create an `internals` list.

## Manifest hierarchy

Unmatched ports remain at the DUT root. A path rule strips a port prefix and
places the result under a C++ object. Optional groups add another hierarchy
level for named interface members. For example:

```json
{
  "prefix": "spi_",
  "path": "spi",
  "groups": [
    {
      "path": "apb",
      "members": ["PADDR", "PWDATA", "PWRITE", "PSEL", "PENABLE"]
    }
  ]
}
```

This maps `spi_PADDR` to `dut.spi.apb.PADDR` and `spi_status` to
`dut.spi.status`. `type_names` can assign the same type, such as `ApbBus`, to
compatible hierarchy nodes.

## Legacy manifest clock configuration

Version-1 manifests can still describe clocks statically for compatibility.
The `clocks` list is independent of the DUT hierarchy and may contain zero,
one, or many clock domains. An empty list supports combinational or purely
timer-driven DUTs. Each entry names a one-bit DUT port, its source, and the
optional primary clock used for cycle reporting.

`source` has three supported values:

- `generated`: the wrapper drives a periodic DUT input. `half_period` controls
  the period and `phase` optionally offsets the first edge.
- `testbench`: C++ drives a DUT input through its ordinary typed signal. The
  wrapper observes every resulting transition, so a coroutine can implement
  an irregular, stopped, stepped, or data-dependent clock.
- `dut`: the wrapper observes a DUT output, such as a divided, recovered, or
  gated clock.

Omitting `source` preserves the original behavior and means `generated`.
For example:

```json
"clocks": [
  {
    "port": "core_clk",
    "source": "generated",
    "half_period": "2ns",
    "primary": true
  },
  {
    "port": "scan_clk",
    "source": "testbench"
  },
  {
    "port": "divided_clk",
    "source": "dut"
  }
]
```

Clock production and scheduler notification are separate concerns. A generated
clock advances at its declared period regardless of DPI callback work. Edge
waiters resume at the edge timestamp; a testbench uses `Delay{1_ps}` when it
wants the direct equivalent of SystemVerilog `#1ps` before observing settled
outputs. Delay callbacks are scheduled only when the C++ scheduler reports a
waiter.

All clock sources use the same signal-ID edge API, so `RisingEdge`,
`FallingEdge`, `Edge`, `First`, and `Join` do not depend on how a clock is
produced. Simultaneous clock edges are processed serially by the simulator;
when a `First` explicitly waits on simultaneous domains, its winner follows
the simulator's process ordering and should not be used to infer hardware
priority.

In a version-1 manifest, non-clock one-bit DUT outputs can opt into the same trigger API through
`"edge_observers": ["rsp_valid"]`. Generated SV processes remain attached to
those signals, but cross-language callbacks occur only while the C++ scheduler
has a matching rising, falling, or change waiter. Testbench-driven scalar
inputs deliver their changed values directly in the runtime. Unsupported waits
fail explicitly instead of silently hanging.

The generator rejects ambiguous configurations: duplicate clock ports,
multiple primary clocks, generated/testbench clocks on DUT outputs, DUT clocks
on inputs, and timing fields on clocks not produced by the wrapper.

The generated binding is consumed by the reusable runtime in
`include/cpptb/dpi_runtime.hpp`. A new design provides a thin adapter
connecting the generated DUT binding to its test-registration function and
timeout policy.
Compact directional open-array transport, scheduler dispatch, optimization
flags, timing, and the standard result line remain shared framework code. The
generated scheduler step carries observed words only; a separate output pull
copies driven words after initialization or a reported output change.

The generated wrapper transports simulation time in its configured
`timeprecision` and passes that precision to the C++ runtime. This gives
`Delay{1_ps}` the same physical-time meaning as SystemVerilog `#1ps` without
presenting a delay as a simulator scheduling phase. The executable contract is
in `tests/conformance/runtime`.

## Current boundary

The `DesignIR` records frontend-independent packed width, signedness, state
metadata, and fixed unpacked ranges. The transport supports scalar and packed
integral input/output ports plus any fixed number of unpacked dimensions. C++
preserves declared bounds and exposes elements as
`dut.port.at(i).at(j).get()` and `.set(value)`. Flattening keeps the last
dimension contiguous and orders every dimension by increasing numeric index,
so ascending, descending, and nonzero ranges retain
their SystemVerilog element identity.

Ports wider than 32 bits and wide array elements must be two-state `bit` and
use `uint64_t` through 64 bits or `Bits<W>` above 64 bits. Four-state X/Z value
propagation, dynamic arrays, interfaces, and `inout` ports remain explicit
validation errors. Generated, testbench-driven, and
DUT-produced clocks may be mixed independently of those data-port shapes.
Signedness is retained in the elaborated contract but values cross the current
C++ API as raw bits; scalar `.get()` returns a zero-extended `uint32_t`, so a
testbench that needs a signed numeric interpretation must sign-extend it.
