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
| 6 | [Interfaces and simulator portability](#6-interfaces-bidirectional-signals-and-portability) | <span class="roadmap-status roadmap-status--next">In progress</span> | Typed interfaces and inout intent are shipped; four-state and second-simulator conformance remain |
| 7 | [Debugging and release tooling](#7-debugging-and-release-tooling) | <span class="roadmap-status roadmap-status--next">In progress</span> | Process-aware logging and scheduler wait diagnostics are done; waveforms and distributable packages remain |
| 8 | [Coherent clock and reset control](#8-coherent-clock-and-reset-control) | <span class="roadmap-status roadmap-status--planned">Planned</span> | Runtime clock ownership and explicit reusable reset components |
| 9 | [Batched execution and run-ahead experiments](#9-batched-execution-and-run-ahead-experiments) | <span class="roadmap-status roadmap-status--planned">Planned</span> | Reduce DPI scheduler resumptions across fine-grained timing boundaries |
| 10 | [Static hierarchy discovery](#10-static-hierarchy-discovery) | <strong class="roadmap-status roadmap-status--done">Done</strong> | Produce the discovery outputs without executing user test code during the build |
| - | [No-priority backlog](#no-priority-backlog) | <span class="roadmap-status roadmap-status--planned">No priority</span> | Deferred interoperability, protocol-component, synchronization, lookup, and harness work |

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
performance guard. Its current semantic run passes; the post-envelope timing
remeasurement is pending because the July 20, 2026 run was rejected by host
load admission.

Typed transaction recording is also implemented in `cpptb_vc`. Timed
`TransactionObservation<T>` envelopes, monitor-owned analysis output, static
field descriptors, uniquely named recorder streams, an in-memory sink, and a
JSON Lines sink keep protocol decoding separate from persistence. The runnable
[APB transaction trace](examples/apb-trace.md) records and checks the same 256
operations in C++ and pure SystemVerilog. The existing `apb_component` pair is
the disabled-recorder overhead guard. The `transaction_recording` authoring
pair retains 200,000 equivalent records in C++ and pure SystemVerilog and is
the enabled-recorder `1.10x` hard gate. Its semantic run passes; the July 20,
2026 timing attempt was rejected by the host-load admission check, so a timing
ratio remains pending a valid serial run.

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
the APB-backed example, and a first-class IP-XACT register-and-memory example
with a matching pure-SystemVerilog sequence are covered by regression. The
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
coverage has its own exact `register_coverage` peer, published as a
diagnostic: the peer tallies precomputed outcomes while the C++ side
performs generic per-transaction decode, so the pair measures the price of
generality (`10.06x`, ~33 ns per observed transaction after the 2026-08
optimization pass) rather than equivalent work. The reasoning is recorded
in the registry entry and OPTIMIZATION_NOTES.

## 6. Interfaces, bidirectional signals, and portability

**Status:** <span class="roadmap-status roadmap-status--next">In progress</span>

Shipped in the Verilator reference flow:

- generated named access for parameterized SystemVerilog interfaces with an
  explicit modport;
- fixed interface arrays and independent clocks selected by exact generated
  signal identity;
- `drive()`, `high_z()`, and `get()` for top-level and interface-member
  `inout` signals, with no implicit delay;
- natural `dut.scope[index].member` access for homogeneous elaborated instance
  and generate arrays; and
- simulator capability diagnostics that reject X/Z writes when a two-state
  backend would silently coerce them.

Two small remaining ergonomics items, both found by porting Ibex's icache
testbench (`experiments/open_core_ports/ports/ibex_icache_cpptb`). Neither
changes any semantics; both are mechanical work that a project currently
repeats by hand:

- **Wide four-state ports.** Code generation rejects any port wider than 32
  bits whose declared type is four-state unless it is an `inout`, for inputs
  and outputs alike and for interface members as well as top-level ports. The
  icache wrapper had to redeclare the 128-bit scrambling key and the 64-bit
  nonce as `bit` before it would build. Internal signals of the same width and
  type are unaffected, so this is a limit of the packed port transport rather
  than of the value model, and carrying the B plane for wide ports is the fix.
  [Four-state values](four-state.md#wide-ports-must-be-declared-two-state)
  records the exact rule and diagnostic.
- **A signal bundle for a flat pin list.** A SystemVerilog interface port with
  a modport already generates a passable per-element group, so a driver and a
  monitor can share one `dut.link[index]` handle. A design whose protocol pins
  are ordinary top-level ports has no equivalent. Every task in the icache port
  therefore takes the whole `Dut` and states which pins it owns only by which
  ones it happens to touch, and the wrapper module exists partly to lift the
  core-side, memory-side and key pins to the boundary where the UVM
  environment binds three interfaces instead. `cpptb_vc`'s `ApbBus` shows the
  shape that works, an aggregate of generated signal handles constructed by the
  testbench, but one has to be written per protocol. The work is proportional
  to the protocol and repeated per port.

Remaining portability work:

Run the complete generated transport and scheduling conformance suite on at
least one additional standards-compliant simulator. A four-state simulator is
required to validate X/Z propagation end to end; Verilator remains the
two-state performance reference. Simulator capability differences should be
reported at startup rather than hidden in testbench code.

Structured logging now converts `$realtime` through seconds rather than
dividing by a `1fs` literal, avoiding a portability bug where a coarser
`timeprecision` could round the divisor to zero. The Verilator integration test
asserts the exact femtosecond timestamps received through DPI. The same
assertion must pass when the second simulator joins the conformance matrix.

The framework now has a fail-closed Verilator capability gate and cached
semantic probe covering X/Z storage, high-impedance and conflicting-driver net
resolution, and both DPI directions. Verilator 5.050 and current upstream
development fail that probe;
their experimental `--fourstate` option is not a usable simulator capability
yet. A regression intentionally records this blocked state and will fail with
an enablement reminder when all semantics become available. The
[four-state guide](four-state.md) documents the existing `LogicBits` API and
the conformance criteria required before transport is enabled.

## 7. Debugging and release tooling

**Status:** <span class="roadmap-status roadmap-status--next">In progress</span>

Scoped structured logging is implemented with:

- `trace`, `debug`, `info`, `warning`, and `error` levels plus `off`;
- lazy callable messages that are not invoked below the configured threshold;
- test name, simulation time, source location, and lifecycle process
  provenance on every emitted record;
- per-test sequence numbers and an opt-in owned `LogHistory` for stable
  same-time ordering and sequential trace inspection;
- SystemVerilog logging macros that capture source location and hierarchy and
  merge HDL messages into the same test-owned sequence and history;
- a synchronous harness-neutral `LogSink` that does not retain messages in
  `TestResult`; and
- matched `structured_logging` and `structured_log_history` C++/pure-SV
  pairs plus a mixed C++/RTL logging pair with a pure-SV peer.

The exact semantic pairs pass. Formal timing remains pending a host-load
window admitted by the standard `1.10x` guard. See
[Structured logging](logging.md) for API and sink-lifetime details.

Remaining facilities needed to diagnose and distribute real regressions:

- optional machine-readable export of structured log histories;
- [transaction recording](verification-components/transaction-recording.md)
  with component and process provenance;
- <strong class="roadmap-status roadmap-status--done">Done:</strong>
  waveform dumping as a build variant -- `--wave` (FST or VCD), one file
  per test from the framework host loop on both timing backends, proven
  against the pure-SV twin by a cycle-sampled wave-equivalence gate in
  `make test`. Runtime start/stop windowing and scope selection remain
  open ergonomics on top;
- waveform-on-failure stays deliberately unautomated: waves are asked for,
  not implied by a failure;
- <strong class="roadmap-status roadmap-status--done">Done:</strong> a
  scheduler wait graph with process parentage, spawn sites, outstanding
  triggers, named synchronization resources, timeout capture, and conservative
  deadlock classification;
- clean attribution of HDL assertion failures;
- simulator and generated-binding compatibility reports; and
- versioned runtime and code-generator packages.

The wait-graph semantic, conformance, negative, and full-example regressions
pass. Its event hot-path performance guard is **Pending** until the benchmark
runner admits a low-load host window; the last attempted run was rejected by
the load gate before collecting samples.

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

## 10. Static hierarchy discovery

**Status:** <strong class="roadmap-status roadmap-status--done">Done</strong>

Shipped. The build recovers the access set by scanning compile-only object
sections -- no link, no run -- and clocks are registered at runtime by
`start_clock()` itself: the generated wrapper carries a driver task per
writable one-bit signal (including unpacked-array elements, which is how
interface-member clocks arrive) and queries the runtime after `PHASE_INIT`
for the ones actually started. Both failure modes below are gone, `clocks.json`
and its staleness checks are gone, and the executed-discovery entry point
survives only as an explicit opt-in (`clock_discovery_source`) for
manifest-driven flows. Validated by the example matrix under both timing
backends, the icache port on both backends with identical check counts, a
pin-exact core_ibex replay, and the directed 944 against the UVM baseline.
The behavior change step 2 called out is real and documented: cross-test
clock conflicts are now legal per-test configurations that fail at run time
if genuinely conflicting. The section below is kept as the original problem
statement and plan.

The build's hierarchy-discovery pass compiles the testbench under
`CPPTB_HIERARCHY_DISCOVERY` and then **runs it**. Scoping measured what that
execution actually contributes, and the answer halves the problem: the access
set is already a static-initialization side effect of template instantiation —
`nm` over the compile-only objects reproduces `access.json` byte-identically
with no link and no run, verified on the examples and the Ibex core port. Only
the **clock configuration** comes from execution, which runs each registered
test's prologue up to its first suspension.

Executing user code inside the build is still the defect. Measured, the
failure is worse than the hang this section first described: a value-dependent
loop before the first `co_await` does hang the build with a compiler command
as the last log line, but an edge wait nothing drives does **not** hang — it
suspends and yields a silently empty clock configuration, an undocumented
wrong answer. A timeout would rename the first symptom and never see the
second; this milestone removes the cause.

Done means the discovery outputs are produced without executing user test
code, both failure modes above are gone, and a testbench that builds today
builds identically with no source changes. An earlier version of this section
required keeping the transport's "measured pruning benefit"; scoping found no
such measurement exists — ports were never value-pruned, and discovery prunes
hierarchy exports and edge observers. The real bar is **output parity**:
byte-identical `access.json` against the executed path, gated in CI until the
executed path is deleted. The steps, from the scoping report (`STATIC-DISCOVERY-SCOPING.md` at the repository root):

1. **Access set without execution.** Emit discovery records into a dedicated
   object section, compile the testbench translation units with `-c` — no
   generated main, no link — and scan the objects. Same schema, parity-gated
   across GCC and Clang on the examples, benchmarks, and Ibex ports.
2. **Clocks without execution.** Promote the already-implemented
   `dynamic_clocks` mode to the default: the generated SV drivers query the
   runtime's clock configuration at time zero over the existing DPI, and
   `clocks.json` with its staleness checks disappears. The work is
   re-qualifying the benchmarks under the `1.10x` guard; the fallback if a
   guard fails is declared clocks through the existing `--clock` key. One
   behavior change to call out: cross-test clock conflicts stop failing the
   build and become legal per-test configurations.
3. **Document the escape hatch.** Explicit declaration through the
   pre-existing `--clock`, edge-observer, and access-manifest keys as the
   override surface.

Rejected with reasons recorded in the scoping report: external static
analysis (the compiler's own instantiation is already the alias-robust
oracle), a fuel-bounded dry run (still executes user code and contributes no
access entries), and carrying every signal unpruned (hundreds of forced
exports where the current builds need none, defeating simulator
optimization).

## No-priority backlog

**Status:** <span class="roadmap-status roadmap-status--planned">No priority</span>

The following ideas are intentionally deferred and are not scheduled after the
numbered milestones:

- UCIS functional-coverage interchange;
- additional protocol components such as AXI-Lite, UART, SPI, and richer
  streaming components;
- `Barrier`, unless a concrete multi-process example needs semantics that
  cannot be expressed clearly with `Join`, `Event`, or `Semaphore`;
- dynamic string-based hierarchy lookup; and
- reference-harness enhancements including JUnit conversion, tag filtering,
  waveform-on-failure reruns, and reproduction-command presentation.

These items may be promoted only when a concrete project supplies the use case,
semantics, runnable example, test coverage, and performance peer needed to
justify prioritizing them.

## Candidate directions

Analysis of capabilities the framework lacks, and of the open-source
verification corpus needed to judge it against real alternatives, is recorded
in [future directions](future-directions.md). Nothing there is scheduled: those
items still have to clear the promotion bar described above.

One item there already has the use case, runnable example and performance peer
that bar asks for, so it is named here rather than left to be found:
[aligning the scheduling semantics with cocotb](future-directions.md#align-the-scheduling-semantics-with-cocotb).
The trigger vocabulary matches cocotb deliberately, but writes apply
immediately where cocotb defers them, so a cocotb testbench translated line for
line drives a cycle early. The documented workaround is a convention rather
than a mechanism: sample on the rising edge, drive off the falling one, and
enter and leave every driving task at a drive point so that it does not
re-anchor itself.

Porting Ibex's icache testbench
(`experiments/open_core_ports/ports/ibex_icache_cpptb`) measured what holding
that convention costs. Every driving task had to be traced edge by edge against
the `default output negedge` clocking block it replaces, and the convention
itself is stated in a comment at the top of the file because nothing in the API
expresses it. It was the largest single cost of writing that port's drivers.
This is the one real semantics gap the port produced; the two items it added to
[milestone 6](#6-interfaces-bidirectional-signals-and-portability) are small
ergonomics work by comparison.

`ReadWrite` and `ReadOnly` express the convention directly, but no
`cpptb build` configuration supplies them to their documented contract. A
default build fails at run time; the `--vpi` stanza that the run-time message
names stops the failure without making the phases settle correctly; a project
can assemble a define combination that is wrong in a third way and says
nothing; and phases still only help an author who knows to reach for them.
Both contract-complete backends need cpptb to own the host loop, which means a
`--cc --exe --build` link against `src/verilator_timing_main.cpp` rather than
the `--binary` link `cpptb build` emits.

### What the gap looks like

The cases below run against one design, `phases`: a 4 ns clock, a
combinational `comb_sum = drive_value + addend`, and one `always_ff` that
samples `drive_value` and counts edges. Measured under Verilator 5.050.

**`ReadWrite` and `ReadOnly` fail at run time in a default `cpptb build`.**

```cpp
Task<void> driver(Dut dut, TestContext& test) {
    dut.clk.set(0);
    test.start_clock(dut.clk, 4_ns);
    co_await RisingEdge{dut.clk};
    co_await ReadWrite{};
    dut.drive_value.set(0x55);
}
CPPTB_REGISTER_TEST(driver);
```

`cpptb test` reports only that the process produced nothing:

```
ERROR driver: simulator did not produce a result file
```

The reason is in `build/cpptb/<name>/results/<test>.log`:

```
CPP_DPI_PHASES_RESULT: ReadWrite, ReadOnly, and NextTimeStep need a timing
backend that dispatches simulator phases. This build has none: a default
`cpptb build` links Verilator's own --binary main, which owns clocks and
timers but dispatches no phases.
  Wait on a clock edge instead: sample after `co_await RisingEdge{clk}`,
drive after `co_await FallingEdge{clk}`. That is the supported route and it
is what the ports in experiments/open_core_ports use.

  Adding `verilator_args = ["--vpi"]` to cpptb.toml stops this error and
places writes on the right edge, but does NOT give a complete phase
contract: ReadOnly does not observe a write settled in ReadWrite. Only a
`--cc --exe --build` link against src/verilator_timing_main.cpp holds the
full contract, and `cpptb build` cannot produce one. See docs/scheduling.md.
```

The `--vpi` stanza the message describes lets the test run, and the drive
point it produces is right, but the phases are not contract-complete. Running the five checks of
`timing_phase_contract` from `tests/conformance/runtime/testbench.cpp` against
this design in a `verilator_args = ["--vpi"]` build fails two of them:

```
ReadOnly observes write settled after ReadWrite: actual=19 expected=35
ReadWrite observes sampled data after NBA: actual=16 expected=32
```

The same generated wrapper, testbench and RTL relinked as
`--cc --exe --build --vpi` against `src/verilator_timing_main.cpp` passes all
five: that main re-evaluates the model after each `ReadWrite` callback before
running `ReadOnly`, and Verilator's own `--binary` main does not.

**The drive-point convention.** The icache port states the rule in a comment
because nothing in the API does, and every driving task holds to it:

```cpp
// A "drive point" is the instant just after a falling edge of clk_i. Every
// task below that drives a pin is entered at a drive point and returns at a
// drive point, and none of them opens with a wait: a task that re-anchored
// itself would place its first write one cycle later than the UVM driver
// does.

Task<void> wait_clks(Dut dut, uint32_t count) {
    if (count == 0) co_return;
    co_await clock_cycles(dut.clk_i, count);
    co_await FallingEdge{dut.clk_i};   // return to a drive point
}

Task<void> lower_req(Dut dut, uint32_t cycles) {
    if (cycles == 0) co_return;
    dut.req_i.set(0);                  // already at a drive point: no wait
    co_await wait_clks(dut, cycles);
    dut.req_i.set(1);
}
```

Written against phases the same driver would carry no convention: it would not
matter where its caller was, and `wait_clks` would have nothing left to do.

```cpp
Task<void> lower_req(Dut dut, uint32_t cycles) {
    if (cycles == 0) co_return;
    co_await ReadWrite{};
    dut.req_i.set(0);
    co_await clock_cycles(dut.clk_i, cycles);
    co_await ReadWrite{};
    dut.req_i.set(1);
}
```

Both shapes place a write on the edge after the point where the driver
resumed. On the design above, a driver that resumes on the 6 ns edge and then
awaits `ReadWrite` is captured by the 10 ns edge; the same driver writing
immediately after `co_await RisingEdge{}`, which is the line-for-line cocotb
translation, is captured by the 6 ns edge it just resumed on.

**The silent wrong-answer configuration.** Matching `design.defines` against
`build.cxx_flags` by hand selects the inline SV-DPI pump:

```toml
[design]
defines = ["CPPTB_SV_DPI_TIMING"]

[build]
cxx_flags = ["-DCPPTB_SV_DPI_TIMING"]
```

It builds and runs. Three of the five contract checks fail:

```
ReadOnly observes write settled after ReadWrite: actual=19 expected=35
ReadWrite observes clocked update in new timestep: actual=0 expected=1
ReadWrite observes sampled data after NBA: actual=0 expected=32
```

and the phase-form driver above drives a cycle early: its write is captured by
the 6 ns edge rather than the 10 ns one. Nothing in the build or the run says
so. Those failures are visible only because a probe asserts on them; a
testbench that does not check settled values reports a pass.

Adding `CPPTB_SV_DPI_NBA_TIMING` (or additionally
`CPPTB_SV_DPI_CALENDAR_TIMING`) to the same hand-matched define pair selects
the NBA or calendar pump instead, and those pass all five contract checks and
place the driver's write on the correct edge in a plain `cpptb build`. The
machinery for a contract-passing build therefore already exists behind
unvalidated defines. It is still not the full documented contract: with no
started clock and no framework timer, a `NextTimeStep` waiter under either
pump misses a DUT-internal `#5ns` event that direct dispatch and the VPI
bridge both wake for, and times out against the watchdog. That is the
observed-events limit the [backend table](scheduling.md#timing-backend-support)
records.

Three ways to close it, in increasing order of commitment. They are not
alternatives so much as a sequence: each is useful on its own, and each makes
the next smaller.

1. **Name and validate the backend selection — done.** `[build]
   timing_backend` accepts exactly two names, `"verilator-direct"` and
   `"vpi"`, both contract-complete; the policy is two supported backends and
   no more, because both sit at the privileged layer that sees the whole
   event queue, which the SV-DPI experiments cannot. The key emits the
   `--cc --exe --build --vpi` link against the framework host loop; a bare
   `--vpi` in `verilator_args` and every hand-set timing define are rejected
   at resolve time with the key named; the run-time diagnostic in a
   backendless build names the key; and `make test` conformance-checks both
   names on every run.
2. **Offer deferred writes, as a project mode — done.** `deferred_writes = true` in
   `cpptb.toml` makes `set()` itself carry cocotb's semantics -- queued,
   flushed at the `ReadWrite` settle point the selected backend provides --
   with `set_now()` as the marked escape hatch, mirroring cocotb's
   `setimmediatevalue()`. There is no `set_deferred()` spelling: cocotb has
   one write syntax and so does this. The mode requires `timing_backend`,
   enforced at build time, which keeps one mechanism instead of a
   default-build variant. Pinned semantics, matching cocotb exactly: a
   `get()` between `set()` and the flush returns the simulator's current
   value, not the queued one. `tests/integration/deferred_writes` pins that,
   write-lands-next-edge, settled-by-ReadOnly, and the `set_now()` escape
   hatch on both supported backends in every `make test`. The port
   validation is done: the icache port converted to the mode passes ten of
   ten on both backends with check counts identical to the immediate-mode
   run, after three drive-point corrections documented in
   [Coming from cocotb](coming-from-cocotb.md).
3. **Flip the mode's default, then retire the immediate branches.** With 2
   shaped as a project mode, full cocotb parity out of the box is a change
   of default, not an API migration. It still alters what existing
   testbenches do and costs a scheduler round trip per write, so it waits
   for mileage on the opt-in. Once the default flips, the component
   library's dual shape stops earning its keep: `cpptb_vc` carries an
   immediate-mode `#else` branch in every drive and observation path
   (falling-edge anchors, post-edge `sample_delay` sampling) solely for
   builds without the mode, and the `sample_delay` knobs exist only to
   serve that branch. The end state is one shape -- rising-edge anchors,
   deferred flushes, pre-evaluation observation -- with the immediate
   branches and their calibrated post-edge counting deleted, and the
   remaining immediate-mode consumers (the exact benchmark kernels and
   their pure-SV twins) migrated or re-calibrated in the same change. The
   benchmark peer the flip also waited for exists and is certified: `timing_phases_deferred` -- the
   same kernel and pure-SV reference, built with the mode on -- passes the
   `1.10x` guard at `0.8903x` against the SV twin, with the immediate kernel
   at `0.7383x` in a comparable admitted window. The write model's measured
   cost on that worst case (two writes and three phase awaits per iteration,
   nothing else) is therefore about `1.21x` over immediate writes, while
   still beating the SV reference.

### What 1 and 2 look like in use

Option 1 is implemented:

```toml
[build]
timing_backend = "verilator-direct"   # or "vpi"
```

Option 2, under the mode, keeps one write spelling -- a driver written the
cocotb way is correct with no drive-point convention:

```cpp
// Requires deferred_writes = true.
Task<void> driver(Dut dut) {
    while (true) {
        co_await RisingEdge{dut.clk};
        dut.wdata.set(next_word());   // queued; lands for the next edge
        dut.wvalid.set(1);
    }
}
```

Doing 2 leaves the framework familiar to a cocotb author without changing
what any existing testbench does. 3 is a separate decision and should be
taken on its own evidence.

## Deliberate non-goals

cpptb should not copy the complete UVM architecture. In particular, a factory,
global configuration database, mandatory runtime phases, and objection system
would add substantial machinery without improving the current testbench model.
The useful ideas to retain are typed transactions and clear separation between
sequences, drivers, monitors, and scoreboards.

That judgement now has a same-design comparison behind it, and it belongs
beside the gaps above so the trade is visible in one place. Ibex's icache
testbench exists in both forms against the same DUT elaborated from the same 77
sources: upstream's UVM environment in
`experiments/open_core_ports/ports/ibex_icache_uvm`, where all ten tests pass,
and a cpptb port of three of them in
`experiments/open_core_ports/ports/ibex_icache_cpptb`. The port needs no
factory, no configuration database, no phasing and no sequencer arbitration,
and its scoreboard is a plain class the monitor calls synchronously, which is
what an analysis port already provides. Generating stimulus values directly
also keeps a class of defect off the path entirely: the two icache ports
between them document six Verilator defects with reduced cases, five of them in
constrained randomization, and the cpptb port draws every field from
`test.random()` with no solver involved, so none of the five has anywhere to
occur. cpptb's own optional constraint layer would put a solver back on the
path, but nothing obliges a testbench to use one. Wall time
is about 230 times lower, with the caveats recorded in the port's `RESULTS.md`:
the baseline runs about 18% more cycles for an unrelated reason, and it
elaborates the UVM environment and two protocol-checker modules that the port
does not, so it compares two harnesses rather than two simulators.

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
