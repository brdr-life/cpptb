# Queued framework features

This roadmap records the accepted order, scope, and release gates for the next
framework features. Each stage must preserve exact C++ DPI/pure-SystemVerilog
workload parity. A valid paired median above `1.10x` stops the stage for review.

## 1. General signal triggers

Make the existing `RisingEdge`, `FallingEdge`, and `Edge` authoring syntax work
for bound signals beyond configured clocks.

- Preserve configured clocks as the existing fast path.
- Detect testbench-driven scalar transitions in the C++ runtime.
- Generate opt-in observers for DUT outputs and update a persistent per-signal
  interest mask only when scheduler registrations change.
- Support rising/falling waits on one-bit signals and value-change waits on
  scalar integral signals. Reject unsupported or unobserved waits explicitly
  instead of allowing a silent hang.
- Cover multiple waiters, cancellation, `First`, timeout losers, asynchronous
  pulses, same-timestamp transitions, and unspecified cross-signal order.
- Add exact `signal_edge` and idle-observer pure-SV twins. Re-run `control` and
  `all` to measure idle and aggregate cost.

## 2. Fixed multi-dimensional arrays

Generalize the existing fixed one-dimensional array transport without changing
the flat DPI word protocol.

- Preserve every declared range in declaration order.
- Flatten with the last dimension contiguous and increasing numeric indices
  within each dimension.
- Author nested access as `dut.matrix.at(i).at(j).get()` and `.set(value)`.
- Support mixed ascending, descending, and nonzero ranges plus wide elements.
- Add rank-two and rank-three frontend parity, generation, bounds, conformance,
  `array_2d`, and exact pure-SV twin tests.

## 3. Probe force and release

Add force capability independently from deposit capability.

```cpp
dut.internal.state.force(value);
dut.internal.state.release();
```

- Both operations are immediate and insert no delay or evaluation phase.
- Same-callback readback after `force()` observes the forced value.
- The user explicitly awaits an edge or `Delay` for dependent RTL evaluation.
- Support whole scalar and wide packed variables and nets first.
- Spike fixed one-dimensional memory-element force under the installed
  Verilator before committing it. Do not emulate unsupported force behavior.
- Add exact scalar/wide force twins, RTL-write-under-force, repeated force,
  release-without-force, variable retention, and resolved-net release tests.

## 4. Packed enum and struct views

Keep packed transport as canonical raw bits and add frontend-neutral metadata
plus generated zero-cost typed views.

- Preserve packed ranges, struct field names and offsets, and enum names and
  values in the design IR.
- Keep raw-bit access available for invalid enum encodings and interoperability.
- Encode packed struct layout explicitly; never rely on C++ bitfield layout.
- Add Slang/Verilator structural parity and exact field extraction/insertion
  twins after multi-dimensional arrays are stable.
- Defer packed unions and four-state expansion from this milestone.

## Shared release process

For every stage:

1. Review the architecture before editing shared generator protocol code.
2. Add unit, negative, conformance, and usage tests with the implementation.
3. Add an individually runnable C++ DPI kernel and exact pure-SV twin.
4. Run semantic parity before performance measurement.
5. Benchmark serially and stop on a valid ratio above `1.10x`.
6. Run full affected regressions and an independent Fable review before the
   stage is considered complete.
