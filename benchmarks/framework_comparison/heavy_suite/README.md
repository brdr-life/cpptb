# Heavy Framework Comparison

This suite compares the same computationally substantial verification work in
four testbench environments:

- pure SystemVerilog;
- cpptb through generated DPI;
- C++ coroutines through raw VPI signal access;
- Cocotb through VPI.

## Workloads

`streaming_fir` drives 100,000 signed samples through a 32-tap FIR. Each
testbench maintains an independent 32-sample history, performs the same 32
software MACs, checks every output, and folds it into a checksum.

`packet_crc32` sends 2,000 packets whose lengths vary from 32 to 95 bytes. The
testbench computes reflected CRC32 byte by byte, drives `last`, checks every
packet result, and verifies the DUT packet counter.

`matrix4x4` loads 2,000 pairs of signed 4x4 matrices, starts the accelerator,
computes all 16 expected outputs in software, and checks both output indices
and data. This is 64 software MACs and 32 checks per block.

The RTL is shared by every mode. The user-facing implementations are:

- `testbenches/cpp_dpi/testbench.cpp`
- `testbenches/systemverilog/heavy_benchmark_sv_tb.sv`
- `testbenches/cpp_vpi/heavy_benchmark_vpi_host.cpp`
- `testbenches/cocotb/test_heavy_benchmark.py`

Transport code, generated DPI bindings, and simulator wrappers are separate
from the C++ DPI testbench.

## Run

Build and execute a three-iteration semantic smoke matrix:

```sh
make framework-comparison-heavy-test
```

Run the workload-specific benchmark defaults:

```sh
make framework-comparison-heavy-benchmark
```

Run one workload or override its size:

```sh
python3 benchmarks/framework_comparison/heavy_suite/run_benchmark.py \
  --skip-build --workload packet_crc32

python3 benchmarks/framework_comparison/heavy_suite/run_benchmark.py \
  --skip-build --workload matrix4x4 --iters 5000
```

Only one simulator process runs at a time. Each mode is warmed once and the
measured order rotates across four rounds. A sample is rejected unless all
four modes report identical iterations, transactions, checks, simulation
cycles, checksum, and failures.

Results are written to `results/latest.json`, `latest.jsonl`, and `latest.md`.
The C++ DPI / pure-SV 1.10x guard is recorded but advisory for this exploratory
suite. Pass `--enforce-guard` when a nonzero exit is required.

The `framework-comparison-heavy-test` target writes its short validation run
under the `smoke` stem so it does not replace the most recent scaled result.
