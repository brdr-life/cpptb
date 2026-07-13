# Peripheral Suite Benchmark

This benchmark is a heavier pure-SV-vs-DPI-C++-vs-VPI-C++-vs-cocotb comparison
built around three RgGen/PeakRDL-style peripherals behind independent APB buses:

- `apb_timer_peakrdl`: two timer channels with compare and overflow IRQ checks.
- `spi_master_apb_if_peakrdl`: SPI control/status registers, FIFO paths, and
  direct output signal checks.
- `apb_i2c_peakrdl`: OpenCores-derived I2C byte/bit controller with register
  access, command issue, status polling, and pad toggling.

The shared DUT wrapper is `rtl/peripheral_suite_dut.sv`. Every mode instantiates
that same module, then layers its own testbench/framework around it.

## Layout

- `rtl/`: shared peripheral-suite DUT and underlying RTL.
- `testbenches/cocotb/`: cocotb top-level wrapper, runner, and Python testbench.
- `testbenches/cpp_vpi/`: C++ coroutine testbench plus a VPI transport/framework.
- `testbenches/cpp_dpi/`: C++ coroutine testbench, binding manifest, generated DUT adapter,
  and a DPI batched transport/framework.
- `testbenches/systemverilog/`: pure SystemVerilog testbench baseline.

## Testbench Shape

All four implementations run the same logical workload:

- One reset process initializes all APB buses and external pins.
- Three concurrent stimulus sequences run independently:
  - timer programming, compare IRQ wait, overflow IRQ wait;
  - SPI status/clock/configuration/FIFO traffic plus direct output checks;
  - I2C prescaler/control/TX/command traffic plus SDA/SCL pad activity.
- Three concurrent monitors sample timer IRQ shape, SPI handshake outputs, and
  I2C output-valid invariants every clock while stimulus is active.

## User-Facing C++ Testbench

For the C++ DPI framework, the file to read and edit is:

- `testbenches/cpp_dpi/testbench.cpp`

That file contains the actual reset, stimulus, APB programming, waits, and
checks. It should read like the cocotb version: explicit values are written to
the DUT, APB transactions are visible, and the transport details are absent. Its
only framework include is:

- `testbenches/cpp_dpi/framework/peripheral_suite.hpp`

The DPI framework hides the scheduler and transport mechanics:

- `testbenches/cpp_dpi/framework/peripheral_suite_fixture.hpp`
- `testbenches/cpp_dpi/framework/peripheral_suite_fixture.cpp`
- `testbenches/cpp_dpi/framework/dpi_transport.cpp`

The design-specific transport wiring is generated from
`testbenches/cpp_dpi/peripheral_suite.dpi.json` into `testbenches/cpp_dpi/generated/`. Regenerate it with:

```sh
make peripheral-suite-dpi-codegen
```

`make peripheral-suite-dpi-codegen-check` validates that the generated files
match the manifest and elaborated SystemVerilog ports.

The C++ VPI implementation has the same user-facing sequence shape in:

- `testbenches/cpp_vpi/testbench.cpp`
- `testbenches/cpp_vpi/framework/peripheral_suite.hpp`
- `testbenches/cpp_vpi/framework/verilator_suite_host.cpp`

The cocotb implementation lives in:

- `testbenches/cocotb/test_peripheral_suite.py`
- `testbenches/cocotb/run_cocotb.py`
- `testbenches/cocotb/peripheral_suite_cocotb_top.sv`

The pure SystemVerilog implementation lives in:

- `testbenches/systemverilog/peripheral_suite_sv_tb.sv`

## Run

Build and run only the C++ VPI framework version:

```sh
make peripheral-suite-build
PERIPHERAL_SUITE_ITERS=1000 ./build/benchmarks/peripheral_suite/peripheral_suite_host
```

Build and run only the C++ DPI framework version:

```sh
make peripheral-suite-dpi-build
PERIPHERAL_SUITE_ITERS=1000 make peripheral-suite-dpi-run
```

Build and run only the pure SV version:

```sh
make peripheral-suite-sv-build
PERIPHERAL_SUITE_ITERS=1000 make peripheral-suite-sv-run
```

Run the cocotb version directly:

```sh
uv run --cache-dir build/uv-cache --no-project \
  --python /opt/homebrew/bin/python3.12 --with cocotb \
  python benchmarks/peripheral_suite/testbenches/cocotb/run_cocotb.py --iters 1000
```

Run the comparison harness:

```sh
python3 benchmarks/peripheral_suite/run_benchmark.py --iters 1000
```

The performance-critical loop contains only C++ DPI and pure SV. It warms both
native binaries, measures 16 adjacent pairs, and alternates which member of a
pair runs first. VPI and cocotb measurements are reported separately so their
startup and host activity cannot interrupt the guard pairs. The guarded metric
is `median(DPI process wall time / SV process wall time)` across those pairs.
The initial pair count must be even and at least 16.

A median strictly above `1.10x` is a valid hard guard failure only when the
DPI-first and SV-first paired medians are both strictly above `1.05x` and the
ratio of independent process-time medians is within 5% relative of the paired
median. A threshold crossing that does not meet those validity checks is
classified as `invalid_environment`; it never becomes a passing result. Valid
strata never relax a guard failure. Exactly `1.10x` passes the hard threshold.

When the median passes but the one-sided 95% upper bound exceeds `1.10x`, the
runner collects one additional balanced 16-pair batch. It evaluates all 32
pairs together; a still-inconclusive result passes with a warning. Workload
mismatches, nonzero test failures, invalid samples, command failures, hard
failures, and invalid environments exit nonzero after preserving available
evidence.

The C++ DPI fixture uses tracked `spawn()` by default, so the guard includes
process lifecycle support. `spawn()` and `spawn_detached()` both reclaim root
coroutine frames after completion. Tracked roots additionally retain the
status needed for copied handles, awaiting, and cancellation, with bookkeeping
at root creation and completion rather than per resume.

Results are written to:

- `benchmarks/peripheral_suite/results/latest.json`
- `benchmarks/peripheral_suite/results/latest.md`
- `benchmarks/peripheral_suite/results/latest.jsonl`

Every completed sample is appended to the JSONL journal with `flush` and
`fsync`. Final and failure JSON/Markdown files are atomically replaced. Each
JSON result carries the raw ordered pairs and medians plus enough
metadata to reproduce or qualify the run: timestamp, commit and dirty state,
host/OS/CPU, simulator/compiler/Python versions, build mode, command and
iteration count, requested and measured pair counts, warmup policy, spawn
mode, binary SHA256, sequence index, slot, order per pair, order-stratified
medians, independent-median agreement, confidence bounds, and guard/warning
status. The Markdown result is a compact rendering of that data. Wall-time
ratios near one are noise-sensitive and must not be described as proving that
one implementation is faster.

The raw result directory is ignored by Git. Accepted publication summaries are
reviewed separately under `benchmarks/baselines/`.

## C++ DPI A/A Diagnostic

Run the environment diagnostic separately from the benchmark guard:

```sh
python3 benchmarks/peripheral_suite/tools/run_aa.py \
  --iters 10000 --skip-build
```

The A/A runner uses the same C++ DPI binary, argv, tracked-spawn environment,
and workload for labels A and B. It performs one warmup per label followed by
20 balanced adjacent AB/BA pairs. It reports the paired B/A median, exact
two-sided 95% median confidence interval, A-first and B-first strata,
second-slot/first-slot median, order-stratum gap, and half-split drift.

The result passes when the CI contains `1.0`, the paired median is in
`[0.98, 1.02]`, and both strata are in `[0.97, 1.03]`. It fails when the CI
excludes `1.0`, either stratum is outside `[0.95, 1.05]`, or the relative
stratum gap exceeds 5%, the second-slot/first-slot median is outside
`[0.95, 1.05]`, or half-split drift exceeds 5%. Exact 5% boundaries are
accepted. An otherwise inconclusive result collects exactly one additional
balanced 20-pair batch and classifies all 40 pairs. Results are
written atomically to `results/aa_latest.json` and `results/aa_latest.md`, with
incremental samples in `results/aa_latest.jsonl`.

## Tracked/Detached A/B

Run the standalone paired comparison with:

```sh
python3 benchmarks/peripheral_suite/tools/run_spawn_ab.py \
  --iters 10000 --runs 15 --skip-build
```

The tool warms both modes, measures adjacent pairs, alternates which mode runs
first, and writes every raw sample plus uncertainty and environment metadata to
`benchmarks/peripheral_suite/results/spawn_ab.json`. It reports a direction only
when the two-sided median confidence interval excludes `1.0x`.

The I2C RTL includes old-style `#1` simulation delay annotations. This benchmark
uses Verilator `--no-timing` for cocotb and C++ VPI so those annotations do not
change those host event loop models. The pure SV and C++ DPI testbenches use
Verilator `--timing` because their SV clock generators and sampling delays need
simulator time advancement.
