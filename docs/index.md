# cpptb documentation

cpptb is an experimental C++20 coroutine testbench framework for
SystemVerilog simulators. It generates a typed DUT interface and a standard
DPI wrapper so testbench code can drive signals and wait for simulator events
without exposing scheduler or transport plumbing.

```cpp
Task<void> count_sequence(Dut dut, TestContext& test) {
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

CPPTB_REGISTER_TEST(count_sequence);
```

## Roadmap status

| Milestone | Status |
|---|---|
| [Framework test lifecycle and structured results](roadmap.md#1-framework-test-lifecycle-and-structured-results) | <span class="roadmap-status roadmap-status--next">In progress</span> |
| [Bounded queues and synchronization](roadmap.md#2-bounded-queues-and-synchronization) | <strong class="roadmap-status roadmap-status--done">Done</strong> |
| [Reusable verification components](roadmap.md#3-reusable-verification-components) | <span class="roadmap-status roadmap-status--planned">Planned</span> |
| [Random stimulus and functional coverage](roadmap.md#4-reproducible-random-stimulus-and-functional-coverage) | <span class="roadmap-status roadmap-status--planned">Planned</span> |
| [Register abstraction](roadmap.md#5-register-abstraction) | <span class="roadmap-status roadmap-status--planned">Planned</span> |
| [Interfaces and simulator portability](roadmap.md#6-interfaces-bidirectional-signals-and-portability) | <span class="roadmap-status roadmap-status--planned">Planned</span> |
| [Debugging and release tooling](roadmap.md#7-debugging-and-release-tooling) | <span class="roadmap-status roadmap-status--planned">Planned</span> |
| [Coherent clock and reset control](roadmap.md#8-coherent-clock-and-reset-control) | <span class="roadmap-status roadmap-status--planned">Planned</span> |

See the [complete roadmap](roadmap.md) for scope, design constraints, and the
delivery process behind each milestone.

## Start here

- [Getting started](getting-started.md) covers prerequisites, generation, and
  the shape of a complete testbench.
- [Testbench authoring](testbench-authoring.md) is the main guide to tasks,
  concurrency, timeouts, events, queues, and process control.
- [Framework test lifecycle](test-lifecycle.md) covers registration, checks,
  process ownership, terminal states, and structured results.
- [Running tests](running-tests.md) covers the optional command-line build and
  execution harness.
- [Hierarchical DUT access](hierarchy.md) covers natural instance paths,
  memories, packed types, backdoor operations, and hierarchical triggers.
- [Examples](examples.md) maps common DUT styles to runnable C++ DPI and pure
  SystemVerilog pairs.
- [Open-source core benchmarks](examples/open-source-cores.md) show the same
  testbench work against PicoRV32, AES-128, and 64-bit Ethernet FCS RTL in
  pure SV, C++ DPI, C++ VPI, and Cocotb.

## Understand the framework

- [Clocking](clocking.md) explains C++ clock registration, simulator-owned
  waveforms, multiple domains, and scheduler callbacks.
- [Scheduling](scheduling.md) defines edge, delay, ordering, and composition
  semantics.
- [Code generation](code-generation.md) explains typed DUT and DPI wrapper
  generation.
- [Architecture](architecture.md) describes the runtime and simulator
  boundary.
- [Performance](performance.md) documents the apples-to-apples benchmark
  contract, regression guard, and measured open-source-core comparison.

## Current capabilities

The current implementation supports concurrent processes, edge and delay
triggers, typed tasks, timeouts, events, bounded queues, locks, semaphores,
wide packed and fixed-point values, multidimensional arrays, and
source-inferred hierarchical access. Compiled test catalogs, one-test-per-run
selection, test-owned process cleanup, fatal and nonfatal checks, and JSON
results are also supported.
Verilator is the end-to-end reference simulator. Direct Verilator timing
dispatch and the standard VPI fallback implement the complete scheduling
contract; the faster generated SV-DPI calendar remains experimental. Broader
simulator portability remains active work in the [roadmap](roadmap.md).
