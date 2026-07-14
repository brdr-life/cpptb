# Framework Comparison

This benchmark compares four ways to drive the same Verilated RTL:

- pure SystemVerilog;
- the cpptb C++ DPI framework;
- C++ through standard VPI calls;
- Python through Cocotb and VPI.

The authoring-core comparison covers three distinct workloads:

- `control`: clocked 32-bit request/response traffic;
- `wide_echo_137`: arbitrary-width traffic plus request/response traffic;
- `signal_edge`: synchronization on a DUT-generated response edge.

The generated matrix also incorporates the existing `peripheral_suite`
benchmark, which runs timer, SPI, and I2C drivers plus monitors concurrently.

The separate `heavy_suite` adds a 32-tap streaming FIR, variable-length packet
CRC32, and a block-oriented 4x4 signed matrix accelerator. Those workloads run
independent software reference models in every testbench and are large enough
to amortize simulator process startup:

```sh
make framework-comparison-heavy-benchmark
```

See `heavy_suite/README.md` for its contracts and individual-run commands.

The `open_cores` suite applies the same four-mode contract to pinned upstream
PicoRV32, secworks AES, and verilog-ethernet FCS RTL:

```sh
make framework-comparison-open-cores-benchmark
```

See `open_cores/README.md` for provenance and workload details.

Every implementation uses the same DUT, stimulus function, reset duration,
2 ns clock, edge sequence, 1 ps observation delays, response formula, checksum,
and final counter checks. A timed sample is rejected unless all modes report
identical transactions, checks, simulation cycles, checksum, failures, and
feature counters.

Run the semantic gate only:

```sh
python3 benchmarks/framework_comparison/run_benchmark.py \
  --semantic-only --skip-peripheral
```

Run the complete comparison using the latest validated peripheral result:

```sh
python3 benchmarks/framework_comparison/run_benchmark.py
```

Refresh the peripheral result as part of the run:

```sh
python3 benchmarks/framework_comparison/run_benchmark.py --refresh-peripheral
```

The runner warms every mode, rotates all four modes through each execution
slot, and launches only one simulator process at a time. Results are written
to `benchmarks/framework_comparison/results/latest.json`, `latest.jsonl`, and
`latest.md`.
