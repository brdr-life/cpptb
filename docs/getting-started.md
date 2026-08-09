# Getting started

In about five minutes you will run a real cpptb test against real RTL, read the
testbench that drives it, and break it on purpose to see what a failure looks
like. Everything here uses `examples/counter`, the smallest complete cpptb
project.

## Prerequisites

Install Verilator, a C++20 compiler, CMake, Python 3.11 or newer, and
[`uv`](https://docs.astral.sh/uv/). Then, from a checkout of this repository:

```sh
uv sync --frozen
```

## 1. Run your first test

```sh
uv run --frozen cpptb test --project examples/counter --build-dir build
```

The first run generates the typed DUT, compiles the simulator, and runs every
registered test. It takes a minute or two; later runs reuse the cached build.

```text
cpptb: counter built: .../build/cpptb/counter/obj/Vdpi_counter
PASS  counter_sequence checks=9 seed=1 wall_ms=0.062
PASS  counter_reset_defaults checks=1 seed=1 wall_ms=0.025
cpptb: 2 tests: 2 passed, 0 failed, 0 errors
```

Two tests, ten checks, no configuration written by hand. `cpptb list` shows the
catalog without running it:

```text
counter_sequence
counter_reset_defaults
```

Each test runs in its own fresh simulator process, so no test can inherit
another's DUT state.

## 2. Read the testbench

The design under test is an ordinary 8-bit counter — `examples/counter/counter.sv`:

```systemverilog
module counter (
    input  logic       clk,
    input  logic       rst_n,
    input  logic       enable,
    output logic [7:0] count
);
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) count <= '0;
    else if (enable) count <= count + 1'b1;
  end
endmodule
```

You never write a wrapper, a probe list, or a DPI manifest for it. `cpptb`
elaborates the RTL and generates a typed `Dut` whose members are the ports —
so `dut.count` exists, is 8 bits wide, and a typo is a compile error.

Here is the heart of `examples/counter/testbench.cpp`:

```cpp
Task<void> counter_sequence(Dut dut, TestContext& test) {
    dut.clk.set_now(0);                  // seed the pin before the clock starts
    test.start_clock(dut.clk, 10_ns);    // the testbench owns clock timing

    dut.rst_n.set(0);
    dut.enable.set(0);
    co_await clock_cycles(dut.clk, 2);   // hold reset for two cycles

    dut.rst_n.set(1);
    dut.enable.set(1);

    for (uint32_t expected = 1; expected <= kCountCycles; ++expected) {
        co_await RisingEdge{dut.clk};    // advance to the edge
        co_await ReadOnly{};             // let the design settle, then sample
        test.expect_eq("enabled count", dut.count.get(), expected);
    }
}

CPPTB_REGISTER_TEST(counter_sequence);
```

Four ideas carry most of cpptb:

- **`CPPTB_REGISTER_TEST` makes a coroutine a test.** The registered name is the
  function name, and it is what `cpptb list` prints.
- **Signal access is explicit and never moves time.** `set()` and `get()` are
  ordinary calls. Only `co_await` advances the simulation, so you can always
  see where time passes.
- **The testbench owns the clock.** `start_clock()` registers a period; it does
  not suspend. Call it once per input clock before the first `co_await`.
- **Checks are values, not exceptions.** `expect_eq` records a failure and
  keeps going; `require_eq` ends the test at once.

:::{note}
Those two lines are the standard cpptb project setup, and this example's
`cpptb.toml` has them:

```toml
[build]
timing_backend = "verilator-direct"   # or "vpi"
deferred_writes = true
```

Both are also the defaults, so a new project gets them without a `cpptb.toml`
at all; the examples state them so the configuration is visible.
`timing_backend` supplies the phase waits — it is what makes
`co_await ReadOnly{}` work — and `deferred_writes = true` selects cocotb's
write model, so a `set()` right after an awaited edge lands for the *next*
edge rather than the one just awaited. See
[The write model](scheduling.md#the-write-model).
:::

## 3. Break it on purpose

A test you have never seen fail is a test you should not trust yet. In
`examples/counter/testbench.cpp`, find the `expect_eq` inside the counting
loop and ask for a count that is one too high:

```cpp
test.expect_eq("enabled count", dut.count.get(), expected + 1);
```

Run it again. cpptb rebuilds automatically because the source changed:

```text
FAIL  counter_sequence checks=9 seed=1 wall_ms=0.572
PASS  counter_reset_defaults checks=1 seed=1 wall_ms=0.029
cpptb: 2 tests: 1 passed, 1 failed, 0 errors
```

The console stays compact; the diagnostics land in the per-test log under
`build/cpptb/counter/results/`:

```text
cpptb: examples/counter/testbench.cpp:35: enabled count: actual=1 expected=2
cpptb: examples/counter/testbench.cpp:35: enabled count: actual=2 expected=3
cpptb: examples/counter/testbench.cpp:35: enabled count: actual=3 expected=4
...
```

Every failure carries its source location, its label, and both values. Because
`expect_eq` is nonfatal, all eight mismatches are reported in one run instead of
only the first. The same records are available as structured JSON next to the
log, in `counter_sequence.json` — that file is the contract a CI system or a
custom harness consumes.

Undo the change before moving on.

## 4. Start your own project

A cpptb project needs two files you write yourself:

```text
my-project/
├── design.sv        # your RTL
└── testbench.cpp    # your registered coroutines
```

Everything generated goes under an ignored `build/` directory. From an
installed cpptb package, run these in the project directory:

```sh
cpptb build     # generate the typed DUT and compile the simulator
cpptb list      # show the registered tests
cpptb test      # run them all
cpptb test my_test_name    # run just one
```

From a checkout of this repository, prefix each with `uv run --frozen`.

Discovery is zero-configuration: cpptb accepts either root-level `.sv`/`.v`
files plus `testbench.cpp`, or RTL under `rtl/` with C++ under `tests/`. Add
`--top module_name` if more than one module could be the root. Copy
`examples/counter` as a starting skeleton, or `examples/apb_regfile` for a
transaction-based bench.

## Next steps

| To learn | Read |
|---|---|
| The model behind every cpptb test | [Core ideas](core-ideas.md) |
| Concurrency, timeouts, events, queues, process control | [Tasks and concurrency](testbench-authoring.md) |
| Exactly when each `co_await` resumes | [Scheduling](scheduling.md) |
| The test workflow and structured results | [Running tests](running-tests.md) |
| Every command-line option and `cpptb.toml` key | [cpptb command line](cli.md) · [cpptb.toml](cpptb-toml.md) |
| Reading and driving internal signals, not just ports | [Hierarchical DUT access](hierarchy.md) |
| Working code for a DUT shaped like yours | [Examples](examples.md) |

Already write cocotb testbenches? [Coming from cocotb](coming-from-cocotb.md)
maps the trigger vocabulary and covers three translation traps worth knowing
before you convert anything.
