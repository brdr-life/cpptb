# cpptb

cpptb is an experimental C++20 coroutine testbench framework for
SystemVerilog simulators. It generates a typed DUT interface and a
standards-based DPI wrapper, then lets testbench code drive signals and await
simulator events directly:

```cpp
#include <cpptb/cpptb.hpp>

using cpptb::coro::Delay;
using cpptb::coro::RisingEdge;
using cpptb::coro::Task;
using namespace cpptb::coro;

Task<void> sequence(CounterTb tb) {
    tb.dut.rst_n.set(0);
    tb.dut.enable.set(0);
    co_await clock_cycles(tb.dut.clk, 2);

    tb.dut.rst_n.set(1);
    tb.dut.enable.set(1);
    co_await RisingEdge{tb.dut.clk};
    co_await Delay{1_ps};
    tb.expect_eq("first count", tb.dut.count.get(), 1);
}
```

The framework currently targets Verilator for end-to-end testing, while its
generated transport uses standard SystemVerilog DPI constructs. Slang provides
simulator-independent elaboration for ports, parameters, hierarchy, and packed
types.

## What works

- Concurrent coroutine processes with `spawn`, `spawn_detached`, and `Join`.
- Rising, falling, or either-edge waits and arbitrary-clock cycle waits.
- Absolute `Delay`, trigger and task timeouts, and `First` races.
- Events, typed channels, process cancellation, and typed task results.
- Typed scalar, wide packed, fixed-point, array, and hierarchical signals.
- Read, deposit, force, and release access to generated internal probes.
- Zero-, one-, and multi-clock designs with generated, testbench-driven, or
  DUT-produced clocks.
- Batched and on-demand DPI transport selected by generated bindings.
- An apples-to-apples C++ DPI versus SystemVerilog benchmark suite with a hard
  1.10 performance-ratio guard.

See [testbench authoring](docs/testbench-authoring.md),
[scheduling](docs/scheduling.md), and [code generation](docs/code-generation.md)
for the detailed contracts.

## Requirements

- A C++20 compiler.
- Verilator 5.026 or newer.
- Python 3.11 or newer and [uv](https://docs.astral.sh/uv/).
- CMake 3.20 or newer for installation and unit-test integration.

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

## Generate a binding

The checked-in examples include their generated output for readability and
reproducible builds. Regenerate or verify one with:

```sh
uv run --frozen cpptb-codegen examples/multiclock/dual_clock_mailbox.dpi.json
uv run --frozen cpptb-codegen \
  examples/multiclock/dual_clock_mailbox.dpi.json --check
```

Start with [the examples](examples/README.md). The generated wrapper and
transport adapter are kept beside each example, while `testbench.cpp` contains
the user-authored sequence.

## Benchmarks

Every performance feature has a C++ DPI implementation and an equivalent
SystemVerilog implementation. Benchmarks run serially and record system-load
evidence without rescaling samples:

```sh
make feature-list
make feature-test FEATURE=event
make feature-benchmark FEATURE=event
make feature-regression
```

CI checks behavior and equivalence. Performance gates run on a controlled local
or dedicated runner because shared CI timing is not stable enough for a 10%
threshold. Compact reference results live in `benchmarks/baselines/`.

## Repository map

- `include/cpptb/`: installable C++ framework headers.
- `tools/codegen/`: Slang-backed `cpptb-codegen` package.
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
