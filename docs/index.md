# cpptb

**Write SystemVerilog testbenches as C++20 coroutines.**

cpptb reads your RTL, generates a typed `Dut` you drive with ordinary C++, and
runs your test inside the simulator. Signal reads and writes stay explicit, and
every point where simulation time advances is a visible `co_await`:

```cpp
Task<void> counter_sequence(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
    test.start_clock(dut.clk, 10_ns);   // the testbench owns clock timing

    dut.rst_n.set(0);
    dut.enable.set(0);
    co_await clock_cycles(dut.clk, 2);

    dut.rst_n.set(1);
    dut.enable.set(1);

    co_await RisingEdge{dut.clk};
    co_await ReadOnly{};                // let the design settle, then check
    test.expect_eq("enabled count", dut.count.get(), 1u);
}

CPPTB_REGISTER_TEST(counter_sequence);
```

There is no manifest to write, no hand-authored DPI wrapper, and no Makefile to
maintain. Point the `cpptb` command at your RTL and a `testbench.cpp`, and it
derives the rest.

## Start here

| If you… | Read this |
|---|---|
| are new to cpptb | [Getting started](getting-started.md) — install, run a real test, then make it fail |
| want the mental model | [Core ideas](core-ideas.md) — how a cpptb test is put together, in ten minutes |
| already write cocotb | [Coming from cocotb](coming-from-cocotb.md) — the trigger map, the write model, and three translation traps |
| want working code to copy | [Examples](examples.md) — every one runnable, each with a pure-SystemVerilog twin |
| want the cheat sheet | [Reference card](refcard.md) — every common operation, one line each |
| are looking up an API | [Library reference](library/awaitables.md) — signatures and semantics, one page per API family |
| need the exact option or key | [cpptb command line](cli.md) and [cpptb.toml](cpptb-toml.md) — the command and configuration references |
| hit an error message | [Troubleshooting](troubleshooting.md) — the diagnostics in one place, each with its fix |
| meet an unfamiliar term | [Glossary](glossary.md) — the vocabulary the rest of the docs uses |

## What you get

- **Typed access to the whole design.** Ports, packed and fixed-point values,
  arrays, structs, enums, and the full internal
  [hierarchy](hierarchy.md) — inferred from RTL, with no probe list to
  maintain. Misspell a signal and the compiler says so.
- **Explicit, readable time.** `get()` and `set()` never move the clock; only
  `co_await` does. [Scheduling](scheduling.md) defines edge, delay, and
  ordering semantics precisely.
- **Real concurrency primitives.** `spawn`, `Join`, `First`, events, bounded
  typed queues, locks, semaphores, timeouts, and test-owned process cleanup.
- **Verification components when you want them.** The optional
  [`cpptb_vc`](verification-components.md) layer adds transaction endpoints,
  scoreboards, APB components, [sparse expected memory](verification-components/memory-model.md),
  and [typed register models](memory-register-models.md) generated from
  SystemRDL or IP-XACT.
- **Randomization that replays.** Deterministic per-process streams,
  [constrained transactions](randomization/constrained-transactions.md),
  [functional coverage](randomization/functional-coverage.md), and
  [replay from a recorded seed](randomization/reproducibility.md).
- **Performance you can check.** Every authoring feature has an equivalent
  pure-SystemVerilog testbench, held to a hard ratio guard on every run. See
  [Performance](performance.md) for the measurements and the benchmark
  contract.

## Known limits

Stated here rather than discovered the hard way:

- **Verilator is the reference simulator.** Two timing backends carry the full
  phase contract — `verilator-direct` for speed and `vpi` for portability —
  and are held to identical results on every run. Broader simulator support is
  active work.
- **A timing backend is always in play.** `timing_backend` defaults to
  `"verilator-direct"` (`"vpi"` is the portable alternative) and supplies the
  phase waits `ReadWrite{}`, `ReadOnly{}`, and `NextTimeStep{}` — the hooks
  that let a testbench act at any point of a timestep. There is no supported
  build without one. See
  [Timing backend support](scheduling.md#timing-backend-support).
- **Four-state simulation is not available** on the Verilator backend today.
  [Four-state values](four-state.md) explains what does and does not work.

## Project status

cpptb is experimental and under active development. The
[roadmap](roadmap.md) tracks every milestone with its current status, scope,
and design constraints; [future directions](future-directions.md) collects
unscheduled ideas.

The framework is validated against real, third-party verification
environments, not only its own examples: Ibex's core-level testlist runs on
cpptb with the identical 912-of-944 outcome as the upstream UVM environment
and 871,825 instructions co-simulated against Spike. See
[Ports of real testbenches](open-core-ports.md).
