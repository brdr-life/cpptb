# Architecture

This page is for contributors and for anyone embedding cpptb in their own build
or regression system. It describes how the runtime is layered, where the
simulator boundary sits, and which ownership rules the design depends on.
Nothing here is needed to write a testbench.

The repository contains a reusable framework and an optional reference
harness. The framework has four separable layers:

1. `coro_runtime.hpp` owns tasks, processes, waits, timers, events, queues,
   and scheduler ordering.
2. `test_api.hpp` owns the compiled test catalog, one-test selection,
   assertions, process lifetime, and exception attribution.
3. `dpi_runtime.hpp` translates simulator callbacks into scheduler steps and
   batches changed values across the DPI boundary.
4. `cpptb-codegen` elaborates RTL and emits a typed DUT plus transport wrapper.

The reference harness has two layers:

1. The public `cpptb` project layer resolves source conventions or
   `cpptb.toml`, owns the two-pass build, and caches simulator artifacts.
2. The runner discovers tests and starts one fresh simulator process per
   selection; `cpptb-run` exposes that lower-level executable protocol alone.

`test_result.hpp` and `test_reporting.hpp` sit beside these layers as an
embeddable result contract. A higher-level harness can call
`registered_tests<Dut>()` and `run_registered_test(...)`, receive `ResultSink`
callbacks, or consume versioned JSON without using the command-line launcher.
The lifecycle contract and its current limitations are documented in
[Framework test lifecycle](test-lifecycle.md).

The generated SystemVerilog wrapper owns clocks and simulator callback timing.
The C++ scheduler owns coroutine readiness and cancellation. Signal reads and
writes use generated IDs and typed bindings; no runtime hierarchical-name
lookup is required on the optimized DPI path.

Every registered test invocation creates one shared lifecycle state. Processes
started through its `TestContext` are retained as test-owned work. Normal root
completion, a fatal requirement, or an uncaught child exception cancels any
remaining owned processes before the result becomes terminal. Generic
low-level scheduler roots still abort on an unobserved exception, so exceptions
cannot disappear silently outside the test lifecycle boundary.

`Scheduler`, `Testbench`, `TestContext`, and their `Process` handles are
confined to the simulator thread that owns the testbench. They are not
thread-safe and must not be copied to worker OS threads. This matches the
single-threaded scheduler and simulator-callback contract and lets lifecycle
ownership use non-atomic reference counting on the process hot path. Parallel
host work must return its result through a simulator-thread integration point
rather than calling framework APIs directly.

Coroutine frames are cached in one process-wide pool under that ownership
contract. This avoids a thread-local lookup for every task creation and
reclamation. An embedding that runs independent simulator runtimes on separate
OS threads may define `CPPTB_CORO_THREAD_LOCAL_FRAME_POOL` to give each thread
its own pool; framework objects and callbacks must still remain on their owning
simulator thread.

Simulator-phase waits have a backend boundary below the public API. The
portable path registers standard VPI callbacks for read/write synchronization,
read-only synchronization, and the next timestep. When cpptb owns Verilator's
host loop, its direct backend polls the same pending phase state at those exact
host-loop locations and dispatches the generated DPI phase task without
allocating one-shot VPI callback records. The VPI path remains available for
other simulators and Verilator builds that require external VPI integration.
Both supported modes continue to use generated DPI for signal transport; only
their simulator-phase dispatch mechanism differs.

Generated pure-DPI timing paths are retained as experiments, not supported
backends. An NBA-barrier phase pump reproduces the timing contract but is
slower than portable VPI. A centralized generated calendar instead gives one
SystemVerilog process ownership of compile-time-discovered clocks, framework
timers, and phase dispatch; it passes conformance and the exact performance
guard under Verilator. External and DUT-driven clocks remain thin observers.

The calendar only knows generated clocks, framework timers, and explicitly
observed signals. Standard DPI has no simulator-wide next-event query, so
arbitrary internal delayed DUT activity still requires generated observation
or a simulator callback API. Cross-simulator validation is required before the
calendar can become a supported portable backend.

The resulting backend policy is:

1. Use direct timing dispatch when cpptb owns the Verilator host loop.
2. Use standard VPI callbacks as the supported portable fallback.
3. Keep the generated SV-DPI calendar feature-gated until its restricted
   `NextTimeStep` semantics and cross-simulator behavior are acceptable.

The framework keeps clock-cycle waits and absolute delays separate. Any signal
configured as an observable clock can drive edge waits, while the persistent
timer owner supports clockless and arbitrary multi-clock designs.

## Reusable DPI runtime

`include/cpptb/dpi_runtime.hpp` owns the design-independent DPI host behavior:

- compact directional input/output transport and driven-signal tracking;
- typed signal `get()`/`set()` callbacks and dirty-output detection;
- generated internal-probe `get()`/`deposit()` access for packed variables and
  fixed memories;
- scheduler construction, edge dispatch, and delay deadlines;
- falling-edge interest and precision-aware time transport;
- timeout invocation, elapsed wall time, completion, and result reporting;
- the standard init, step, output-pull, deadline, and edge-interest C exports
  expected by the generated wrapper.

`include/cpptb/test_result.hpp` keeps status, check counts, timing, and
structured failure records independent of DPI. `test_reporting.hpp` writes the
versioned JSON result consumed by the optional launcher, so user-facing
fixtures and embedding harnesses do not include simulator transport headers.

A design supplies a small `DpiAdapter` containing its DUT and result types,
generated signal metadata, binding call, testbench registration call, result
name, and timeout policy. `CPPTB_DEFINE_DPI_RUNTIME(Adapter)` provides the C
entry points. No design transport needs to copy open arrays, decode events, or
format a result line.

The hot scheduler step receives only the compact observed-word array. Driven
words are fetched through a separate idempotent output-pull export on
initialization or after `STEP_OUTPUTS_CHANGED`, so unchanged steps do not carry
an output argument through the simulator ABI.

`deposit()` performs the underlying SystemVerilog blocking assignment
immediately. It does not insert a scheduler delay or observation phase;
testbench code uses an explicit `co_await Delay{...}` when downstream RTL must
evaluate before observation.

## Current scope

The end-to-end test suite currently targets Verilator. Scalar signal values use
`uint32_t`; packed values use `uint64_t` through 64 bits and `Bits<W>` above 64
bits. Generated DPI bindings support fixed multidimensional unpacked arrays,
packed enum and struct views, wide values, fixed-point helpers, and generated
hierarchical probes with read, deposit, force, and release operations.
Four-state X/Z propagation remains deferred. The generated
transport uses standard SystemVerilog DPI, but additional simulator backends
have not yet passed the conformance suite.

The Authoring Core sources currently present under
`benchmarks/authoring_core/` exercise typed tasks, cycle waits, edge timeouts,
predicate waits, events, bounded queues, locks, semaphores, wide packed
signals, fixed-point arithmetic, fixed unpacked arrays, and a synchronous
memory front door. Its C++ DPI testbench is
`benchmarks/authoring_core/testbenches/cpp_dpi/testbench.cpp`, the corresponding pure-SV
source is `benchmarks/authoring_core/testbenches/systemverilog/authoring_core_sv_tb.sv`, and the
shared workload contract is `benchmarks/authoring_core/workload.py`. Runtime
API tests are in `tests/unit/coro_runtime_test.cpp`.
