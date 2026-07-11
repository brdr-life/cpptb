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
- `cocotb/`: cocotb top-level wrapper, runner, and Python testbench.
- `cpp_vpi/`: C++ coroutine testbench plus a VPI transport/framework.
- `cpp_dpi/`: C++ coroutine testbench, binding manifest, generated DUT adapter,
  and a DPI batched transport/framework.
- `pure_sv/`: pure SystemVerilog testbench baseline.

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

- `cpp_dpi/testbench.cpp`

That file contains the actual reset, stimulus, APB programming, waits, and
checks. It should read like the cocotb version: explicit values are written to
the DUT, APB transactions are visible, and the transport details are absent. Its
only framework include is:

- `cpp_dpi/framework/peripheral_suite.hpp`

The DPI framework hides the scheduler and transport mechanics:

- `cpp_dpi/framework/peripheral_suite_fixture.hpp`
- `cpp_dpi/framework/peripheral_suite_fixture.cpp`
- `cpp_dpi/framework/dpi_transport.cpp`

The design-specific transport wiring is generated from
`cpp_dpi/peripheral_suite.dpi.json` into `cpp_dpi/generated/`. Regenerate it with:

```sh
make peripheral-suite-dpi-codegen
```

`make peripheral-suite-dpi-codegen-check` validates that the generated files
match the manifest and elaborated SystemVerilog ports.

The C++ VPI implementation has the same user-facing sequence shape in:

- `cpp_vpi/testbench.cpp`
- `cpp_vpi/framework/peripheral_suite.hpp`
- `cpp_vpi/framework/verilator_suite_host.cpp`

The cocotb implementation lives in:

- `cocotb/test_peripheral_suite.py`
- `cocotb/run_cocotb.py`
- `cocotb/peripheral_suite_cocotb_top.sv`

The pure SystemVerilog implementation lives in:

- `pure_sv/peripheral_suite_sv_tb.sv`

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
uv run --no-project --python /opt/homebrew/bin/python3.12 --with cocotb \
  python benchmarks/peripheral_suite/cocotb/run_cocotb.py --iters 1000
```

Run the comparison harness:

```sh
python3 benchmarks/peripheral_suite/run_benchmark.py --iters 1000 --runs 15
```

The performance-critical loop contains only C++ DPI and pure SV. It warms both
native binaries, measures 15 adjacent pairs, and alternates which member of a
pair runs first. VPI and cocotb measurements are reported separately so their
startup and host activity cannot interrupt the guard pairs. The guarded metric
is `median(DPI process wall time / SV process wall time)` across those pairs.

A median above `1.10x` is a hard guard failure. When the median passes but the
one-sided 95% upper bound exceeds `1.10x`, the runner collects one additional
15-pair batch. It then evaluates all 30 pairs together; a still-inconclusive
result passes with a warning, while a median above `1.10x` fails immediately.
Workload mismatches, nonzero test failures, and invalid samples also fail
immediately.

The C++ DPI fixture uses tracked `spawn()` by default, so the guard includes
process lifecycle support. `spawn()` and `spawn_detached()` both reclaim root
coroutine frames after completion. Tracked roots additionally retain the
status needed for copied handles, awaiting, and cancellation, with bookkeeping
at root creation and completion rather than per resume.

Results are written to:

- `benchmarks/peripheral_suite/results/latest.json`
- `benchmarks/peripheral_suite/results/latest.md`

Each JSON result should carry the raw ordered pairs and medians plus enough
metadata to reproduce or qualify the run: timestamp, commit and dirty state,
host/OS/CPU, simulator/compiler/Python versions, build mode, command and
iteration count, requested and measured pair counts, warmup policy, spawn
mode, order per pair, confidence bounds, and guard/warning status. The
Markdown result is a compact rendering of that data. Wall-time
ratios near one are noise-sensitive and must not be described as proving that
one implementation is faster.

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
