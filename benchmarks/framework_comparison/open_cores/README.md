# Open-Source Core Comparison

This suite measures the same realistic verification work in pure
SystemVerilog, cpptb through generated DPI, C++ through raw VPI, and Cocotb
through VPI. Each workload elaborates only the selected core.

## Workloads

- `picorv32_firmware` boots a small RV32I firmware image on
  [PicoRV32](https://github.com/YosysHQ/picorv32). The program performs a
  dependency-heavy xorshift loop and stores its result through a memory-mapped
  completion address.
- `secworks_aes128` programs [secworks AES](https://github.com/secworks/aes)
  through its documented 32-bit register interface and validates the four
  AES-128 ECB vectors from NIST SP 800-38A.
- `ethernet_fcs64` drives the 64-bit `axis_eth_fcs` core from
  [verilog-ethernet](https://github.com/alexforencich/verilog-ethernet) with
  64-1518 byte frames, partial final beats, and deterministic valid gaps.

Upstream RTL is pinned with its license under `third_party/`; benchmark-only
wrappers are under `rtl/`.

## Run

Build and run a one-iteration semantic matrix:

```sh
make framework-comparison-open-cores-test
```

Run the workload-specific benchmark defaults:

```sh
make framework-comparison-open-cores-benchmark
```

Run one workload at a chosen size:

```sh
python3 benchmarks/framework_comparison/open_cores/run_benchmark.py \
  --skip-build --workload secworks_aes128 --iters 10000
```

Run only its Cocotb implementation while developing or inspecting the test:

```sh
make framework-comparison-open-cores-cocotb-run \
  OPEN_CORES_COCOTB_WORKLOAD=secworks_aes128 \
  OPEN_CORES_COCOTB_ITERS=100
```

The complete runnable Cocotb bench is
`testbenches/cocotb/test_open_cores.py`. Its three workload coroutines drive
the same RTL wrapper, stimulus, edge sequence, checks, and checksum contract
as the other modes. `testbenches/cocotb/run_cocotb.py` contains only the
Verilator/Cocotb build and launch adapter.

Processes are serialized and mode order rotates over four rounds. A sample is
rejected unless transactions, checks, simulation cycles, checksum, and
failures match across all four modes.
