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
components. A design supplies RTL sources and one or more registered
coroutines in `testbench.cpp`. The public `cpptb` command derives the top module,
generates the typed DUT and DPI transport, compiles the simulator, and caches
the complete build.

From an installed package, run these commands in the project directory:

```sh
cpptb build
cpptb list
cpptb test
cpptb test reset_defaults
```

From this source checkout, prefix them with `uv run --frozen`, or point at an
example directly:

```sh
uv run --frozen cpptb test --project examples/counter --build-dir build
```

Zero-configuration discovery accepts either root-level `.sv`/`.v` files plus
`testbench.cpp`, or RTL under `rtl/` plus C++ sources under `tests/`. Use
`--top module_name` when root inference is ambiguous. See
[running tests](running-tests.md) for optional configuration and explicit
source flags.

Clock timing belongs to the C++ testbench. Initialize each input clock and
register its period before the test's first `co_await`:

```cpp
dut.clk.set_now(0);
test.start_clock(dut.clk, 10_ns);
```

Repeat `start_clock()` for independent input clocks. Its optional third
argument is a phase before the normal first half-period. DUT-produced clocks
need no registration; wait on their generated signal normally.

The ordinary workflow has no user-authored manifest, wrapper, transport source,
or Makefile. Slang infers the complete elaborated hierarchy from RTL. An
optional `cpptb.toml` can supply source globs, include paths, defines, and
parameter overrides for designs that need them.

The generated C++ DUT exposes explicit `get()` and `set()` operations. Time
advances only when a coroutine awaits a scheduling primitive; writing a signal
or depositing an internal value does not insert a delay. `start_clock()`
registers simulator timing but does not itself suspend the calling coroutine.

Register one or more root coroutines with `CPPTB_REGISTER_TEST`. `cpptb test`
discovers the compiled catalog and launches every selection in a fresh
simulator process. `cpptb-run` remains available for higher-level harnesses
that already own compilation and only need the executable protocol.

```sh
cpptb list
cpptb test
```

Continue with [testbench authoring](testbench-authoring.md),
[running tests](running-tests.md),
[hierarchical DUT access](hierarchy.md),
[clocking](clocking.md), and [scheduling](scheduling.md). The
[example guide](examples.md) maps common DUT styles to a checked
C++/SystemVerilog pair.
