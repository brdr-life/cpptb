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
timing_backend = "verilator-direct"
wave = true        # or "fst" / "vcd"
```

`wave` requires a `timing_backend`: the framework host loop owns the dump
points — one sample per timestep, after that timestep's phases settle — and
only a timing backend links it. Both backends dump identically.

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

## Wave equivalence against pure SystemVerilog

`make wave-equivalence-test` is the proof the dumps mean what they say: it
runs the fifo_scoreboard example twice — once as the pure-SV twin
(`--trace` + `$dumpvars`), once through `cpptb test --wave vcd` — and
compares the two dumps with `tools/wave_compare.py`, which samples every
signal under the DUT instance after each rising clock edge and requires
the state trajectories to be identical, cycle for cycle:

```
WAVE_COMPARE status=equal cycles=44 signals=9 a_cycles=44 b_cycles=44
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
    --clock clk --min-cycles 30
```

Runtime start/stop windowing and scope selection remain on the
[roadmap](roadmap.md#7-debugging-and-release-tooling); the whole-run dump
is deliberately the only mode for now.
