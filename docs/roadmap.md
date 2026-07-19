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
| 3 | [Reusable verification components](#3-reusable-verification-components) | <strong class="roadmap-status roadmap-status--done">Done</strong> | Package-ready bus and stream interfaces, APB components, monitors, checkers, and scoreboards |
| 4 | [Random stimulus and functional coverage](#4-reproducible-random-stimulus-and-functional-coverage) | <strong class="roadmap-status roadmap-status--done">Done</strong> | Reproducible random streams, adaptive solving, and mergeable functional coverage |
| 5 | [Memory and register verification components](#5-memory-and-register-verification-components) | <strong class="roadmap-status roadmap-status--done">Done</strong> | Optional protocol-independent memory and typed register models with frontdoor and backdoor adapters |
| 6 | [Interfaces and simulator portability](#6-interfaces-bidirectional-signals-and-portability) | <span class="roadmap-status roadmap-status--planned">Planned</span> | Interfaces, resolved signals, four-state values, and another simulator |
| 7 | [Debugging and release tooling](#7-debugging-and-release-tooling) | <span class="roadmap-status roadmap-status--planned">Planned</span> | Logging, waveforms, wait diagnostics, and distributable packages |
| 8 | [Coherent clock and reset control](#8-coherent-clock-and-reset-control) | <span class="roadmap-status roadmap-status--planned">Planned</span> | Runtime clock ownership and explicit reusable reset components |
| 9 | [Batched execution and run-ahead experiments](#9-batched-execution-and-run-ahead-experiments) | <span class="roadmap-status roadmap-status--planned">Planned</span> | Reduce DPI scheduler resumptions across fine-grained timing boundaries |

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

The API, semantic coverage, and performance-qualification machinery are
implemented. A current-policy formal baseline refresh for the realistic process
gates remains pending an admitted low-load host run.
The benchmark suite distinguishes persistent verification processes from
repeated dynamic process creation. The generic scheduler process pair remains a
hard-gated decomposition check. Realistic lifecycle qualification uses the
hard-gated persistent-monitor and finite driver/worker/scoreboard workloads;
repeated lifecycle-owned zero-time children remain a diagnostic fixed-cost
control rather than a framework release gate.

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

**Status:** <strong class="roadmap-status roadmap-status--done">Done</strong>

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

The protocol-neutral transaction slice is also implemented:

- a `MemoryMappedMaster` concept for typed reads and writes with addresses,
  data, byte enables, and explicit responses;
- `StreamSource` and `StreamSink` concepts for packet or word streams;
- direct coroutine transaction methods that serialize concurrent callers when
  required without exposing scheduler or arbitration machinery;
- reference-model adapters and keyed or out-of-order scoreboards; and
- explicit active master/source/sink roles and passive monitor/checker roles
  without a mandatory agent or environment hierarchy.

APB is the first complete protocol component. Its master, passive monitor,
protocol checker, structured transactions, runnable example, scoreboard, and
coverage composition are implemented. The same sequence is reusable through
the generic memory-mapped interface. The exact `apb_component` C++/pure-SV
pair exercises the active and passive components under the standard `1.10x`
performance guard.

The implementation lives under the separate `cpptb_vc` include tree,
namespace, and CMake target. It depends only on public core APIs and generated
signal values supplied by the caller, preserving a clean path to a separately
versioned component package. See
[Verification components](verification-components.md).

Add AXI-Lite, streaming, UART, or SPI only when each component is backed by
complete runnable examples and an exact SystemVerilog performance peer.

These remain ordinary C++ objects and coroutines, not a second scheduler or a
mandatory component hierarchy.

The endpoint layer should separate connectivity from storage in the useful
part of the UVM TLM model. A producer calls a put interface, a consumer calls a
get interface, and a monitor publishes through an analysis interface. A
`Queue<T>` may implement the put/get pair, but components should be testable
with another transport. Analysis publication must remain nonblocking; a
subscriber that needs asynchronous consumption connects through a buffered
adapter rather than silently delaying every other subscriber.

Transaction methods may encapsulate the signal writes and waits that define a
protocol operation, but the component implementation must show every signal
write, edge wait, sampling phase, and delay. The framework must not insert
implicit resets, unrelated waits, clock startup, or signal writes.

## 4. Reproducible random stimulus and functional coverage

**Status:** <strong class="roadmap-status roadmap-status--done">Done</strong>

The random-generation and constrained-random slices are implemented:

- `test.random()` returns the current process's deterministic stream;
- `randint()`, `choice()`, `weighted_choice()`, arbitrary-width `randbits()`,
  and `shuffle()` cover the common value-generation path;
- the master seed and versioned algorithm name are retained in schema-version 4
  structured results and accepted by the reference runner;
- independently derived process streams make concurrent generation insensitive
  to coroutine interleaving; and
- the exact `random_stimulus` C++/pure-SV pair checks every response and final
  checksum under the standard `1.10x` guard.
- user-defined transactions use `Randomized`, `Rand<T>`, `RandC<T>`, named
  constraints, `randomize()`, `randomize_with(...)`, `pre_randomize()`, and
  `post_randomize()`;
- membership sets and ranges, weighted distributions, soft defaults, and
  runtime constraint handles cover common SystemVerilog-style policy control;
- nested randomized objects, fixed `RandArray<T, N>` fields, and arbitrary-width
  `RandBits<Width>` values compose into one flattened solve problem;
- a backend-neutral typed constraint representation separates authored
  transaction classes from solving policy;
- the default adaptive backend keeps the sampling path dependency-free and
  invokes an application-configured fallback only after search exhaustion;
- the optional direct Z3 backend caches persistent translated models, handles
  coupled constraints, selects replayable randomized assignments, and reports
  named unsatisfiable cores;
- structured results record the configured backend, backend version, and the
  number of sampling and solver executions;
- the exact `constrained_packet` C++/pure-SV pair uses the same random stream,
  candidate rejection, DUT transactions, checks, and checksum under the
  standard performance policy; and
- the exact `constraint_extensions` pair exercises the new constraint and
  composite-field semantics against an equivalent pure-SystemVerilog workload;
- `Covergroup<T>` and typed coverpoints support ordinary, ignore, illegal,
  transition, and crossed bins with explicit allocation-free sampling; and
- coverage snapshots report percentages and stable model data, merge matching
  seeds, reject mismatched models, and serialize to schema-version 1 JSON.

CRAVE remains a possible future adapter, not a required or API-defining
dependency. Its viability spike required compatibility repairs for modern
CMake, macOS, Clang, and Z3 that the direct Z3 adapter does not require.

See [Randomization](random-stimulus.md) for the feature guide and
[side-by-side examples](randomization/examples.md) comparing cpptb, Cocotb,
UVM, and pure SystemVerilog authoring styles.

The exact `coverage_sampling` C++/pure-SV pair performs equivalent point,
ignore, illegal, transition, and cross accounting for each DUT transaction and
passes the standard `1.10x` hard guard. See
[Functional coverage](randomization/functional-coverage.md) for API and report
examples. UCIS interoperability remains a follow-on after the schema-1 model
has been exercised by larger regressions.

Random stimulus and coverage must remain separate facilities: users should be
able to use deterministic directed stimulus with coverage, or random stimulus
without adopting a coverage model. Coverage-hole selection may later generate
values from uncovered bins, but it is an optional policy layered on the same
random and coverage APIs rather than a prerequisite for either facility.

## 5. Memory and register verification components

**Status:** <strong class="roadmap-status roadmap-status--done">Done</strong>

The protocol-independent sparse memory model is implemented:

- allocated regions, byte enables, configurable endianness, and read/write
  permissions;
- expected-data regions with contextual mismatch diagnostics;
- read, write, and error-injection callbacks; and
- explicit image load, fill, inspect, and dump helpers;
- caller-owned `read_into()` storage and allocation-free integer word access.

The first typed register-model slice is also implemented:

- registers, fields, memories, access policies, and reset values;
- frontdoor operations through the generic `MemoryMappedMaster` interface;
- backdoor access through generated hierarchy paths;
- mirrored values, prediction, volatile fields, and read/write checking; and
- generation from SystemRDL and IP-XACT through a standard PeakRDL exporter
  plugin, plus direct native RgGen YAML/JSON/TOML import;
- preserved `accesswidth`, generated-file provenance, and path-qualified
  diagnostics for unsupported widths and reset references;
- fieldless and write-only prediction, readable-only mirror checking, and
  warnings for desired states that field write effects cannot reach; and
- reset-mask-aware desired and mirrored validity plus passive prediction from
  monitored bus transactions, including partial byte enables;
- generated typed backdoor adapters from standard SystemRDL `hdl_path` and
  `hdl_path_slice` metadata, with compile-time hierarchy pruning and no
  implicit delay; and
- generated register-backed memory frontdoors and hierarchy backdoors with
  per-operation path selection, raw `peek/poke`, and allocation-free span
  operations; and
- a pinned secworks AES ground-truth suite whose generated model reproduces
  the unchanged upstream bench's complete 720-event register transcript and
  all 20 NIST cases.

The memory and register layers should not own simulation time. A frontdoor
adapter exposes ordinary coroutine transactions, and a backdoor adapter
performs explicit hierarchical operations. This keeps both models reusable
across APB, AXI-Lite, Wishbone, and custom buses.

Named generated field and memory handles, relocatable block bases, desired and
mirrored values, every standard SystemRDL write effect, write-once policy,
passive memory prediction, generated-model execution against a fake master,
and the APB-backed example are covered by regression. The
exact `memory_model` C++/pure-SV semantic pair passes at 100,000 operations;
the bus-free `memory_model_direct` pair also matches at 200,000 model
operations, 300,002 checks, and zero simulated cycles. Formal timing remains
pending an admitted low-load run under the `1.10x` hard guard.

The scalable AES ground-truth workload has a historical diagnostic measurement
of `1.587x` pure SV for 3,600 cases. That run exceeded the current host-load
admission limit, and the ratio also exceeds the performance guard; semantic
equivalence is exact. The
architectural experiments intended to address this shared runtime cost are
tracked in [milestone 9](#9-batched-execution-and-run-ahead-experiments).

The RAL milestone was completed in the following order so correctness and
observation semantics landed before convenience and broader interchange
support:

| Order | Work item | Status | Acceptance criteria |
|---:|---|---|---|
| 1 | Prediction-valid masks and reset correctness | <strong class="roadmap-status roadmap-status--done">Done</strong> | Unspecified reset bits remain unknown to the mirror and are excluded from checks until predicted |
| 2 | Passive register predictor | <strong class="roadmap-status roadmap-status--done">Done</strong> | Monitored reads/writes from any bus initiator update the correct generated handle with byte-enable diagnostics |
| 3 | Generated backdoor metadata and adapters | <strong class="roadmap-status roadmap-status--done">Done</strong> | SystemRDL HDL paths can bind optional typed hierarchy access without affecting frontdoor-only users |
| 4 | Register-file hierarchy and arrays | <strong class="roadmap-status roadmap-status--done">Done</strong> | Generated models retain natural indexed hierarchy and deterministic iteration |
| 5 | Split and arbitrary-width frontdoors | <strong class="roadmap-status roadmap-status--done">Done</strong> | `accesswidth < regwidth`, endianness, partial failure, and values wider than 64 bits are modeled exactly |
| 6 | Typed field enumerations | <strong class="roadmap-status roadmap-status--done">Done</strong> | SystemRDL `encode` values generate typed APIs while raw negative-test access remains available |
| 7 | Block traversal and standard sequences | <strong class="roadmap-status roadmap-status--done">Done</strong> | Blocks expose reset/update/mirror traversal plus reusable reset, access, and bit-bash sequences |
| 8 | Multiple maps, aliases, and custom frontdoors | <strong class="roadmap-status roadmap-status--done">Done</strong> | One logical model can represent alternate address views and exceptional access procedures |
| 9 | User effects and RgGen import | <strong class="roadmap-status roadmap-status--done">Done</strong> | `ruser`/`wuser` policy hooks and direct RgGen metadata import preserve path-qualified diagnostics |
| 10 | Optional access coverage and bulk memory operations | <strong class="roadmap-status roadmap-status--done">Done</strong> | Caller-owned bulk memory operations and opt-in access coverage add no unused-model cost |

Every row requires unit and negative tests, generated-model execution, a
runnable usage example, and an exact pure-SystemVerilog performance peer before
it is marked Done. See the [register abstraction layer](memory-register-models.md),
[register-generation guide](verification-components/register-generation.md),
and [sparse expected memory](verification-components/memory-model.md).

Rows 1-4 have their implementation, negative/generated tests, and exact
SystemVerilog peers. Row 5 supports split transfers, explicit
little/big-endian ordering, partial-failure accounting, and generated
`Bits<Width>` frontdoors for logical registers wider than 64 bits. The exact
`register_split` and `register_wide` C++/pure-SV semantic pairs pass at 100,000
iterations. Generated wide HDL backdoors, passive prediction over every
transfer address, and wide register-backed memory elements use the same typed
`Bits<Width>` model. Formal performance runs remain pending a host-load window
accepted by the benchmark admission policy; rejected load windows are reported
as `invalid_environment`, not as pass or failure data.

Row 6 generates shared C++ enum types from SystemRDL `encode`, symbolic
failure diagnostics, typed read/write/desired/mirror access, and an explicit
`.raw()` escape hatch for reserved encodings. Its `register_enum` C++/pure-SV
semantic pair passes at 100,000 operations; the performance guard is recorded
separately from functional completion.

Row 7 has generated `reset_all()`, `update_all()`, and `mirror_all()` traversal
in deterministic address order for homogeneous and arbitrary-width models. Its
optional sequence layer now provides reset-check, mixed frontdoor/backdoor
access-check, and conservative policy-aware bit-bash operations with generated
asynchronous traversal, summary counters, negative tests, and an exact pure-SV
semantic peer. The semantic pair passes at 100,000 iterations with 2,100,000
frontdoor operations and 4,600,002 checks. Formal timing remains pending an
admitted host-load window.

Row 8 provides named `RegisterAddressMap` instances with independent masters,
base addresses, per-register and per-memory aliases, and optional custom
frontdoors. All views share one logical desired/mirrored state; backdoor paths
continue to describe physical RTL storage. Its exact `register_maps` pair
executes the same primary, alias, and custom-frontdoor sequence in C++ and pure
SystemVerilog.

Row 9 provides an optional `RegisterUserEffectPolicy` for generated SystemRDL
`ruser` and `wuser` fields. Without a policy those bits deliberately become
unknown; with a policy, update encoding and read/write prediction are fully
defined. Native RgGen YAML, JSON, and TOML contracts can be imported directly
with `cpptb-rggen`, preserving hierarchy and path-qualified diagnostics. The
exact `register_user_effects` pair covers the policy hot path.
Packed field callbacks, cached register metadata, single-transfer prediction
commit, and a simulator-thread frame pool reduced the 10,000,000-iteration C++
diagnostic from `3.362 s` to `0.994 s`. The matching `0.400 s` pure-SV median
gives a raw `2.49x` ratio. A formal rerun was rejected before sampling because
host load exceeded admission limits. This zero-time abstraction-versus-inlined
equations microbenchmark is registered as diagnostic at 10,000,000 iterations;
the timed secworks AES integration remains the release-facing performance
comparison, and run-ahead work remains tracked in milestone 9.

Row 10 includes generated memory `read/write(index, ..., AccessPath)`, raw
`peek/poke`, caller-owned bulk spans, partial-frontdoor failure reporting, and
typed SystemRDL `hdl_path_slice` mapping for one-dimensional HDL memories. Its
ergonomic layer includes allocation-free memory slices, compile-time register
array slices, typed register/field/memory traversal, and direct logical/HDL path
introspection. Entry-relative, memory-relative byte-offset, and absolute bus
address forms support both scalar and caller-owned chunk operations with
alignment and range diagnostics. `RegisterAccessCoverage` is an optional
analysis subscriber that records register/memory read and write outcomes,
including failed and unmapped transactions; models that do not instantiate it
execute no coverage path. Its
exact `register_memory` C++/pure-SV semantic pair performs the same four
deposits, four reads, six checks, and checksum updates per iteration. Optional
coverage has its own exact `register_coverage` peer. Timing publication awaits
a host-load window admitted by the benchmark policy.

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

- scoped structured logging with simulation timestamps and `trace`, `debug`,
  `info`, `warning`, and `error` levels;
- lazy message formatting so disabled logs do not burden hot paths;
- transaction recording with component and process provenance;
- runtime waveform start/stop and scope selection;
- waveform-on-failure support in the test runner;
- a scheduler wait graph with process names, spawn sites, and outstanding
  triggers for deadlock and timeout reports;
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
model must scale to unrelated input clocks and DUT-generated clocks, and report
actionable ownership errors when a second component attempts to drive a
scheduler-owned clock.

Reusable reset components may be built on these primitives, but their APIs
must expose each asserted value and each wait. They must not silently drive a
reset sequence merely because a test starts.

## 9. Batched execution and run-ahead experiments

**Status:** <span class="roadmap-status roadmap-status--planned">Planned</span>

Profile the architectural options for reducing simulator-boundary resumptions
without weakening the ordinary coroutine API. The exact secworks AES workload
has measured `1.587x` pure SV in a high-load diagnostic run even though its
generated clock produces no DPI callback. Its dominant remaining cost is
entering DPI and resuming the
C++ scheduler at each of 88,001 authored timing boundaries; register handles
and per-access coroutine wrappers account for only a few percent.

Prototype and measure these approaches independently:

- a compact command batch that lets C++ submit several drives, waits, reads,
  and checks per DPI entry;
- run-ahead execution that continues until a value-dependent read, unknown HDL
  event, cancellation point, or simulator phase requires control to return to
  C++; and
- generated SystemVerilog execution of eligible batches while retaining the
  existing scheduler for dynamic or unsupported control flow.

The experiments must preserve arbitrary clocks, hierarchical signal access,
explicit timing, read-after-write behavior, cancellation, timeouts, process
ownership, and failure attribution. Batching must be an optimization beneath
the familiar `Task` authoring model or an explicitly scoped sequence facility,
not a second mandatory testbench language.

Each prototype must report DPI entries and scheduler resumptions as well as
wall time. It must first match an exact pure-SystemVerilog peer, then improve
the secworks AES benchmark and at least one realistic concurrent workload
without regressing existing workloads beyond the repository's `1.10x` policy.
Small register-handle and coroutine-wrapper changes are not priority
experiments because controlled decomposition shows they cannot close the
current gap.

## Deliberate non-goals

cpptb should not copy the complete UVM architecture. In particular, a factory,
global configuration database, mandatory runtime phases, and objection system
would add substantial machinery without improving the current testbench model.
The useful ideas to retain are typed transactions and clear separation between
sequences, drivers, monitors, and scoreboards.

The first component library also does not require a heavyweight sequencer,
mandatory agent hierarchy, or implicit component startup. Direct coroutine
transactions and typed endpoints remain the default; arbitration is introduced
only where a protocol or concurrent-caller use case requires it. The optional
direct Z3 backend solves coupled constraints without making a solver a runtime
dependency. A complete SystemVerilog constraint-language clone, dynamic random
arrays, and coverage-guided solving remain deferred until real tests establish
their value.

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
without adopting its full infrastructure. Generic protocol interfaces and
shared memory models follow the useful separation demonstrated by the
[VUnit verification-component interfaces](https://vunit.github.io/verification_components/user_guide.html),
while the randomization and coverage direction also considers
[OSVVM's independently usable verification facilities](https://osvvm.org/about-os-vvm).

## Delivery process

For every runtime feature:

1. Add unit, negative, conformance, cancellation, and usage tests as applicable.
2. Add an individually runnable C++ DPI workload and exact SystemVerilog twin.
3. Run semantic parity before performance measurement.
4. Benchmark serially and stop on a valid C++ DPI/SystemVerilog ratio above
   `1.10x` unless a scoped waiver is reviewed and recorded. Synthetic
   abstraction-versus-direct kernels may instead be explicitly classified as
   diagnostic when a matched timed integration benchmark retains the release
   gate.
5. Run the full affected regression and retain a compact reviewed baseline.
6. Update the user guide, runnable examples, and roadmap status in the same
   change.
