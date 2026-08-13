# Waveforms

Every cpptb-build project can dump waves. Tracing is a build variant — the
instrumentation costs simulation speed even when nothing is being dumped —
so it is off by default and enabled with one flag:

```sh
cpptb test --wave --project examples/fifo_scoreboard
```

Each test writes one wave file next to its result, and the summary line
names it:

```
PASS  fifo_test checks=27 seed=1 wall_ms=0.280 wave=.../results/fifo_test.fst
```

FST is the default format (small, fast, opens in GTKWave and Surfer). Pass
a format to override, or pin the setting in `cpptb.toml` for a debug
session:

```sh
cpptb test --wave vcd ...
```

```toml
[build]
wave = true        # or "fst" / "vcd"
```

The framework host loop — linked into every build by the default
`timing_backend` — owns the dump points: one sample per timestep, after that
timestep's phases settle. Both backends dump identically.

Running a built binary by hand works without the runner: the host loop
reads `CPPTB_WAVE`:

```sh
CPPTB_WAVE=debug.fst ./build/cpptb/stream_fifo/obj/Vdpi_stream_fifo
```

Without the environment variable an instrumented build runs silently; a
non-wave build ignores the variable entirely. Toggling `--wave` changes the
build fingerprint, so the model rebuilds cleanly in either direction.

Structs and parameters are traced readably (`--trace-structs`
`--trace-params`), the full hierarchy is captured, and the trace flushes
periodically plus on `$stop`/`$fatal`, so a failing test keeps its wave up
to the failure.

Open the file in any FST/VCD viewer — GTKWave and Surfer both read FST
directly. One deliberate non-feature: a failure does not dump a wave by
itself. Waves are asked for, never implied by a result — rerunning the
failing test with `--wave` (the seed is in the result JSON) is the intended
workflow, and automating that rerun is
[deliberately parked](roadmap.md#7-debugging-and-release-tooling).

## Wave equivalence against pure SystemVerilog

`make wave-equivalence-test` is the proof the dumps mean what they say: it
runs four examples twice each — once as the pure-SV twin (`--trace` +
`$dumpvars` under a compile-time define), once through
`cpptb test --wave vcd` — and compares each pair of dumps with
`tools/wave_compare.py`, which samples every signal under the DUT
instance after each rising clock edge and requires the state trajectories
to be identical, cycle for cycle. The five compared pairs cover different
design classes: a plain sequential counter, a ready/valid FIFO with
backpressure, a register file behind the APB components, and a dual-clock
mailbox compared on both of its domains:

```
WAVE_COMPARE status=equal cycles=11  signals=4  a_cycles=11  b_cycles=11
WAVE_COMPARE status=equal cycles=44  signals=17 a_cycles=44  b_cycles=44
WAVE_COMPARE status=equal cycles=80  signals=16 a_cycles=80  b_cycles=80
WAVE_COMPARE status=equal cycles=172 signals=20 a_cycles=172 b_cycles=172
WAVE_COMPARE status=equal cycles=115 signals=20 a_cycles=115 b_cycles=115
```

This is a stronger check than the count-based equivalence gates: it
compares the design's whole per-cycle state, not summary totals, and it is
deliberately blind to sub-cycle testbench mechanics (a twin's `#1ps`, a
deferred flush) exactly as the design is. The comparator is standard
library only; point it at any two VCDs, the scope of the common instance
in each, and the clock:

```sh
python3 tools/wave_compare.py \
    --a a.vcd --a-scope dpi_stream_fifo.i_dut \
    --b b.vcd --b-scope stream_fifo_sv_tb.i_dut \
    --clock-signal clk --min-cycles 30
```

## Backend identity

`make backend-equivalence-test` pins a stronger claim in the other
direction: the two supported timing backends produce **identical**
dumps, not merely equivalent ones. The same four examples build once
per `timing_backend` — `verilator-direct` and `vpi` — with the same
stimulus, and `tools/backend_compare.py` requires each pair of runs to
match exactly:

- the result records agree field for field, including
  `simulation_time_fs` and every check count (`wall_time_ns`, which
  measures the host, is the one exclusion);
- `.vcd` dumps are byte-identical;
- `.fst` dumps are byte-identical outside the format's header date
  field, the one place the writer records wall-clock time. The counter
  pair repeats in fst to keep both wave formats covered.

Byte identity means the backends schedule the same evals at the same
simulation times — a scheduling divergence cannot hide between clock
edges the way it could under the cycle-sampled comparison above. A
waveform debugged under the fast backend is the waveform the portable
backend produces.

Runtime start/stop windowing and scope selection remain on the
[roadmap](roadmap.md#7-debugging-and-release-tooling); the whole-run dump
is deliberately the only mode for now.
