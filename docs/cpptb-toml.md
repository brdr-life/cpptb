# cpptb.toml reference

`cpptb.toml` is the optional project configuration file, read from the
project directory. Everything in it has a working default or a filesystem
convention behind it, so a project with no `cpptb.toml` at all still builds —
the file exists to make a project's choices persistent and visible.
Command-line options override it for one invocation; see
[cpptb command line](cli.md).

A complete file, with every section present:

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
timing_backend = "verilator-direct"
deferred_writes = true
optimization = "-O2"
cxx_flags = ["-Wall"]
verilator_args = ["-Wno-UNOPTFLAT"]

[run]
timeout_cycles = 1000000
```

## [design]

| Key | Type | Default | Effect |
|---|---|---|---|
| `sources` | list of paths/globs | `rtl/**/*.sv`, `rtl/**/*.v`, or root-level `.sv`/`.v` files | RTL inputs, expanded in listed order with each pattern sorted deterministically |
| `top` | string | inferred | The DUT top module; required only when elaboration cannot choose one root unambiguously |
| `include_dirs` | list of paths | none | SystemVerilog include search directories |
| `defines` | list of strings | none | Preprocessor defines for elaboration, as `NAME` or `NAME=VALUE` |
| `parameters` | table of name → string or integer | none | Elaboration parameters for the top module; also writable as a `[design.parameters]` table |

A parameter table entry may be a string or an integer. String values reach
elaboration verbatim, which is how string-typed SystemVerilog parameters
(file paths, mode names) are passed.

## [testbench]

| Key | Type | Default | Effect |
|---|---|---|---|
| `sources` | list of paths/globs | `tests/**/*.cpp` or root-level `testbench.cpp` | C++ testbench translation units; each is compiled and linked into the simulator |
| `include_dirs` | list of paths | none | Extra include directories for testbench compilation |

## [build]

| Key | Type | Default | Effect |
|---|---|---|---|
| `directory` | path | `"build"` | Artifact root, relative to the project unless absolute |
| `target` | string | the top module name | Name of the generated simulator target under `build/cpptb/` |
| `simulator` | string | `"verilator"` | Simulator backend; currently Verilator |
| `timing_backend` | `"verilator-direct"` or `"vpi"` | `"verilator-direct"` | How the simulator delivers the `ReadWrite{}`, `ReadOnly{}`, and `NextTimeStep{}` phase waits. Only these two names are accepted; there is no supported build without a backend |
| `deferred_writes` | bool | `true` | `true` is cocotb's write model: `set()` queues and flushes at the ReadWrite point, so a write after an awaited edge lands on the next one. `false` restores legacy immediate writes and is on a deprecation path |
| `optimization` | string | `"-O2"` | Optimization level applied to both the testbench and the Verilated model. Without it, Verilator's own default optimizes the model at `-Os` and leaves testbench sources unoptimized. Lower to `-O0` for a debug build |
| `cxx_flags` | list of strings | none | Extra flags for testbench compilation, after the defaults so an explicit flag wins |
| `verilator_args` | list of strings | none | Extra arguments passed to Verilator, e.g. lint waivers. Timing defines and a bare `--vpi` are rejected here — the timing keys own them |
| `wave` | `true`, `"fst"`, or `"vcd"` | off | Build with waveform tracing and dump one wave file per test; `true` means FST. See [Waveforms](waveforms.md) |
| `experimental_four_state` | bool | `false` | Request four-state mode behind a Verilator semantic capability probe; currently upstream-blocked. Do not add `--fourstate` to `verilator_args` — that bypass is rejected. See [Four-state values](four-state.md) |

`timing_backend` and `deferred_writes` carry cpptb's timing semantics and
travel together: a queued write is applied at a simulator phase, so the
write model needs a backend. The two backends are held to identical results
and byte-identical waveforms on every run, so the choice is about speed
(`verilator-direct`) versus portability (`vpi`), never semantics. The pinned
behavior lives in [The write model](scheduling.md#the-write-model) and
[Timing backend support](scheduling.md#timing-backend-support).

Changing any `[build]` key re-fingerprints the build: the next `cpptb build`
recompiles cleanly instead of reusing objects compiled under the previous
settings.

## [run]

| Key | Type | Default | Effect |
|---|---|---|---|
| `timeout_cycles` | integer | `1000000` | Simulation-cycle watchdog: a test still running after this many cycles ends with a specific timeout diagnostic instead of hanging the run |

The watchdog counts simulation cycles inside the simulator. The wall-clock
limit on the whole OS process is a harness concern —
`cpptb test --timeout SECONDS` on the [command line](cli.md#per-command-options).

## Validation

Unknown values fail at resolve time with the key named, before anything
compiles: `timing_backend` accepts exactly its two names, `wave` its two
formats, and hand-set timing defines in `cxx_flags` or `verilator_args` are
rejected in favor of the keys that own them. Missing sources, tests, and
include directories produce project-level diagnostics rather than compiler
errors.
