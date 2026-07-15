# Roadmap

The runtime currently supports arbitrary-clock edge waits, absolute delays,
typed tasks, process control, events, channels, two- and four-state values,
wide and fixed-point values, multidimensional arrays, packed views,
hierarchical access, and force/release. Every completed runtime feature has an
equivalent SystemVerilog workload in the authoring benchmark suite.

The API reuses familiar Cocotb timing names only where the behavior matches.
Signal access remains explicit C++ (`get()`, `set()`, `force()`, and
`release()`), and physical time remains an explicit `Delay`.

## 1. Simulator timing model (in progress)

Add the three simulator-phase waits used by Cocotb, backed by standard
simulator callbacks rather than approximate physical delays:

- `ReadWrite{}` resumes after the current evaluation has settled, while writes
  are still legal. It corresponds to a read/write synchronization callback.
- `ReadOnly{}` resumes at the stable end of the current timestep. Reads are
  legal and writes fail with an actionable diagnostic.
- `NextTimeStep{}` resumes at the beginning of the next scheduled timestep. It
  does not imply a one-tick or one-precision-unit delay.

The scheduler must enforce legal phase transitions. In particular, a task
cannot move from `ReadOnly` back to `ReadWrite` in the same timestep. Phase
callbacks must be interest-gated so tests that do not use these waits pay no
callback cost.

Acceptance requires unit and negative tests, multiclock and clockless
conformance cases, runnable usage examples, and an individually runnable C++
DPI benchmark with an exact SystemVerilog twin. The feature benchmark must
remain at or below the project's `1.10x` C++ DPI/SystemVerilog guard unless a
waiver is reviewed and recorded.

## 2. Regression runner

Provide one command that discovers and runs examples, semantic conformance,
negative tests, generated-file checks, and serial performance workloads. It
must retain per-workload results and distinguish correctness failures from
performance guard failures.

## 3. Simulator portability

Run the generated DPI transport and conformance suite on another
standards-compliant simulator. Keep Slang as the frontend, isolate
simulator-specific build invocation, and document capability or scheduling
differences instead of hiding them in testbench code.

## 4. Interfaces and bidirectional signals

Extend generation and typed access to SystemVerilog interfaces, modports,
unpacked interface arrays, and `inout`/tri-state signals. Preserve hierarchical
access and four-state behavior without requiring user-authored binding files.

## 5. Diagnostics and release tooling

Improve failure context, optional waveform control, simulator capability
reporting, generated-file compatibility checks, and versioned package
releases. Keep the core runtime header-only and the design generator separately
installable.

## 6. Coherent clock and reset components

Build reusable clock and reset components on the primitive scheduler API. They
must support multiple unrelated clocks, DUT-generated clocks, cancellation,
and explicit ownership without introducing hidden waits or signal writes.

## 7. Reusable verification components

Extract stable driver, monitor, scoreboard, and transaction patterns without
moving signal activity or time advancement behind surprising convenience
calls. The user-facing operations should continue to show every DUT write,
read, edge wait, and delay.

## Release process

For every runtime stage:

1. Add unit, negative, conformance, and usage tests with the implementation.
2. Add an individually runnable C++ DPI workload and exact SystemVerilog twin.
3. Run semantic parity before performance measurement.
4. Benchmark serially and stop on a valid C++ DPI/SystemVerilog ratio above
   `1.10x`.
5. Run the full affected regression and retain a compact reviewed baseline.
