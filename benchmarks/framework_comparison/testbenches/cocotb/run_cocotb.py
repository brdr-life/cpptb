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


REPO = Path(__file__).resolve().parents[4]
TEST_DIR = Path(__file__).resolve().parent
BUILD_DIR = REPO / "build" / "benchmarks" / "framework_comparison" / "cocotb"
RTL = REPO / "benchmarks" / "authoring_core" / "rtl" / "authoring_core_dut.sv"
TOP = TEST_DIR / "authoring_core_cocotb_top.sv"
SUPPORTED_WORKLOADS = ("control", "wide_echo_137", "signal_edge")


def call_supported(func, **kwargs):
    signature = inspect.signature(func)
    filtered = {key: value for key, value in kwargs.items() if key in signature.parameters}
    return func(**filtered)


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--workload", choices=SUPPORTED_WORKLOADS, default="control")
    parser.add_argument("--iters", type=int, default=1000)
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--rebuild", action="store_true")
    args = parser.parse_args(argv)

    if args.iters <= 0:
        parser.error("--iters must be positive")

    pythonpath = str(TEST_DIR) + os.pathsep + os.environ.get("PYTHONPATH", "")
    environment = {
        "PYTHONPATH": pythonpath,
        "FRAMEWORK_COMPARISON_WORKLOAD": args.workload,
        "FRAMEWORK_COMPARISON_ITERS": str(args.iters),
        "COCOTB_LOG_LEVEL": os.environ.get("COCOTB_LOG_LEVEL", "WARNING"),
        "COCOTB_REDUCED_LOG_FMT": "1",
        "PYTHONDONTWRITEBYTECODE": "1",
    }
    os.environ.update(environment)

    runner = get_runner("verilator")
    call_supported(
        runner.build,
        sources=[str(RTL), str(TOP)],
        hdl_toplevel="authoring_core_cocotb_top",
        build_dir=str(BUILD_DIR),
        build_args=[
            "-MAKEFLAGS",
            f"OPT_FAST={_opt_fast()}",
            "--timing",
            "--public-flat-rw",
            "-Wno-PINMISSING",
            "-Wno-TIMESCALEMOD",
            "-Wno-WIDTH",
            "-Wno-UNUSEDSIGNAL",
        ],
        always=args.rebuild and not args.no_build,
    )

    if args.build_only:
        return 0

    call_supported(
        runner.test,
        hdl_toplevel="authoring_core_cocotb_top",
        test_module="test_authoring_core_comparison",
        build_dir=str(BUILD_DIR),
        test_dir=str(TEST_DIR),
        results_xml=str(BUILD_DIR / "results.xml"),
        extra_env=environment,
        waves=False,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
