# cpptb

cpptb is an experimental C++20 coroutine testbench framework for
SystemVerilog simulators. It generates a typed DUT interface and a
standards-based DPI wrapper, then lets testbench code drive signals and await
simulator events directly:

```cpp
#include <cpptb/cpptb.hpp>
#include "dut.hpp"

using cpptb::TestContext;
using cpptb::coro::Delay;
using cpptb::coro::RisingEdge;
using cpptb::coro::Task;
using cpptb::Dut;
using namespace cpptb::coro;

Task<void> sequence(Dut dut, TestContext& test) {
    dut.clk.set(0);
    test.start_clock(dut.clk, 10_ns);

    dut.rst_n.set(0);
    dut.enable.set(0);
    co_await clock_cycles(dut.clk, 2);

    dut.rst_n.set(1);
    dut.enable.set(1);
    co_await RisingEdge{dut.clk};
    co_await Delay{1_ps};
    test.expect_eq("first count", dut.count.get(), 1);
}

CPPTB_REGISTER_TEST(sequence);
```

The framework currently targets Verilator for end-to-end testing, while its
generated transport uses standard SystemVerilog DPI constructs. Slang provides
simulator-independent elaboration for ports, parameters, hierarchy, and packed
types.

## What works

- Concurrent coroutine processes with `spawn`, `spawn_detached`, and `Join`.
- Rising, falling, or either-edge waits and arbitrary-clock cycle waits.
- Absolute `Delay`, trigger and task timeouts, and `First` races.
- Events, bounded typed queues, locks, semaphores, process cancellation, and
  typed task results.
- Typed scalar, wide packed, fixed-point, array, and hierarchical signals.
- Read, deposit, force, release, and edge access through natural generated
  hierarchy paths with usage-pruned DPI transport.
- Zero-, one-, and multi-clock designs with C++-owned input clocks and
  directly observed DUT-produced clocks.
- Batched and on-demand DPI transport selected by generated bindings.
- Multiple registered tests with one fresh simulator process per selection,
  fatal and nonfatal checks, test-owned process cleanup, and JSON results.
- Deterministic random streams, constrained transactions with adaptive optional
  solver fallback, and explicit functional coverage with bins and crosses.
- An optional `cpptb_vc` layer with transaction ports, in-order and keyed
  scoreboards, ready/valid helpers, and APB verification components.
- An apples-to-apples C++ DPI versus SystemVerilog benchmark suite with a hard
  1.10 performance-ratio guard and one documented direct-force transport
  waiver.

See [testbench authoring](docs/testbench-authoring.md),
[randomization and coverage](docs/random-stimulus.md),
[running tests](docs/running-tests.md),
[hierarchical DUT access](docs/hierarchy.md),
[scheduling](docs/scheduling.md), and [code generation](docs/code-generation.md)
for the detailed contracts.

## What does not work yet

Stated here rather than discovered the hard way:

- **Scheduler phase waits.** `co_await ReadWrite{}`, `ReadOnly{}`, and
  `NextTimeStep{}` report an actionable error in a default build rather than
  running: the default link owns clocks and timers but dispatches no simulator
  phases. Sample after `RisingEdge`, drive after `FallingEdge` instead — that is
  the supported pattern, and [scheduling](docs/scheduling.md) works through the
  gap and the plan for closing it.
- **Verilator is the only end-to-end simulator.** The generated transport is
  standard SV-DPI and elaboration is simulator-independent through Slang, but
  nothing else is exercised in CI yet.
- **The build-time discovery pass runs your testbench.** A testbench that hangs
  or exits at time zero therefore fails the *build*, and the failure reads like
  a compile error. If a first build stalls, suspect the testbench's own control
  flow before the toolchain.
- **Coverage is explicit.** There are no automatic per-value bins for a sampled
  type and no `default` bin; every bin is declared. `with (expr)` cross filters
  are matched at bin granularity — exact whenever the expression is constant
  across each bin, and `where()` records the expression it stands for so a
  translation from SystemVerilog stays checkable.
- **Sources added through raw `verilator_args` are not tracked for rebuilds**,
  and `.svh` files are rejected in the source list — include their directory
  instead.
- Editing a running process's coroutine set changes stimulus ordering for
  random streams: two testbenches that draw from the same seed produce
  identical stimulus only if they wake the same processes in the same order.

The measurements behind the performance and equivalence claims — and forty-one
findings from porting Ibex's UVM testbenches, with reduced cases — live under
[experiments/open_core_ports](experiments/open_core_ports/), which fetches
several GB of external dependencies and is not part of the installable package.

## Requirements

`make doctor` checks every requirement below against the local toolchain and
reports what is missing or too old, with the fix for the current platform:

```sh
make doctor
```

- A C++20 compiler. Verilator adds no `-std` flag of its own, so cpptb passes
  `-std=c++20` explicitly wherever it drives a Verilator build.
- Verilator 5.046 or newer. The example and conformance builds pass
  `--no-sched-zero-delay`, which Verilator added in 5.046; older releases fail
  with `%Error: Invalid option: --no-sched-zero-delay`.
- Python 3.11 or newer and [uv](https://docs.astral.sh/uv/).
- CMake 3.20 or newer for installation and unit-test integration.
- Optional: Z3 4.15.5 or newer for the `cpptb::z3` constraint backend
  (`-DCPPTB_WITH_Z3=ON`). Earlier releases lack `z3::get_full_version()`. The
  adapter is off by default, and its tests skip when Z3 is absent or too old.

`make z3-toolchain` is the portable way to satisfy the optional Z3 requirement
on both macOS and Linux, and is what CI uses. It installs a pinned `z3-solver`
wheel, which ships `z3++.h` and the shared library for both platforms on arm64
and x86_64, then writes a `z3.pc` so the existing pkg-config detection finds it:

```sh
make z3-toolchain
export PKG_CONFIG_PATH="$PWD/build/pkgconfig:$PKG_CONFIG_PATH"   # for CMake
```

`make` targets pick the generated `z3.pc` up automatically; the export is only
needed when invoking CMake directly. The wheel is installed under `build/z3/`
rather than the project virtual environment because `uv sync` prunes anything
outside the lockfile, which would break already-linked binaries.

Linux and macOS are both supported. On Debian/Ubuntu, Verilator must be built
from source because the packaged release is older than 5.046; `libfl-dev`
supplies the `FlexLexer.h` header that is easy to miss:

```sh
sudo apt-get install -y autoconf bison flex libfl-dev help2man \
    zlib1g-dev pkg-config
```

The packaged `libz3-dev` is currently older than 4.15.5, so the optional Z3
adapter needs Z3 built from source on those distributions.

Mojo is only needed for the historical experiments under `experiments/`.

## Build and test

```sh
uv sync --frozen
make test
```

The default `make` target prints the available developer commands. CMake can
build the C++ unit tests and install the header-only target:

```sh
cmake -S . -B build/cmake -DBUILD_TESTING=ON
cmake --build build/cmake
ctest --test-dir build/cmake --output-on-failure
cmake --install build/cmake --prefix build/install
```

Installed CMake consumers use:

```cmake
find_package(cpptb CONFIG REQUIRED)
target_link_libraries(my_testbench PRIVATE cpptb::cpptb)
```

Reusable verification components are a separate optional target:

```cmake
target_link_libraries(my_testbench PRIVATE cpptb::vc)
```

## Build a testbench

An ordinary project needs only RTL and a C++ testbench:

```text
counter-project/
|-- counter.sv
`-- testbench.cpp
```

From that directory, the public command owns inference, binding generation,
discovery, compilation, caching, and test selection:

```sh
cpptb build
cpptb list
cpptb test
cpptb test counter_reset_defaults
```

The conventional `rtl/` and `tests/` directories are also discovered. Use an
optional `cpptb.toml` or explicit `--source`, `--testbench`, and `--top` options
only when the project is ambiguous. Generated source, metadata, logs, binaries,
and results stay under `build/cpptb/<target>/`; user code always includes the
stable generated `dut.hpp` header.

Start with [the examples](examples/README.md). They progress from a counter and
clockless delays through a ready/valid scoreboard, multiple clocks, APB
transactions, and expected timeout/cancellation paths. Every example includes
an equivalent pure-SystemVerilog testbench and runs under `make examples-test`.

## Documentation site

The Markdown under `docs/` is the shared source for two static HTML builds.
Build both variants or preview either one locally:

```sh
make docs-build
make docs-sphinx-serve    # http://localhost:8001
make docs-zensical-serve  # http://localhost:8002
```

The Sphinx build uses MyST and Furo; the parallel Zensical build uses its
modern theme and built-in search. Generated HTML stays under `build/docs/` and
is not committed.

## Benchmarks

Every performance feature has a C++ DPI implementation and an equivalent
SystemVerilog implementation. Benchmarks run serially and record system-load
evidence without rescaling samples:

```sh
make feature-list
make feature-test FEATURE=event
make feature-benchmark FEATURE=event
make feature-regression
make framework-comparison-heavy-benchmark
make framework-comparison-open-cores-benchmark
```

CI checks behavior and equivalence. Performance gates run on a controlled local
or dedicated runner because shared CI timing is not stable enough for a 10%
threshold. Compact reference results live in `benchmarks/baselines/`.

The heavy four-mode suite compares pure SystemVerilog, C++ DPI, raw C++ VPI,
and Cocotb on a 32-tap FIR, variable-length packet CRC32, and 4x4 matrix
accelerator. Its independent scoreboards perform the same software arithmetic
and reject samples unless their semantic evidence and simulated cycle counts
match exactly.

The open-source core suite extends that comparison to PicoRV32 firmware,
secworks AES-128 register traffic, and 64-bit AXI-stream Ethernet FCS frames.
Vendored RTL is pinned with its upstream license and each workload elaborates
only the selected core.

## Repository map

- `include/cpptb/`: installable C++ framework headers.
- `include/cpptb_vc/`: optional reusable verification-component headers.
- `tools/codegen/`: public `cpptb` CLI and lower-level Slang code generator.
- `examples/`: end-to-end C++ DPI examples and SystemVerilog equivalents.
- `tests/`: unit, generator, and simulator conformance tests.
- `benchmarks/`: reproducible feature and four-mode comparison suites.
- `docs/`: user guide, architecture, scheduling, and performance notes.
- `experiments/`: historical Mojo, VPI, C API, and UVM investigations.

cpptb is a research-stage framework. The conformance suite captures the
supported contract; portability beyond the tested simulator set remains active
work.

## License

Framework code is licensed under Apache-2.0. Benchmark RTL with separate
copyright or license notices remains under those terms; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
