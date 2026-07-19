#!/usr/bin/env python3
"""Compare the upstream AES oracle with the cpptb and pure-SV peers."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path


BENCH_DIR = Path(__file__).resolve().parent
ROOT = BENCH_DIR.parents[2]
BUILD_DIR = ROOT / "build/benchmarks/regmodel_ground_truth/secworks_aes"
ORACLE_BINARY = BUILD_DIR / "oracle_obj/Vtb_aes"
CPPTB_BINARY = BUILD_DIR / "cpptb/secworks_aes_regmodel/obj/Vdpi_secworks_aes_regmodel"
SV_BINARY = BUILD_DIR / "systemverilog_obj/Vsecworks_aes_repeated_tb"
TRACE_PREFIX = "AES_BUS "
EXPECTED_CASES = 20
EXPECTED_EVENTS = 720
EXPECTED_CHECKSUM = 0x46264475
CPPTB_MODES = ("regmodel", "master", "fused")
RESULT_RE = re.compile(
    r"(?:AES_REGMODEL_RESULT|SV_AES_REGMODEL_RESULT) "
    r"suites=(?P<suites>\d+) cases=(?P<cases>\d+) checks=(?P<checks>\d+) "
    r"checksum=(?P<checksum>[0-9a-fA-F]+)(?: failures=(?P<failures>\d+))?"
)


def run(command: list[str], *, env: dict[str, str] | None = None) -> str:
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if completed.returncode:
        tail = "\n".join(completed.stdout.splitlines()[-40:])
        raise RuntimeError(
            f"command failed ({completed.returncode}): {' '.join(command)}\n{tail}"
        )
    return completed.stdout


def trace_lines(output: str) -> list[str]:
    return [line for line in output.splitlines() if line.startswith(TRACE_PREFIX)]


def parse_result(output: str) -> dict[str, int]:
    match = RESULT_RE.search(output)
    if not match:
        raise AssertionError("missing AES result summary")
    values = {
        key: int(value, 16 if key == "checksum" else 10)
        for key, value in match.groupdict().items()
        if value is not None
    }
    values.setdefault("failures", 0)
    return values


def oracle_checksum(trace: list[str]) -> int:
    checksum = 0x811C9DC5
    reads = 0
    for line in trace:
        if " op=R " not in line:
            continue
        value = int(line.rsplit("data=", 1)[1], 16)
        checksum = ((checksum ^ value) * 0x01000193) & 0xFFFFFFFF
        reads += 1
    if reads != EXPECTED_CASES * 4:
        raise AssertionError(
            f"oracle captured {reads} result reads, expected {EXPECTED_CASES * 4}"
        )
    return checksum


def compare_trace(name: str, oracle: list[str], candidate: list[str]) -> None:
    if oracle == candidate:
        return
    mismatch = next(
        (
            index
            for index, (left, right) in enumerate(zip(oracle, candidate))
            if left != right
        ),
        min(len(oracle), len(candidate)),
    )
    expected = oracle[mismatch] if mismatch < len(oracle) else "<end of trace>"
    actual = candidate[mismatch] if mismatch < len(candidate) else "<end of trace>"
    raise AssertionError(
        f"{name} diverged at event {mismatch}:\n"
        f"  oracle: {expected}\n"
        f"  {name}: {actual}\n"
        f"  event counts: oracle={len(oracle)} {name}={len(candidate)}"
    )


def verify_result(name: str, result: dict[str, int]) -> None:
    expected = {
        "suites": 1,
        "cases": EXPECTED_CASES,
        "checks": EXPECTED_CASES * 4,
        "checksum": EXPECTED_CHECKSUM,
        "failures": 0,
    }
    if result != expected:
        raise AssertionError(f"{name} result {result!r} != {expected!r}")


def build() -> None:
    run(["make", "-C", str(BENCH_DIR), "oracle-build", "cpptb-build", "sv-build"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args()

    if not args.skip_build:
        build()

    oracle_output = run([str(ORACLE_BINARY), "+AES_BUS_TRACE"])
    cpptb_outputs: dict[str, str] = {}
    for mode in CPPTB_MODES:
        cpptb_env = os.environ.copy()
        cpptb_env["AES_REGMODEL_REPEATS"] = "1"
        cpptb_env["AES_REGMODEL_MODE"] = mode
        cpptb_outputs[mode] = run(
            [str(CPPTB_BINARY), "+AES_BUS_TRACE"], env=cpptb_env
        )
    sv_output = run(
        [str(SV_BINARY), "+AES_BUS_TRACE", "+AES_REGMODEL_REPEATS=1"]
    )

    if "*** All 20 test cases completed successfully" not in oracle_output:
        raise AssertionError("upstream oracle did not report all 20 cases passing")

    oracle_trace = trace_lines(oracle_output)
    sv_trace = trace_lines(sv_output)
    if len(oracle_trace) != EXPECTED_EVENTS:
        raise AssertionError(
            f"oracle captured {len(oracle_trace)} events, expected {EXPECTED_EVENTS}"
        )
    cpptb_traces = {
        mode: trace_lines(output) for mode, output in cpptb_outputs.items()
    }
    for mode, trace in cpptb_traces.items():
        compare_trace(f"cpptb_{mode}", oracle_trace, trace)
    compare_trace("pure_sv", oracle_trace, sv_trace)

    checksum = oracle_checksum(oracle_trace)
    if checksum != EXPECTED_CHECKSUM:
        raise AssertionError(
            f"oracle checksum {checksum:08x} != {EXPECTED_CHECKSUM:08x}"
        )
    for mode, output in cpptb_outputs.items():
        verify_result(f"cpptb_{mode}", parse_result(output))
    verify_result("pure_sv", parse_result(sv_output))

    print(
        "AES_EQUIVALENCE_RESULT "
        f"oracle_events={len(oracle_trace)} "
        f"cpptb_modes={len(cpptb_traces)} "
        f"cpptb_events={len(cpptb_traces['regmodel'])} "
        f"sv_events={len(sv_trace)} cases={EXPECTED_CASES} "
        f"checksum={checksum:08x} status=pass"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, RuntimeError) as error:
        print(f"AES_EQUIVALENCE_ERROR: {error}", file=sys.stderr)
        sys.exit(1)
