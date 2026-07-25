#!/usr/bin/env python3
import argparse
import inspect
import os
import sys
from pathlib import Path

def _opt_fast() -> str:
    """Match the optimization the other three modes are built with.

    cocotb drives Verilator itself, so without this its model would be built
    at Verilator's default while pure SystemVerilog and the C++ modes are
    built at CPPTB_BENCH_OPT_FAST, biasing the comparison in cpptb's favour.
    """
    return os.environ.get("CPPTB_BENCH_OPT_FAST", "-O3")


try:
    from cocotb.runner import get_runner
except ModuleNotFoundError:
    from cocotb_tools.runner import get_runner


REPO = Path(__file__).resolve().parents[5]
TEST_DIR = Path(__file__).resolve().parent
SUITE_DIR = TEST_DIR.parents[1]
BUILD_ROOT = REPO / "build" / "benchmarks" / "framework_comparison" / "open_cores"
WORKLOADS = ("picorv32_firmware", "secworks_aes128", "ethernet_fcs64")
WORKLOAD_IDS = {name: index for index, name in enumerate(WORKLOADS)}
SOURCES = [
    SUITE_DIR / "third_party" / "picorv32" / "picorv32.v",
    *(SUITE_DIR / "third_party" / "secworks_aes").glob("*.v"),
    SUITE_DIR / "third_party" / "verilog_ethernet" / "lfsr.v",
    SUITE_DIR / "third_party" / "verilog_ethernet" / "axis_eth_fcs.v",
    SUITE_DIR / "rtl" / "open_cores_benchmark_dut.sv",
    SUITE_DIR / "rtl" / "open_cores_benchmark_top.sv",
]


def call_supported(func, **kwargs):
    signature = inspect.signature(func)
    return func(**{key: value for key, value in kwargs.items() if key in signature.parameters})


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--workload", choices=WORKLOADS, default=WORKLOADS[0])
    parser.add_argument("--iters", type=int, default=100)
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--rebuild", action="store_true")
    args = parser.parse_args(argv)
    if args.iters <= 0:
        parser.error("--iters must be positive")

    build_dir = BUILD_ROOT / f"cocotb_{args.workload}"
    environment = {
        "PYTHONPATH": str(TEST_DIR) + os.pathsep + os.environ.get("PYTHONPATH", ""),
        "OPEN_CORE_BENCH_WORKLOAD": args.workload,
        "OPEN_CORE_BENCH_ITERS": str(args.iters),
        "COCOTB_LOG_LEVEL": os.environ.get("COCOTB_LOG_LEVEL", "WARNING"),
        "COCOTB_REDUCED_LOG_FMT": "1",
        "PYTHONDONTWRITEBYTECODE": "1",
    }
    os.environ.update(environment)
    runner = get_runner("verilator")
    call_supported(
        runner.build,
        sources=[str(source) for source in SOURCES],
        hdl_toplevel="open_cores_benchmark_top",
        build_dir=str(build_dir),
        build_args=[
            "-MAKEFLAGS",
            f"OPT_FAST={_opt_fast()}",
            "--timing", "--public-flat-rw",
            f"-DOPEN_CORE_WORKLOAD={WORKLOAD_IDS[args.workload]}",
            "-Wno-TIMESCALEMOD", "-Wno-WIDTH", "-Wno-UNUSEDSIGNAL",
            "-Wno-UNOPTFLAT",
        ],
        always=args.rebuild and not args.no_build,
    )
    if args.build_only:
        return 0
    call_supported(
        runner.test,
        hdl_toplevel="open_cores_benchmark_top",
        test_module="test_open_cores",
        build_dir=str(build_dir),
        test_dir=str(TEST_DIR),
        results_xml=str(build_dir / "results.xml"),
        extra_env=environment,
        waves=False,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
