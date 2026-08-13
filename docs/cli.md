# cpptb command line

The complete reference for the `cpptb` command. For the workflow around it —
registering tests, project layout, structured results — see
[Running tests](running-tests.md); for the configuration file the command
reads, see [cpptb.toml](cpptb-toml.md).

Configuration precedence is command-line options first, then `cpptb.toml`,
then filesystem conventions. Every option below therefore overrides its
`cpptb.toml` counterpart for one invocation without editing the project.

```sh
cpptb --version
cpptb build --help
```

## Commands

| Command | Purpose |
|---|---|
| `cpptb build` | Resolve the project, generate DUT bindings, and build the simulator executable |
| `cpptb list` | Build if needed, then list the compiled test catalog |
| `cpptb test [TEST ...]` | Build if needed, then run all tests or the named tests serially |

Each test runs in its own fresh simulator process; `cpptb test` with no
names runs the whole catalog.

## Project and build options

All three commands accept the same selectors:

| Option | Purpose |
|---|---|
| `--project PATH` | Project directory; defaults to the current directory |
| `--source PATH_OR_GLOB` | RTL source, directory, or glob; repeat for multiple inputs |
| `--testbench PATH_OR_GLOB` | C++ testbench source, directory, or glob; repeat as needed |
| `--top MODULE` | Select the SystemVerilog DUT top module |
| `--target NAME` | Override the generated simulator target name |
| `--build-dir PATH` | Build root, relative to the project unless absolute |
| `--simulator verilator` | Simulator backend; currently Verilator |
| `--framework-root PATH` | Locate a cpptb checkout, install prefix, or include directory |
| `--timing-backend {verilator-direct,vpi}` | Override `build.timing_backend`, so one project can be built and compared under both supported phase backends |
| `--deferred-writes` | Override `build.deferred_writes`: `set()` queues and flushes at the ReadWrite phase. One-way — there is no `--no-deferred-writes`, and the default is already `true`, so the flag matters only against a project that set `deferred_writes = false` |
| `--wave [{fst,vcd}]` | Build with waveform tracing and dump one wave file per test, landing beside its result as `results/<test>.fst`; FST when no format is given. See [Waveforms](waveforms.md) |
| `--experimental-four-state` | Request experimental four-state mode after a Verilator semantic capability probe (currently upstream-blocked; see [Four-state values](four-state.md)) |
| `--rebuild` | Ignore the content cache and rebuild |
| `--verbose` | Show normally hidden generation and compiler commands |

This `--top` selects the HDL DUT and is separate from PeakRDL's register-map
`--top`; register generation and its naming options are documented in
[Generate a register model](verification-components/register-generation.md).

## Per-command options

| Option | Command | Purpose |
|---|---|---|
| `--timeout SECONDS` | `list` | Wall-time limit for catalog discovery |
| `--timeout SECONDS` | `test` | Wall-time limit, applied independently to each test process |
| `--result-dir PATH` | `test` | Result directory (default: `build/cpptb/TARGET/results`) |

The wall-time `--timeout` is a harness limit on the OS process. It is
separate from the in-simulation watchdog, `run.timeout_cycles` in
[cpptb.toml](cpptb-toml.md#run), which ends a hung test from inside the
simulator with a specific diagnostic.

## Exit codes

CI can distinguish a red test from broken plumbing:

| Code | Meaning |
|---:|---|
| `0` | Every selected test passed (skips and expected failures count as success) |
| `1` | At least one test failed or errored during its run |
| `2` | The run never started: project, configuration, generation, or build error — the diagnostic is on stderr, prefixed `cpptb:` |

`cpptb-run` follows the same convention.

## Environment variables

There is no `--seed` option: the seed route is the `CPPTB_RANDOM_SEED`
environment variable (decimal or `0x`-prefixed hex, default `1`), which
`cpptb test` propagates into each simulator process — set it once and every
test in the run replays deterministically. The complete set a built
simulator reads — `CPPTB_TEST`, `CPPTB_LIST_TESTS`, `CPPTB_RANDOM_SEED`,
`CPPTB_LOG_LEVEL`, `CPPTB_RESULT_FILE`, `CPPTB_WAVE` — is tabulated in the
[TestContext reference](library/test-context.md#environment-variables), and
[Seeds, streams, and replay](randomization/reproducibility.md) covers the
replay workflow.

## Lower-level commands

Three lower-level entry points exist for custom build systems and harnesses;
ordinary projects do not need them:

- `cpptb-codegen` drives generation directly from RTL sources — see
  [Code generation](code-generation.md#lower-level-surfaces).
- `cpptb-run` exposes the runner's executable protocol — see
  [Running tests](running-tests.md#lower-level-runner-protocol).
- `cpptb-rggen` imports native RgGen YAML/JSON/TOML register contracts — see
  [Generate a register model](verification-components/register-generation.md).
