# Getting started

Install Verilator, a C++20 compiler, Python 3.11 or newer, CMake, and `uv`.
From a checkout:

```sh
uv sync --frozen
make test
```

Use `examples/counter` as the smallest complete project template. For a more
realistic starting point, copy `examples/apb_regfile` for a transaction-based
testbench or `examples/fifo_scoreboard` for concurrent verification
components. A design supplies RTL sources and one typed coroutine in
`testbench.cpp`. `cpptb-codegen` derives the top module when exactly one source
module is a valid root, then generates the typed DUT, DPI wrapper, and C++
transport.

Generate bindings with:

```sh
uv run --frozen cpptb-codegen path/to/design.sv
```

Use `--top module_name` when root inference is ambiguous. Clock timing belongs
to the C++ testbench, not code generation. Initialize each input clock and
register its period before the test's first `co_await`:

```cpp
dut.clk.set(0);
test.start_clock(dut.clk, 10_ns);
```

Repeat `start_clock()` for independent input clocks. Its optional third
argument is a phase before the normal first half-period. DUT-produced clocks
need no registration; wait on their generated signal normally.

The ordinary workflow has no user-authored manifest or transport source.
Version-1 `.dpi.json` manifests remain available as an advanced compatibility
path for build inputs such as include directories, defines, and parameter
overrides. Slang always infers the complete elaborated hierarchy from RTL.

The generated C++ DUT exposes explicit `get()` and `set()` operations. Time
advances only when a coroutine awaits a scheduling primitive; writing a signal
or depositing an internal value does not insert a delay. `start_clock()`
registers simulator timing but does not itself suspend the calling coroutine.

Continue with [testbench authoring](testbench-authoring.md),
[hierarchical DUT access](hierarchy.md),
[clocking](clocking.md), and [scheduling](scheduling.md). The
[example guide](examples.md) maps common DUT styles to a checked
C++/SystemVerilog pair.
