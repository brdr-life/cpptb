#!/usr/bin/env python3
import argparse
import inspect
import os
import sys
from pathlib import Path

try:
    from cocotb.runner import get_runner
except ModuleNotFoundError:
    from cocotb_tools.runner import get_runner


REPO = Path(__file__).resolve().parents[5]
TEST_DIR = Path(__file__).resolve().parent
SUITE_DIR = TEST_DIR.parents[1]
BUILD_DIR = REPO / "build" / "benchmarks" / "framework_comparison" / "heavy_cocotb"
RTL = SUITE_DIR / "rtl" / "heavy_benchmark_dut.sv"
TOP = TEST_DIR / "heavy_benchmark_cocotb_top.sv"
WORKLOADS = ("streaming_fir", "packet_crc32", "matrix4x4")


def call_supported(func, **kwargs):
    signature = inspect.signature(func)
    return func(**{key: value for key, value in kwargs.items() if key in signature.parameters})


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--workload", choices=WORKLOADS, default="streaming_fir")
    parser.add_argument("--iters", type=int, default=1000)
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--rebuild", action="store_true")
    args = parser.parse_args(argv)
    if args.iters <= 0:
        parser.error("--iters must be positive")

    environment = {
        "PYTHONPATH": str(TEST_DIR) + os.pathsep + os.environ.get("PYTHONPATH", ""),
        "HEAVY_BENCH_WORKLOAD": args.workload,
        "HEAVY_BENCH_ITERS": str(args.iters),
        "COCOTB_LOG_LEVEL": os.environ.get("COCOTB_LOG_LEVEL", "WARNING"),
        "COCOTB_REDUCED_LOG_FMT": "1",
        "PYTHONDONTWRITEBYTECODE": "1",
    }
    os.environ.update(environment)
    runner = get_runner("verilator")
    call_supported(
        runner.build,
        sources=[str(RTL), str(TOP)],
        hdl_toplevel="heavy_benchmark_cocotb_top",
        build_dir=str(BUILD_DIR),
        build_args=["--timing", "--public-flat-rw", "-Wno-TIMESCALEMOD", "-Wno-WIDTH", "-Wno-UNUSEDSIGNAL"],
        always=args.rebuild and not args.no_build,
    )
    if args.build_only:
        return 0
    call_supported(
        runner.test,
        hdl_toplevel="heavy_benchmark_cocotb_top",
        test_module="test_heavy_benchmark",
        build_dir=str(BUILD_DIR),
        test_dir=str(TEST_DIR),
        results_xml=str(BUILD_DIR / "results.xml"),
        extra_env=environment,
        waves=False,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
