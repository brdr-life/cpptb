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


REPO = Path(__file__).resolve().parents[3]
BENCH_DIR = Path(__file__).resolve().parents[1]
TEST_DIR = Path(__file__).resolve().parent
RTL_DIR = BENCH_DIR / "rtl"
BUILD_DIR = REPO / "build" / "benchmarks" / "peripheral_suite" / "cocotb"

SOURCES = [
    RTL_DIR / "apb_timer_regs_core_pkg.sv",
    RTL_DIR / "apb_timer_regs_core.sv",
    RTL_DIR / "timer_peakrdl.sv",
    RTL_DIR / "apb_timer_peakrdl.sv",
    RTL_DIR / "apb_spi_master_regs_core_pkg.sv",
    RTL_DIR / "apb_spi_master_regs_core.sv",
    RTL_DIR / "spi_master_apb_if_peakrdl.sv",
    RTL_DIR / "apb_i2c_regs_core_pkg.sv",
    RTL_DIR / "apb_i2c_regs_core.sv",
    RTL_DIR / "i2c_master_bit_ctrl.sv",
    RTL_DIR / "i2c_master_byte_ctrl.sv",
    RTL_DIR / "apb_i2c_peakrdl.sv",
    RTL_DIR / "peripheral_suite_dut.sv",
    TEST_DIR / "peripheral_suite_cocotb_top.sv",
]

BUILD_ARGS = [
    "--public-flat-rw",
    "--no-timing",
    "-Wno-MULTIDRIVEN",
    "-Wno-TIMESCALEMOD",
    "-Wno-WIDTHTRUNC",
    "-Wno-WIDTHEXPAND",
    f"-I{RTL_DIR}",
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
    os.environ["PERIPHERAL_SUITE_ITERS"] = str(args.iters)
    os.environ.setdefault("COCOTB_LOG_LEVEL", "WARNING")
    os.environ.setdefault("COCOTB_REDUCED_LOG_FMT", "1")
    os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")

    runner = get_runner("verilator")

    call_supported(
        runner.build,
        sources=[str(source) for source in SOURCES],
        hdl_toplevel="cocotb_peripheral_suite",
        build_dir=str(BUILD_DIR),
        build_args=BUILD_ARGS,
        always=args.rebuild and not args.no_build,
    )

    if args.build_only:
        return 0

    call_supported(
        runner.test,
        hdl_toplevel="cocotb_peripheral_suite",
        test_module="test_peripheral_suite",
        build_dir=str(BUILD_DIR),
        test_dir=str(TEST_DIR),
        results_xml=str(BUILD_DIR / "results.xml"),
        extra_env={
            "PYTHONPATH": os.environ["PYTHONPATH"],
            "PERIPHERAL_SUITE_ITERS": str(args.iters),
            "COCOTB_LOG_LEVEL": os.environ["COCOTB_LOG_LEVEL"],
            "COCOTB_REDUCED_LOG_FMT": os.environ["COCOTB_REDUCED_LOG_FMT"],
            "PYTHONDONTWRITEBYTECODE": os.environ["PYTHONDONTWRITEBYTECODE"],
        },
        waves=False,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
