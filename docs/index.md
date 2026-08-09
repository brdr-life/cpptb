# cpptb documentation

cpptb is an experimental C++20 coroutine testbench framework for
SystemVerilog simulators. It generates a typed DUT interface and a standard
DPI wrapper so testbench code can drive signals and wait for simulator events
without exposing scheduler or transport plumbing.

```cpp
Task<void> count_sequence(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
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

CPPTB_REGISTER_TEST(count_sequence);
```

## Roadmap status

| Milestone | Status |
|---|---|
| [Framework test lifecycle and structured results](roadmap.md#1-framework-test-lifecycle-and-structured-results) | <strong class="roadmap-status roadmap-status--done">Done</strong> |
| [Bounded queues and synchronization](roadmap.md#2-bounded-queues-and-synchronization) | <strong class="roadmap-status roadmap-status--done">Done</strong> |
| [Reusable verification components](roadmap.md#3-reusable-verification-components) | <strong class="roadmap-status roadmap-status--done">Done</strong> |
| [Random stimulus and functional coverage](roadmap.md#4-reproducible-random-stimulus-and-functional-coverage) | <strong class="roadmap-status roadmap-status--done">Done</strong> |
| [Memory and register verification components](roadmap.md#5-memory-and-register-verification-components) | <strong class="roadmap-status roadmap-status--done">Done</strong> |
| [Interfaces and simulator portability](roadmap.md#6-interfaces-bidirectional-signals-and-portability) | <span class="roadmap-status roadmap-status--next">In progress</span> |
| [Debugging and release tooling](roadmap.md#7-debugging-and-release-tooling) | <span class="roadmap-status roadmap-status--next">In progress</span> |
| [Coherent clock and reset control](roadmap.md#8-coherent-clock-and-reset-control) | <span class="roadmap-status roadmap-status--planned">Planned</span> |
| [Batched execution and run-ahead experiments](roadmap.md#9-batched-execution-and-run-ahead-experiments) | <span class="roadmap-status roadmap-status--planned">Planned</span> |

See the [complete roadmap](roadmap.md) for scope, design constraints, and the
delivery process behind each milestone.

## Start here

- [Getting started](getting-started.md) covers prerequisites, generation, and
  the shape of a complete testbench.
- [Testbench authoring](testbench-authoring.md) is the main guide to tasks,
  concurrency, timeouts, events, queues, and process control.
- [Framework test lifecycle](test-lifecycle.md) covers registration, checks,
  process ownership, terminal states, and structured results.
- [Structured logging](logging.md) covers scoped severity levels, lazy message
  construction, ordered histories, custom sinks, and automatic process
  attribution.
- [Verification components](verification-components.md) covers the optional
  `cpptb_vc` package, transaction interfaces, scoreboards, streams, APB,
  [sparse expected memory](verification-components/memory-model.md), and the
  [register abstraction layer](memory-register-models.md). The
  [generation guide](verification-components/register-generation.md) covers
  PeakRDL generation from SystemRDL or IP-XACT.
- [Randomization](random-stimulus.md) provides a multi-page guide to direct
  generators, constrained transactions, composite fields, solver backends,
  replay, [functional coverage](randomization/functional-coverage.md), and
  [side-by-side framework examples](randomization/examples.md).
- [Running tests](running-tests.md) covers the optional command-line build and
  execution harness.
- [Hierarchical DUT access](hierarchy.md) covers natural instance paths,
  memories, packed types, backdoor operations, and hierarchical triggers.
- [SystemVerilog interfaces and bidirectional signals](interfaces.md) covers
  modports, interface arrays, named directions, clocks, and inout drive intent.
- [Examples](examples.md) maps common DUT styles to runnable C++ DPI and pure
  SystemVerilog pairs.
- [Open-source core benchmarks](examples/open-source-cores.md) show the same
  testbench work against PicoRV32, AES-128, and 64-bit Ethernet FCS RTL in
  pure SV, C++ DPI, C++ VPI, and Cocotb.
- [secworks AES register-model oracle](examples/secworks-aes-regmodel.md)
  proves a generated SystemRDL model against the unchanged upstream top-level
  bench and provides a scalable matched pure-SV performance peer.

## Understand the framework

- [Clocking](clocking.md) explains C++ clock registration, simulator-owned
  waveforms, multiple domains, and scheduler callbacks.
- [Scheduling](scheduling.md) defines edge, delay, ordering, and composition
  semantics.
- [Waveforms](waveforms.md) covers the `--wave` build variant, per-test dump
  files, and the pure-SV wave-equivalence flow.
- [Code generation](code-generation.md) explains typed DUT and DPI wrapper
  generation.
- [Architecture](architecture.md) describes the runtime and simulator
  boundary.
- [Performance](performance.md) documents the apples-to-apples benchmark
  contract, regression guard, and measured open-source-core comparison.

## Current capabilities

The current implementation supports concurrent processes, edge and delay
triggers, typed tasks, timeouts, events, bounded queues, locks, semaphores,
typed transaction endpoints, analysis fan-out, in-order and keyed scoreboards,
ready/valid and APB components, wide packed and fixed-point values,
multidimensional arrays, and deterministic and constrained-random stimulus
with membership,
distributions, soft constraints, adaptive optional solver fallback, composite
fields, functional coverpoints, crosses, transitions, and source-inferred
hierarchical access. Two timing backends carry the full phase contract --
`verilator-direct` for speed and `vpi` for portability -- and
`deferred_writes = true` selects cocotb's write model: a `set()` right
after an awaited edge applies after that edge's own updates. Compiled test
catalogs, one-test-per-run selection, test-owned process cleanup, fatal and
nonfatal checks, process-aware structured logging, per-test waveform dumps
(`--wave`, FST or VCD), and JSON results are also supported. Optional
`cpptb_vc` components add sparse expected memory, typed register models,
register-access coverage, and PeakRDL generation without extending the
core scheduler API.
Verilator is the end-to-end reference simulator. Direct Verilator timing
dispatch and the standard VPI fallback implement the complete scheduling
contract; the faster generated SV-DPI calendar remains experimental. Broader
simulator portability remains active work in the [roadmap](roadmap.md).
