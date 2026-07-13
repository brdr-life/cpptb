# Roadmap

The first runtime milestone is implemented: arbitrary-clock edge waits,
absolute delays, typed tasks, process control, events, channels, wide and fixed
values, multidimensional arrays, packed views, and hierarchical probe access.
Every completed feature has an equivalent SystemVerilog workload in the
authoring benchmark suite.

## 1. Simulator portability

Run the generated DPI transport and conformance suite on another
standards-compliant simulator. Keep Slang as the frontend, isolate
simulator-specific build invocation, and document any DPI scheduling
differences instead of hiding them in testbench code.

## 2. Four-state values

Add explicit value and validity masks for X/Z-aware scalar and packed access.
Two-state access must remain the compact fast path. Comparisons, formatting,
and conversions need checked behavior rather than silently collapsing unknown
bits.

## 3. Reusable verification components

Extract stable driver, monitor, scoreboard, and transaction patterns without
moving signal activity or time advancement behind surprising convenience
calls. The user-facing operations should continue to show every DUT write,
read, edge wait, and delay.

## 4. Diagnostics and release tooling

Improve failure context, optional waveform control, simulator capability
reporting, generated-file compatibility checks, and versioned package
releases. Keep the core runtime header-only and the design generator separately
installable.

## Release process

For every runtime stage:

1. Add unit, negative, conformance, and usage tests with the implementation.
2. Add an individually runnable C++ DPI workload and exact SystemVerilog twin.
3. Run semantic parity before performance measurement.
4. Benchmark serially and stop on a valid C++ DPI/SystemVerilog ratio above
   `1.10x`.
5. Run the full affected regression and retain a compact reviewed baseline.
