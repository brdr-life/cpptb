# Running tests

This page covers the run workflow around the `cpptb` command: registering
tests, how a project is laid out, the lower-level runner protocol, and the
structured JSON results a CI system consumes. The command's options are
specified in [cpptb command line](cli.md), and every configuration key in
the [cpptb.toml reference](cpptb-toml.md).

The command is optional. It is one harness over a reusable C++ framework, and
[Framework test lifecycle](test-lifecycle.md) documents that framework's
registration, checks, process ownership, terminal states, and result model for
embedding in a build or regression system of your own.

A compiled cpptb simulator may contain one or more registered tests. The
reference harness selects exactly one test per simulator invocation so each
run starts with fresh DUT and runtime state. Another harness may embed the
same framework APIs and choose a different process policy.

## Register tests

Register each root coroutine in the user-authored testbench translation unit:

```cpp
Task<void> reset_defaults(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
    test.start_clock(dut.clk, 10_ns);

    dut.rst_n.set(0);
    co_await RisingEdge{dut.clk};
    co_await ReadOnly{};
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
cpptb build             # generate the typed DUT and compile the simulator
cpptb list              # show the registered tests
cpptb test              # run them all, each in a fresh simulator process
cpptb test reset_defaults
```

Tests run serially, one fresh process at a time, which keeps simulator
global state isolated and benchmark load predictable. Build inputs are
content-fingerprinted, so the second and subsequent commands reuse the
compiled binary until RTL, C++, framework headers, options, or tool versions
change. Every option — project selectors, backend overrides, waveform
tracing, timeouts, cache bypass — is documented in
[cpptb command line](cli.md).

## Project layout and build ownership

An ordinary user project needs only RTL and a testbench. It may use a flat
layout:

```text
counter-project/
├── counter.sv                       # authored RTL
└── testbench.cpp                    # authored tests
```

or conventional source directories:

```text
counter-project/
├── rtl/
│   └── counter.sv
└── tests/
    ├── testbench.cpp
    └── drivers.cpp
```

Everything else stays under one ignored build directory:

```text
build/                               # generated and gitignored
    └── cpptb/
        └── counter/
            ├── generated/
            │   ├── dut.hpp          # stable public include
            │   ├── counter_dut.hpp
            │   ├── counter_binding.hpp
            │   ├── dpi_counter.cpp
            │   └── dpi_counter.sv
            ├── metadata/
            │   ├── access.json
            │   └── access-objects/
            ├── obj/
            │   └── Vdpi_counter
            ├── results/
            ├── build.log
            └── build-state.json
```

This repository additionally keeps a pure-SystemVerilog comparison bench under
`examples/counter/systemverilog/`. It exists for cpptb's equivalence and
performance regression and is not required in a user project.

The files under `generated/` and `metadata/` are disposable implementation
artifacts. In particular, `access.json` is recovered by compiling the
authored C++ testbench translation units alone -- no link, no execution --
and scanning the objects for the discovery records the framework headers
plant; users do not write it. Clocks need no build artifact at all: they
are registered at run time by `start_clock()` and driven by generated
per-signal SystemVerilog tasks.
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
filesystem conventions — a project with neither options nor a `cpptb.toml`
still builds from the conventional layout above. One-off overrides
(`--source`, `--testbench`, `--top`) are specified in
[cpptb command line](cli.md); persistent configuration lives in the
[cpptb.toml reference](cpptb-toml.md), which documents every section and key
with its default. Missing tools, sources, tests, include directories,
ambiguous top modules, and invalid configuration all produce project-level
diagnostics before compilation starts.

## Lower-level runner protocol

`cpptb-run` remains available when another build system already owns the
simulator executable:

```sh
cpptb-run list -- path/to/simulator
cpptb-run run --all --result-dir results -- path/to/simulator
```

`--` separates runner options from the simulator command. `run` also accepts
`--timeout` (a wall-time limit per process) and `--seed`; `--result-dir`
defaults to `cpptb-results`. The underlying environment protocol remains
intentionally small:

- `CPPTB_LIST_TESTS=1` prints one `CPPTB_TEST name` line per registered test.
- `CPPTB_TEST=name` selects one test.
- `CPPTB_RESULT_FILE=path.json` requests the structured result file.

An embedding harness can also skip both command-line runners and consume the
C++ embedding API directly — [Embedding and results](library/embedding.md)
documents `registered_tests<Dut>()`, `RunRequest`, the
`run_registered_test(...)` selection overload, and `ResultSink`.

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
