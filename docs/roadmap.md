# Roadmap

cpptb already provides the low-level foundation needed to write concurrent
testbenches: typed DUT and hierarchy access, arbitrary-clock edge waits,
physical delays, simulator-phase waits, typed tasks, process control, events,
queues, timeouts, wide and fixed-point values, multidimensional arrays,
packed views, bounded queues, locks, semaphores, and force/release. Every
completed runtime feature has an exact SystemVerilog workload in the authoring
benchmark suite.

The next milestones focus on verification productivity rather than adding more
ways to suspend a coroutine. The goal is to make tests easy to organize,
reproduce, debug, and reuse while keeping DUT reads, writes, and time
advancement explicit.

The API reuses familiar cocotb names where behavior matches. Signal access
remains explicit C++ (`get()`, `set()`, `deposit()`, `force()`, and
`release()`), and physical time remains an explicit `Delay`.

## Framework scope

This roadmap now separates the reusable framework from the optional reference
harness. Framework milestones cover C++ APIs and semantics that remain useful
when cpptb is embedded in another build, regression, or CI system:

- scheduler, DUT access, test lifecycle, result, and verification-component
  APIs;
- generated typed bindings and simulator transport contracts; and
- metadata and callbacks that any external harness can consume.

The public `cpptb` command and lower-level `cpptb-run` are a maintained
reference harness. Future CLI filtering policy, process orchestration, JUnit
conversion, waveform reruns, wall-time limits, reproduction-command
presentation, and build diagnostics are harness work. They are not framework
milestone blockers. The current development priority is the framework.

## Status summary

| # | Milestone | Status | Outcome |
|---:|---|---|---|
| - | [Foundation](#foundation) | <strong class="roadmap-status roadmap-status--done">Done</strong> | Typed DUT access, timing, concurrency, hierarchy, data types, and simulator backends |
| 1 | [Framework test lifecycle and structured results](#1-framework-test-lifecycle-and-structured-results) | <strong class="roadmap-status roadmap-status--done">Done</strong> | Owned test processes, terminal states, checks, and harness-neutral diagnostics |
| 2 | [Bounded queues and synchronization](#2-bounded-queues-and-synchronization) | <strong class="roadmap-status roadmap-status--done">Done</strong> | Bounded FIFO communication, locks, semaphores, and cancellation-safe handoff |
| 3 | [Reusable verification components](#3-reusable-verification-components) | <span class="roadmap-status roadmap-status--next">In progress</span> | Typed endpoints, drivers, monitors, scoreboards, and analysis fan-out |
| 4 | [Random stimulus and functional coverage](#4-reproducible-random-stimulus-and-functional-coverage) | <span class="roadmap-status roadmap-status--planned">Planned</span> | Reproducible random streams and mergeable functional coverage |
| 5 | [Register abstraction](#5-register-abstraction) | <span class="roadmap-status roadmap-status--planned">Planned</span> | Typed register models with explicit frontdoor and backdoor adapters |
| 6 | [Interfaces and simulator portability](#6-interfaces-bidirectional-signals-and-portability) | <span class="roadmap-status roadmap-status--planned">Planned</span> | Interfaces, resolved signals, four-state values, and another simulator |
| 7 | [Debugging and release tooling](#7-debugging-and-release-tooling) | <span class="roadmap-status roadmap-status--planned">Planned</span> | Logging, waveforms, wait diagnostics, and distributable packages |
| 8 | [Coherent clock and reset control](#8-coherent-clock-and-reset-control) | <span class="roadmap-status roadmap-status--planned">Planned</span> | Runtime clock ownership and explicit reusable reset components |

Select a milestone to jump to its detailed scope below.

## Foundation

**Status:** <strong class="roadmap-status roadmap-status--done">Done</strong>

The simulator timing model is implemented. `ReadWrite{}`, `ReadOnly{}`, and
`NextTimeStep{}` are backed by the direct Verilator timing backend and the
standard VPI fallback. The scheduler enforces legal phase transitions and
rejects writes during `ReadOnly` with an actionable diagnostic. The generated
SV-DPI event calendar remains an experimental backend with a deliberately
documented event-discovery limitation.

The repository also has an internal regression orchestrator and a reference
command-line harness. Those tools validate and demonstrate the framework, but
their future feature set is tracked separately from framework completion.

## 1. Framework test lifecycle and structured results

**Status:** <strong class="roadmap-status roadmap-status--done">Done</strong>

The framework lifecycle milestone includes:

- multiple registered tests with catalog listing and exact per-invocation
  selection through embeddable C++ APIs;
- nonfatal `expect()` and `expect_eq()` checks plus fatal `require()` and
  `require_eq()` checks;
- structured warnings plus test-owned process cleanup, stable process IDs,
  spawn-site provenance, and exception attribution;
- failure-only diagnostic formatting for integers, symbolic enums, `Bits`,
  `LogicBits`, ranges, and generated packed views, with a custom formatter
  extension point;
- skip, expected-failure, unexpected-pass, and simulation-time-timeout
  outcomes with explicit success rules;
- discoverable tags and typed parameterized cases with stable catalog names;
- versioned JSON results and optional in-process `ResultSink` callbacks; and
- an exact C++/pure-SV `test_lifecycle` performance pair under the standard
  `1.10x` hard guard.

The API, semantic coverage, and performance qualification are implemented.
The benchmark suite distinguishes persistent verification processes from
repeated dynamic process creation. The realistic persistent-monitor workload
passes the standard hard guard; repeated zero-time child creation remains a
separately documented scheduler cost rather than a lifecycle blocker.

See [Framework test lifecycle](test-lifecycle.md) for the API, terminal-state
rules, metadata model, and formatter extension point. Diagnostic strings are
constructed only on failed equality checks; the passing-check hot path remains
free of diagnostic allocation.

The reference harness already demonstrates catalog discovery, exact selection,
fresh-process execution, result files, and cached builds. CLI tag filtering,
JUnit conversion, waveform-on-failure reruns, wall-time policy, reproduction
commands, and presentation of build or infrastructure failures are parked
harness enhancements. Deterministic random streams remain in milestone 4;
only their result metadata belongs in the lifecycle model.

## 2. Bounded queues and synchronization

**Status:** <strong class="roadmap-status roadmap-status--done">Done</strong>

`Queue<T>` provides optional capacity and producer backpressure as the single
public FIFO communication primitive. The implemented API includes:

- optional capacity and producer backpressure;
- `get_nowait()`, `put_nowait()`, `empty()`, `full()`, and `size()`;
- cancellation-safe FIFO waiter ordering;
- `Lock` and `Semaphore`; and
- cancellation-safe reserved-item, reserved-slot, lock-handoff, and permit
  recovery.

Idle synchronization objects do not register callbacks or add
simulator-boundary traffic. `Barrier` remains deferred until a real
multi-process example demonstrates semantics that are not already covered by
`Join`, `Event`, or `Semaphore`.

## 3. Reusable verification components

**Status:** <span class="roadmap-status roadmap-status--next">In progress</span>

The first lightweight transaction-component slice is implemented:

- `PutPort<T>` producer endpoints and `GetPort<T>` consumer endpoints;
- connectable queue-backed implementations without requiring components to
  depend on a concrete `Queue<T>`;
- `AnalysisPort<T>` for nonblocking fan-out from a passive monitor to multiple
  subscribers, plus an explicit queue adapter for subscribers that need
  asynchronous consumption;
- `InOrderScoreboard<T>` for immediate transaction pairing and structured
  nonfatal mismatch reporting; and
- ready/valid driver and monitor helpers with explicit sample edge and delay.

The runnable [component FIFO example](examples/component-fifo.md) composes
these pieces without replacing the lower-level `fifo_scoreboard` example. An
exact `analysis_fanout` C++/pure-SV benchmark covers every publication and
delivery and remains below the repository's `1.10x` performance guard.

Remaining scope includes transaction producers and consumers, reference-model
adapters, and reusable active or passive protocol components. Start with APB,
then add AXI-Lite, streaming, UART, or SPI only when backed by complete
runnable examples.

These remain ordinary C++ objects and coroutines, not a second scheduler or a
mandatory component hierarchy.

The endpoint layer should separate connectivity from storage in the useful
part of the UVM TLM model. A producer calls a put interface, a consumer calls a
get interface, and a monitor publishes through an analysis interface. A
`Queue<T>` may implement the put/get pair, but components should be testable
with another transport. Analysis publication must remain nonblocking; a
subscriber that needs asynchronous consumption connects through a buffered
adapter rather than silently delaying every other subscriber.

Transaction methods may encapsulate a protocol operation, but the component
implementation must show every signal write, edge wait, sampling phase, and
delay. The framework must not insert implicit resets, waits, or signal writes.

## 4. Reproducible random stimulus and functional coverage

**Status:** <span class="roadmap-status roadmap-status--planned">Planned</span>

Add a test-owned random service with a recorded master seed and independently
reproducible process streams. Begin with uniform ranges, weighted choices,
shuffling, and user-defined generators before considering a constraint solver.

Functional coverage should support coverpoints, bins, illegal bins, crosses,
and transition coverage. Results should be mergeable across seeds. A stable
JSON representation is sufficient initially; UCIS interoperability can follow
after the data model has been exercised by real tests.

Random stimulus and coverage must remain separate facilities: users should be
able to use deterministic directed stimulus with coverage, or random stimulus
without adopting a coverage model.

## 5. Register abstraction

**Status:** <span class="roadmap-status roadmap-status--planned">Planned</span>

Build a typed register model for register-heavy peripherals and SoCs:

- registers, fields, memories, access policies, and reset values;
- frontdoor operations through a user-supplied bus adapter;
- backdoor access through generated hierarchy paths;
- mirrored values, prediction, volatile fields, and read/write checking; and
- generation from SystemRDL, IP-XACT, or RgGen metadata.

The register layer should not own simulation time. A frontdoor adapter exposes
ordinary coroutine transactions, and a backdoor adapter performs explicit
hierarchical operations. This keeps the register model reusable across APB,
AXI-Lite, and custom buses.

## 6. Interfaces, bidirectional signals, and portability

**Status:** <span class="roadmap-status roadmap-status--planned">Planned</span>

Extend generated typed access to SystemVerilog interfaces, modports, unpacked
interface arrays, `inout` ports, and resolved tri-state nets. Preserve natural
hierarchical access and four-state values without requiring user-authored
binding files.

Run the complete generated transport and scheduling conformance suite on at
least one additional standards-compliant simulator. A four-state simulator is
required to validate X/Z propagation end to end; Verilator remains the
two-state performance reference. Simulator capability differences should be
reported at startup rather than hidden in testbench code.

## 7. Debugging and release tooling

**Status:** <span class="roadmap-status roadmap-status--planned">Planned</span>

Add the facilities needed to diagnose and distribute real regressions:

- structured logging with simulation timestamps and component scopes;
- runtime waveform start/stop and scope selection;
- waveform-on-failure support in the test runner;
- a scheduler wait graph for deadlock and timeout reports;
- clean attribution of HDL assertion failures;
- simulator and generated-binding compatibility reports; and
- versioned runtime and code-generator packages.

Keep the core runtime header-only and the Slang-backed generator separately
installable.

## 8. Coherent clock and reset control

**Status:** <span class="roadmap-status roadmap-status--planned">Planned</span>

Extend clock ownership beyond initial registration. A returned clock handle
should support stop, restart, pause, period or duty-cycle changes, and explicit
release of ownership while remaining coherent with pending edge waits. The
model must scale to unrelated input clocks and DUT-generated clocks.

Reusable reset components may be built on these primitives, but their APIs
must expose each asserted value and each wait. They must not silently drive a
reset sequence merely because a test starts.

## Deliberate non-goals

cpptb should not copy the complete UVM architecture. In particular, a factory,
global configuration database, mandatory runtime phases, and objection system
would add substantial machinery without improving the current testbench model.
The useful ideas to retain are typed transactions and clear separation between
sequences, drivers, monitors, and scoreboards.

The optimized DPI path should also retain generated typed hierarchy access
rather than adding runtime string-based signal lookup as the default. Dynamic
lookup can remain a future portability or debugging facility if a concrete use
case requires it.

The naming and capability comparisons are informed by cocotb's official
[library reference](https://docs.cocotb.org/en/stable/library_reference.html),
[coroutine guide](https://docs.cocotb.org/en/stable/coroutines.html), and
[timing model](https://docs.cocotb.org/en/stable/timing_model.html). The
transaction-component terminology follows the reusable portions of the
[Accellera UVM User's Guide](https://www.accellera.org/images/downloads/standards/uvm/uvm_users_guide_1.2.pdf)
without adopting its full infrastructure.

## Delivery process

For every runtime feature:

1. Add unit, negative, conformance, cancellation, and usage tests as applicable.
2. Add an individually runnable C++ DPI workload and exact SystemVerilog twin.
3. Run semantic parity before performance measurement.
4. Benchmark serially and stop on a valid C++ DPI/SystemVerilog ratio above
   `1.10x` unless a scoped waiver is reviewed and recorded.
5. Run the full affected regression and retain a compact reviewed baseline.
6. Update the user guide, runnable examples, and roadmap status in the same
   change.
