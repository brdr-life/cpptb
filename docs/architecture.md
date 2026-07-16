# Architecture

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
