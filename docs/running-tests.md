# Running tests with the reference harness

This page documents the optional `cpptb` command-line harness. The reusable
C++ registration, checks, process ownership, terminal states, and result model
are documented separately in [Framework test lifecycle](test-lifecycle.md).

A compiled cpptb simulator may contain one or more registered tests. The
reference harness selects exactly one test per simulator invocation so each
run starts with fresh DUT and runtime state. Another harness may embed the
same framework APIs and choose a different process policy.

## Register tests

Register each root coroutine in the user-authored testbench translation unit:

```cpp
Task<void> reset_defaults(Dut dut, TestContext& test) {
    dut.clk.set(0);
    test.start_clock(dut.clk, 10_ns);

    dut.rst_n.set(0);
    co_await RisingEdge{dut.clk};
    co_await Delay{1_ps};
    test.expect_eq("reset count", dut.count.get(), 0u);
}

Task<void> counts_when_enabled(Dut dut, TestContext& test) {
    // Drive this test from its own initial DUT state.
}

CPPTB_REGISTER_TEST(reset_defaults);
CPPTB_REGISTER_TEST(counts_when_enabled);
```

The registered name is the C++ function name. If a binary contains one test,
running it directly selects that test for compatibility with small benches.
An unknown or duplicate `cpptb test` selection produces an actionable catalog
error before simulation work is scheduled.

## List and run

Build the simulator, list its catalog, run all tests, or select one test:

```sh
cpptb build
cpptb list
cpptb test
cpptb test counter_reset_defaults
```

`--timeout` is a wall-time limit for each process. Tests run serially, one
fresh process at a time, which keeps simulator global state isolated and
benchmark load predictable. Build inputs are content-fingerprinted, so the
second and subsequent commands reuse the compiled binary until RTL, C++,
framework headers, options, or tool versions change. Use `--rebuild` to bypass
the cache and `--verbose` to display the normally hidden compiler commands.

## CLI reference

The installed command is self-documenting:

```sh
cpptb --help
cpptb build --help
cpptb list --help
cpptb test --help
```

| Command | Purpose |
|---|---|
| `cpptb build` | Resolve the project, generate DUT bindings, and build the simulator executable |
| `cpptb list` | Build if needed, then list the compiled test catalog |
| `cpptb test [TEST ...]` | Build if needed, then run all tests or the named tests serially |

All three commands accept the same project and build selectors:

| Option | Purpose |
|---|---|
| `--project PATH` | Project directory; defaults to the current directory |
| `--source PATH_OR_GLOB` | RTL source, directory, or glob; repeat for multiple inputs |
| `--testbench PATH_OR_GLOB` | C++ testbench source, directory, or glob; repeat as needed |
| `--top MODULE` | Select the SystemVerilog DUT top module |
| `--target NAME` | Override the generated simulator target name |
| `--build-dir PATH` | Select the build root, relative to the project unless absolute |
| `--simulator verilator` | Select the simulator backend; currently Verilator |
| `--framework-root PATH` | Locate a cpptb checkout, install prefix, or include directory |
| `--rebuild` | Ignore the content cache and rebuild |
| `--verbose` | Show normally hidden generation and compiler commands |

`cpptb list --timeout SECONDS` limits catalog discovery wall time.
`cpptb test --timeout SECONDS` applies the wall-time limit independently to
each test process, and `--result-dir PATH` overrides the default
`build/cpptb/TARGET/results` directory.

This `--top` selects the HDL DUT and is separate from PeakRDL's register-map
`--top`. Register generation and all of its naming options are documented in
[Generate a register model](verification-components/register-generation.md).

## Project layout and build ownership

An ordinary user project needs only RTL and a testbench. It may use a flat
layout:

```text
counter-project/
|-- counter.sv                       # authored RTL
`-- testbench.cpp                    # authored tests
```

or conventional source directories:

```text
counter-project/
|-- rtl/
|   `-- counter.sv
`-- tests/
    |-- testbench.cpp
    `-- drivers.cpp
```

Everything else stays under one ignored build directory:

```text
build/                               # generated and gitignored
    `-- cpptb/
        `-- counter/
            |-- generated/
            |   |-- dut.hpp          # stable public include
            |   |-- counter_dut.hpp
            |   |-- counter_binding.hpp
            |   |-- discover_counter_clocks.cpp
            |   |-- dpi_counter.cpp
            |   `-- dpi_counter.sv
            |-- metadata/
            |   |-- clocks.json
            |   `-- access.json
            |-- obj/
            |   `-- Vdpi_counter
            |-- results/
            |-- build.log
            `-- build-state.json
```

This repository additionally keeps a pure-SystemVerilog comparison bench under
`examples/counter/systemverilog/`. It exists for cpptb's equivalence and
performance regression and is not required in a user project.

The files under `generated/` and `metadata/` are disposable implementation
artifacts. In particular, `clocks.json` and `access.json` are generated by
compiling and inspecting the authored C++ testbench; users do not write them.
The generated include directory is added automatically, so testbenches use one
stable include and type regardless of the RTL top name:

```cpp
#include <cpptb/cpptb.hpp>
#include "dut.hpp"

using cpptb::Dut;
```

The example Make targets are optional, thin aliases over the same command:

```sh
make -C examples/counter build
make -C examples/counter test
make -C examples/counter run TEST=counter_reset_defaults
```

## Project discovery and configuration

Configuration precedence is command-line options, then `cpptb.toml`, then
filesystem conventions. The zero-config conventions are:

- RTL from `rtl/**/*.sv` and `rtl/**/*.v`, or root-level `.sv` and `.v` files.
- C++ from `tests/**/*.cpp`, or root-level `testbench.cpp`.
- One inferred top module and a `build/` artifact root.

Point at nonstandard files directly when that is enough:

```sh
cpptb test \
  --source hardware/core.sv \
  --source hardware/peripheral.sv \
  --testbench verification/core_test.cpp \
  --top core
```

For persistent project options, add the optional compact configuration:

```toml
[design]
sources = ["rtl/packages/*.sv", "rtl/**/*.sv"]
top = "processor"
include_dirs = ["rtl/include"]
defines = ["SIMULATION=1"]
parameters = { DATA_WIDTH = 64 }

[testbench]
sources = ["verification/testbench.cpp", "verification/drivers/*.cpp"]
include_dirs = ["verification/include"]

[build]
directory = "build"
simulator = "verilator"
optimization = "-O2"
cxx_flags = ["-Wall"]
```

`optimization` governs both halves of the build: the C++ testbench and the model
Verilator generates. Verilator optimizes neither by default, so this defaults to
`-O2` rather than leaving a coroutine-heavy testbench unoptimized. Lower it for
a debug build, which makes breakpoints and stack traces behave:

```toml
[build]
optimization = "-O0"
cxx_flags = ["-g"]
```

Changing it re-fingerprints the build, so the next `cpptb build` recompiles
instead of reusing objects from the previous setting.

Four-state Verilator work is guarded separately from raw simulator arguments:

```toml
[build]
experimental_four_state = true
```

The option currently runs a semantic capability probe and reports that
Verilator does not preserve the required X/Z behavior. Do not add
`--fourstate` to `verilator_args`; that bypass is rejected because accepting
the upstream flag does not imply correct four-state execution. See
[four-state values](four-state.md) for details.

Source patterns are expanded in listed order, with each pattern sorted
deterministically. `cpptb build` reports ambiguous top modules with the exact
`--top` remedy. Missing tools, sources, tests, include directories, and invalid
configuration produce project-level diagnostics before compilation starts.

## Lower-level runner protocol

`cpptb-run` remains available when another build system already owns the
simulator executable:

```sh
cpptb-run list -- path/to/simulator
cpptb-run run --all --result-dir results -- path/to/simulator
```

`--` separates runner options from the simulator command. The underlying
environment protocol remains intentionally small:

- `CPPTB_LIST_TESTS=1` prints one `CPPTB_TEST name` line per registered test.
- `CPPTB_TEST=name` selects one test.
- `CPPTB_RESULT_FILE=path.json` requests the structured result file.

The C++ API also exposes `registered_tests<Dut>()`, `RunRequest`, the
`run_registered_test(...)` selection overload, and an optional `ResultSink`.
An embedding harness can therefore consume lifecycle callbacks directly and
does not have to adopt either command-line runner.

## Framework lifecycle behavior

The harness consumes the framework's compiled catalog and `TestResult`; it
does not implement check or process semantics. See
[Framework test lifecycle](test-lifecycle.md) for `expect()`, `expect_eq()`,
`require()`, `require_eq()`, owned processes, terminal states, diagnostic
formatting, and embedding callbacks.

## Structured results

Each test result uses schema version 5 and records lifecycle metadata, its
terminal status, check counts, simulation and wall time, and structured
failure and warning records:

```json
{
  "schema_version": 5,
  "test_name": "reset_defaults",
  "case_name": "",
  "status": "passed",
  "status_reason": "",
  "tags": ["smoke"],
  "random_seed": 4660,
  "random_algorithm": "xoshiro256ss-v1",
  "constraint_backend": "adaptive",
  "constraint_backend_version": "",
  "random_sampling_solves": 1,
  "random_solver_solves": 0,
  "checks": 1,
  "failures": 0,
  "warnings": 0,
  "simulation_time_fs": 5001000,
  "wall_time_ns": 8000,
  "failure_records": [],
  "warning_records": [],
  "wait_graph": null
}
```

Failure records distinguish expectations, requirements, exceptions, timeouts,
unexpected passes, and test selection errors. Failure and warning records
include the source location, simulation time, stable process ID, process spawn
location, and formatted comparison values when applicable. Schema versions 1
through 4 remain readable by the reference runner. A timed-out result may
carry a structured `wait_graph`; passing results use `null`. The runner writes a JSON
result and combined simulator log for every test, prints a compact summary, and returns
nonzero when any test fails or encounters an infrastructure error.

CLI filtering, JUnit output, waveform-on-failure reruns, wall-time policy, and
presentation of build or infrastructure failures remain harness work. The
framework now exposes tags and parameterized case descriptors for a harness to
consume; CLI tag expressions are not yet part of the reference harness.

Use `cpptb-run run --seed 0x1234 ...` or `CPPTB_RANDOM_SEED=0x1234` to replay
the deterministic random stream recorded in a result. See
[Seeds, streams, and replay](randomization/reproducibility.md) for random
process-stream semantics.
