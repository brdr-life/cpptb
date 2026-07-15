# Portable timing backend experiments

This experiment asks whether `ReadWrite`, `ReadOnly`, and `NextTimeStep` can
be scheduled through standard SystemVerilog DPI without using VPI. It compares
five transports with the exact `timing_phases` C++ and pure-SystemVerilog
testbench pair.

## Reproduce

Build every transport:

```sh
make authoring-core-timing-experiments-build
```

Run the serialized semantic probes and paired measurements:

```sh
UV_CACHE_DIR=build/uv-cache uv run --frozen \
  python benchmarks/authoring_core/run_timing_backend_experiments.py \
  --iters 100000 --pairs 16
```

The runner probes semantics before benchmarking. A backend with incorrect
results is reported but not timed as a valid implementation.

## Designs

### Inline SV-DPI phase pump

The C++ DPI step result includes bits for pending timing phases. The generated
SV wrapper immediately calls back into C++ for each requested phase. This does
not give the DUT an evaluation-region boundary between a testbench write and
`ReadOnly`, so settled-value observations are incorrect. The 1,000-iteration
probe fails all 1,000 settled-value checks.

### NBA-barrier SV-DPI phase pump

The generated wrapper toggles a private token with a nonblocking assignment
and waits for that token before dispatching `ReadWrite` or `ReadOnly`. Clocks,
framework timers, and generated observers notify the pump when a new known
time step starts. This variant passes the complete strengthened timing
conformance suite under Verilator 5.050.

This mechanism has an important portability boundary: DPI cannot ask a
simulator for its next scheduled event. `NextTimeStep` therefore sees only
events represented in the generated wrapper. Arbitrary internal DUT delays
that are neither exported nor observed are outside its event calendar.

### Centralized generated SV-DPI calendar

The generated wrapper replaces the independent clock, timer, and phase-pump
processes with one calendar owner for all compile-time-discovered generated
clocks. At each known timestamp it dispatches `NextTimeStep`, services a due
framework timer, drives every due generated clock, and then drains `ReadWrite`
and `ReadOnly` through the same NBA settle barrier used by the valid phase
pump. Timers are deliberately serviced before coincident clock edges to retain
the scheduler's established ordering contract.

External and DUT-driven clocks still use thin generated observers because the
wrapper does not own their timing. An earlier timer scheduled by one of those
observers can wake the calendar through the existing timer-kick path. Calendar
code is guarded by `CPPTB_SV_DPI_CALENDAR_TIMING`, so builds that do not select
the experiment compile it out.

This is a generated calendar, not a simulator-wide event queue. It knows about
generated clocks, framework timers, and explicitly observed external signals.
Standard DPI still cannot discover arbitrary internal DUT events, so complete
`NextTimeStep` portability would require either broader generated observation
or a simulator callback API.

## July 14, 2026 result

All measurements were serialized and alternated C++/SV execution order. The
valid backends used 100,000 iterations. The runner requested 16 initial pairs;
the benchmark continuation policy expanded both failing portable rows to 32
measured pairs before confirming the hard failure.

| Backend | Checks | C++ median | SV median | Ratio | Decision |
|---|---:|---:|---:|---:|---|
| Direct Verilator | Pass | 81.3 ms | 98.2 ms | 0.831x | Retain |
| Portable VPI | Pass | 121.1 ms | 93.0 ms | 1.303x | Portable fallback |
| SV-DPI inline | Fail | - | - | - | Reject semantics |
| SV-DPI NBA | Pass | 125.4 ms | 92.5 ms | 1.356x | Reject performance |
| SV-DPI calendar | Pass | 90.2 ms | 92.2 ms | 0.976x | Promising experiment |

The centralized calendar reduced process-wall time by about 28% relative to
the NBA pump and passed the `1.10x` hard guard. It remained about 18% slower
than the Verilator-specific direct path. The result establishes a credible
pure-DPI architecture for the generated event set; it does not yet establish
cross-simulator portability, which must be tested before promotion to a
supported backend.
