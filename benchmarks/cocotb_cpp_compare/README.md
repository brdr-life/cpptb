# cocotb vs C++ Coroutine Benchmark

This benchmark compares a cocotb testbench with the C++ coroutine/VPI model in
`cpptb/`.

The DUT is the RgGen/PeakRDL APB event unit copied under
`cpptb/rggen_apb_event/rtl/`. The benchmark repeatedly drives:

- APB register reads and writes.
- IRQ enable, pending, set, and clear behavior.
- Event enable, pending, and clear behavior.
- Periodic sleep and wake flows.
- Concurrent IRQ and sleep-output monitors.

Run:

```sh
python3 benchmarks/cocotb_cpp_compare/run_benchmark.py --iters 1000 --runs 3
```

The runner invokes cocotb through `uv run --python /opt/homebrew/bin/python3.12
--with cocotb ...` because cocotb 2.0 does not support Python 3.14. Override
that with `COCOTB_BENCH_PYTHON=/path/to/python` if needed.

The runner builds the C++ benchmark and the cocotb/Verilator simulation once,
then times repeated runs. It reports:

- Internal wall time printed by the benchmark itself.
- Whole-process wall time measured by the outer runner.

Results are written to:

- `benchmarks/cocotb_cpp_compare/results/latest.json`
- `benchmarks/cocotb_cpp_compare/results/latest.md`
