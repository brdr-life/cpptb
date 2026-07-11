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

The generator rejects ambiguous configurations: duplicate clock ports,
multiple primary clocks, generated/testbench clocks on DUT outputs, DUT clocks
on inputs, and timing fields on clocks not produced by the wrapper.

The generated binding is consumed by the reusable runtime in
`cpptb/dpi_runtime.hpp`. A new design provides a thin adapter connecting the
generated DUT binding to its test-registration function and timeout policy.
Open-array transport, scheduler dispatch, optimization flags, timing, and the
standard result line remain shared framework code.

The generated wrapper transports simulation time in its configured
`timeprecision` and passes that precision to the C++ runtime. This gives
`Delay{1_ps}` the same physical-time meaning as SystemVerilog `#1ps` without
presenting a delay as a simulator scheduling phase. The executable contract is
in `cpptb/conformance`.

## Current boundary

The `DesignIR` already records frontend-independent port type, signedness, and
state metadata. The current transport supports input and output scalar or
packed integral ports up to 32 bits and arbitrary mixes of generated,
testbench-driven, and DUT-generated clocks. Transport validation intentionally
rejects wider ports, unpacked arrays, interfaces, `inout` ports, and four-state
value propagation. Those can now be added below the frontend boundary without
changing RTL elaboration or the testbench API.
