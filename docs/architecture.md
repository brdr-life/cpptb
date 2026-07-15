# Architecture

The runtime has four layers:

1. `coro_runtime.hpp` owns tasks, processes, waits, timers, events, channels,
   and scheduler ordering.
2. `dpi_runtime.hpp` translates simulator callbacks into scheduler steps and
   batches changed values across the DPI boundary.
3. `cpptb-codegen` elaborates RTL and emits a typed DUT plus transport wrapper.
4. A design adapter registers user testbench processes and defines result and
   timeout policy.

The generated SystemVerilog wrapper owns clocks and simulator callback timing.
The C++ scheduler owns coroutine readiness and cancellation. Signal reads and
writes use generated IDs and typed bindings; no runtime hierarchical-name
lookup is required on the optimized DPI path.

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
