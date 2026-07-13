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


REPO = Path(__file__).resolve().parents[2]
BENCH_DIR = REPO / "benchmarks" / "cocotb_cpp_compare"
TEST_DIR = BENCH_DIR / "cocotb"
BUILD_DIR = REPO / "build" / "benchmarks" / "cocotb_cpp_compare" / "cocotb"

SOURCES = [
    REPO / "cpptb" / "rggen_apb_event" / "rtl" / "apb_event_service_unit_regs_core_pkg.sv",
    REPO / "cpptb" / "rggen_apb_event" / "rtl" / "apb_event_service_unit_regs_core.sv",
    REPO / "cpptb" / "rggen_apb_event" / "rtl" / "apb_event_sleep_unit_regs_core_pkg.sv",
    REPO / "cpptb" / "rggen_apb_event" / "rtl" / "apb_event_sleep_unit_regs_core.sv",
    REPO / "cpptb" / "rggen_apb_event" / "rtl" / "apb_event_unit_peakrdl.sv",
    REPO / "cpptb" / "rggen_apb_event" / "rtl" / "vpi_apb_event_unit.sv",
]


def call_supported(func, **kwargs):
    signature = inspect.signature(func)
    filtered = {k: v for k, v in kwargs.items() if k in signature.parameters}
    return func(**filtered)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iters", type=int, default=1000)
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--rebuild", action="store_true")
    args = parser.parse_args()

    os.environ["PYTHONPATH"] = (
        str(TEST_DIR) + os.pathsep + os.environ.get("PYTHONPATH", "")
    )
    os.environ["BENCH_ITERS"] = str(args.iters)
    os.environ.setdefault("COCOTB_LOG_LEVEL", "WARNING")
    os.environ.setdefault("COCOTB_REDUCED_LOG_FMT", "1")
    os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")

    runner = get_runner("verilator")

    call_supported(
        runner.build,
        sources=[str(source) for source in SOURCES],
        hdl_toplevel="vpi_apb_event_unit",
        build_dir=str(BUILD_DIR),
        build_args=["--public-flat-rw", "-Wno-MULTIDRIVEN"],
        always=args.rebuild and not args.no_build,
    )

    if args.build_only:
        return 0

    call_supported(
        runner.test,
        hdl_toplevel="vpi_apb_event_unit",
        test_module="test_apb_event_bench",
        build_dir=str(BUILD_DIR),
        test_dir=str(TEST_DIR),
        results_xml=str(BUILD_DIR / "results.xml"),
        extra_env={
            "PYTHONPATH": os.environ["PYTHONPATH"],
            "BENCH_ITERS": str(args.iters),
            "COCOTB_LOG_LEVEL": os.environ["COCOTB_LOG_LEVEL"],
            "COCOTB_REDUCED_LOG_FMT": os.environ["COCOTB_REDUCED_LOG_FMT"],
            "PYTHONDONTWRITEBYTECODE": os.environ["PYTHONDONTWRITEBYTECODE"],
        },
        waves=False,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
