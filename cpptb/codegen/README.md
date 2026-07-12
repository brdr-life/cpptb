# C++ DPI binding generator

`generate_dpi_bindings.py` turns a SystemVerilog top module and a small JSON
manifest into the design-specific part of the C++ DPI framework:

- a typed C++ DUT hierarchy containing `coro::Signal` members;
- stable signal IDs, driven-input metadata, and the C++ binding function;
- a complete SystemVerilog DPI wrapper with batched input/output transport,
  multiple clock generators, edge and delay callbacks, parameter wiring, and
  DUT instantiation.

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
uv run --frozen python cpptb/codegen/generate_dpi_bindings.py \
  benchmarks/peripheral_suite/cpp_dpi/peripheral_suite.dpi.json
```

Use `--check` in CI to verify that checked-in generated files match their RTL
and manifest:

```sh
uv run --frozen python cpptb/codegen/generate_dpi_bindings.py \
  benchmarks/peripheral_suite/cpp_dpi/peripheral_suite.dpi.json --check
```

To run the frontend tests and compare both checked-in designs against
Verilator:

```sh
make cpptb-codegen-test
make cpptb-codegen-frontend-check
```

## Frontend configuration

Compilation inputs are simulator-neutral manifest fields: `sources`,
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

## Internal probes

The optional `internals` manifest list exposes elaborated variables, nets, and
one-dimensional fixed memories below `dut.internal`. Read-only entries provide
`get()`; `read_write` variables also provide `deposit(value)`. An independent
`force` capability adds immediate `force(value)` and `release()` primitives:

```json
"internals": [
  {"path": "cycle_count", "access": "read"},
  {"path": "memory", "access": "read_write", "force": true},
  {"path": "resolved_status", "access": "read", "force": true}
]
```

Values up to 32 and 64 bits cross DPI as native unsigned values. Wider packed
values use `svBitVecVal` words and `Bits<W>`. A deposit performs an immediate
SystemVerilog blocking assignment and never advances time. A same-callback
`get()` therefore observes the deposited value. The testbench still explicitly
awaits `Delay` or another trigger when downstream RTL must evaluate before
observation.

Internal probes currently use two-state transport: X/Z information is not
preserved. A probe deposit that races a DUT nonblocking assignment to the same
variable in the same time slot also has the usual SystemVerilog indeterminate
ordering and is outside the supported deterministic contract.

Verilator reports `BLKANDNBLK` when RTL also writes a deposited variable with a
nonblocking assignment. Such designs must compile with `-Wno-BLKANDNBLK`; the
testbench must avoid depositing in the same time slot as the RTL write. An
NBA-scheduled backdoor operation is intentionally not part of `deposit()`.

A testbench uses scalar and memory probes directly and chooses every scheduling
boundary explicitly:

```cpp
check(dut.internal.status.get(), expected_status);
dut.internal.pending_data.deposit(0x1234'5678u);
dut.internal.memory.at(address).deposit(expected);

// Immediate readback observes the deposited values in the same callback.
check(dut.internal.pending_data.get(), 0x1234'5678u);

// Delay only when dependent RTL needs time to evaluate.
co_await Delay{1_ps};
check(dut.read_data.get(), expected);
```

Read-only probes expose only `get()`, and `.at(index)` enforces the elaborated
SystemVerilog memory range.

Force and release call direct generated DPI exports and never schedule a delay
or evaluation phase. The generated wrapper keeps force RHS values in
module-lifetime shadow storage. Fixed memory force uses constant-index dispatch
and is capped at 1024 generated elements. A released variable retains its
forced value until RTL assigns it again; a released net returns to its resolved
drivers, following SystemVerilog semantics. The testbench explicitly awaits an
edge or `Delay` before checking dependent logic.

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

## Clock configuration

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

Non-clock one-bit DUT outputs can opt into the same trigger API through
`"edge_observers": ["rsp_valid"]`. Generated SV processes remain attached to
those signals, but cross-language callbacks occur only while the C++ scheduler
has a matching rising, falling, or change waiter. Testbench-driven scalar
inputs deliver their changed values directly in the runtime. Unsupported waits
fail explicitly instead of silently hanging.

The generator rejects ambiguous configurations: duplicate clock ports,
multiple primary clocks, generated/testbench clocks on DUT outputs, DUT clocks
on inputs, and timing fields on clocks not produced by the wrapper.

The generated binding is consumed by the reusable runtime in
`cpptb/dpi_runtime.hpp`. A new design provides a thin adapter connecting the
generated DUT binding to its test-registration function and timeout policy.
Compact directional open-array transport, scheduler dispatch, optimization
flags, timing, and the standard result line remain shared framework code. The
generated scheduler step carries observed words only; a separate output pull
copies driven words after initialization or a reported output change.

The generated wrapper transports simulation time in its configured
`timeprecision` and passes that precision to the C++ runtime. This gives
`Delay{1_ps}` the same physical-time meaning as SystemVerilog `#1ps` without
presenting a delay as a simulator scheduling phase. The executable contract is
in `cpptb/conformance`.

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
propagation, multidimensional or dynamic arrays, interfaces, and `inout` ports
remain explicit validation errors. Generated, testbench-driven, and
DUT-generated clocks may be mixed independently of those data-port shapes.
Signedness is retained in the elaborated contract but values cross the current
C++ API as raw bits; scalar `.get()` returns a zero-extended `uint32_t`, so a
testbench that needs a signed numeric interpretation must sign-extend it.
