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

The framework keeps clock-cycle waits and absolute delays separate. Any signal
configured as an observable clock can drive edge waits, while the persistent
timer owner supports clockless and arbitrary multi-clock designs.
