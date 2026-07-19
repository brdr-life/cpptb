#!/usr/bin/env python3
"""Serially benchmark the oracle-equivalent cpptb and pure-SV AES suites."""

from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path

from run_equivalence import (
    BENCH_DIR,
    BUILD_DIR,
    CPPTB_BINARY,
    CPPTB_MODES,
    EXPECTED_CASES,
    SV_BINARY,
    parse_result,
)


ROOT = BENCH_DIR.parents[2]
DEFAULT_MAX_NORMALIZED_LOAD_1M = 0.30


def load_snapshot(
    load_average: tuple[float, float, float] | None = None,
    logical_cpu_count: int | None = None,
) -> dict[str, object]:
    load_average = load_average or os.getloadavg()
    logical_cpu_count = logical_cpu_count or os.cpu_count() or 1
    return {
        "logical_cpu_count": logical_cpu_count,
        "load_average": list(load_average),
        "normalized_load_1m": load_average[0] / logical_cpu_count,
    }


def assess_load(
    samples: list[dict[str, object]], max_normalized_load_1m: float
) -> dict[str, object]:
    maximum = max(float(sample["normalized_load_1m"]) for sample in samples)
    return {
        "status": "pass" if maximum <= max_normalized_load_1m else "fail",
        "maximum_normalized_load_1m": maximum,
        "max_normalized_load_1m": max_normalized_load_1m,
    }


def write_report(report: dict[str, object]) -> None:
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    (BUILD_DIR / "benchmark-latest.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )


def run_timed(command: list[str], env: dict[str, str]) -> tuple[float, dict[str, int]]:
    started = time.perf_counter()
    completed = subprocess.run(
        command,
        cwd=ROOT,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    elapsed = time.perf_counter() - started
    if completed.returncode:
        tail = "\n".join(completed.stdout.splitlines()[-40:])
        raise RuntimeError(
            f"command failed ({completed.returncode}): {' '.join(command)}\n{tail}"
        )
    return elapsed, parse_result(completed.stdout)


def validate_pair(
    cpptb: dict[str, int], pure_sv: dict[str, int], repeats: int
) -> None:
    expected_cases = EXPECTED_CASES * repeats
    for name, result in (("cpptb", cpptb), ("pure_sv", pure_sv)):
        if result["suites"] != repeats:
            raise AssertionError(f"{name} reported {result['suites']} suites")
        if result["cases"] != expected_cases or result["checks"] != expected_cases * 4:
            raise AssertionError(f"{name} reported an unexpected workload: {result}")
        if result["failures"] != 0:
            raise AssertionError(f"{name} reported failures: {result}")
    if cpptb["checksum"] != pure_sv["checksum"]:
        raise AssertionError(
            "benchmark checksum mismatch: "
            f"cpptb={cpptb['checksum']:08x} pure_sv={pure_sv['checksum']:08x}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repeats", type=int, default=180)
    parser.add_argument("--runs", type=int, default=6)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--cpptb-mode", choices=CPPTB_MODES, default="regmodel")
    parser.add_argument("--max-ratio", type=float, default=1.10)
    parser.add_argument(
        "--max-normalized-load",
        type=float,
        default=DEFAULT_MAX_NORMALIZED_LOAD_1M,
        help="reject timing evidence when one-minute load / logical CPUs exceeds this value",
    )
    parser.add_argument(
        "--allow-overhead",
        action="store_true",
        help="report a ratio above --max-ratio without failing",
    )
    args = parser.parse_args()
    if args.repeats < 1 or args.max_ratio <= 0 or args.max_normalized_load <= 0:
        parser.error(
            "--repeats, --max-ratio, and --max-normalized-load must be positive"
        )
    if args.runs < 4 or args.runs % 2 != 0:
        parser.error("--runs must be an even number of at least 4")
    if args.repeats > 200:
        parser.error(
            "--repeats must not exceed 200 with the public project's default "
            "simulation-cycle safety limit"
        )

    if not args.skip_build:
        subprocess.run(
            ["make", "-C", str(BENCH_DIR), "cpptb-build", "sv-build"],
            cwd=ROOT,
            check=True,
        )

    base_env = os.environ.copy()
    cpptb_env = base_env | {
        "AES_REGMODEL_REPEATS": str(args.repeats),
        "AES_REGMODEL_MODE": args.cpptb_mode,
    }
    sv_command = [str(SV_BINARY), f"+AES_REGMODEL_REPEATS={args.repeats}"]
    cpptb_command = [str(CPPTB_BINARY)]

    load_samples = [load_snapshot()]
    admission = assess_load(load_samples, args.max_normalized_load)
    if admission["status"] != "pass":
        report = {
            "status": "invalid_environment",
            "reason": "normalized one-minute host load exceeds the admission limit",
            "repeats": args.repeats,
            "cpptb_mode": args.cpptb_mode,
            "runs": args.runs,
            "load_samples": load_samples,
            "load_assessment": admission,
        }
        write_report(report)
        print(
            "AES_REGMODEL_BENCHMARK_ENVIRONMENT_ERROR: "
            f"normalized one-minute load "
            f"{admission['maximum_normalized_load_1m']:.3f} exceeds "
            f"{args.max_normalized_load:.3f}",
            file=sys.stderr,
        )
        return 3

    warm_cpptb = run_timed(cpptb_command, cpptb_env)[1]
    warm_sv = run_timed(sv_command, base_env)[1]
    validate_pair(warm_cpptb, warm_sv, args.repeats)

    timings: dict[str, list[float]] = {"cpptb": [], "pure_sv": []}
    last_results: dict[str, dict[str, int]] = {}
    for run_index in range(args.runs):
        order = ("cpptb", "pure_sv") if run_index % 2 == 0 else ("pure_sv", "cpptb")
        for implementation in order:
            if implementation == "cpptb":
                elapsed, result = run_timed(cpptb_command, cpptb_env)
            else:
                elapsed, result = run_timed(sv_command, base_env)
            timings[implementation].append(elapsed)
            last_results[implementation] = result
        validate_pair(last_results["cpptb"], last_results["pure_sv"], args.repeats)
        load_samples.append(load_snapshot())

    cpptb_median = statistics.median(timings["cpptb"])
    sv_median = statistics.median(timings["pure_sv"])
    ratio = cpptb_median / sv_median
    load_assessment = assess_load(load_samples, args.max_normalized_load)
    environment_valid = load_assessment["status"] == "pass"
    ratio_valid = ratio <= args.max_ratio
    report = {
        "status": (
            "passed"
            if environment_valid and ratio_valid
            else "failed"
            if environment_valid
            else "invalid_environment"
        ),
        "repeats": args.repeats,
        "cpptb_mode": args.cpptb_mode,
        "cases": EXPECTED_CASES * args.repeats,
        "runs": args.runs,
        "cpptb_seconds": timings["cpptb"],
        "pure_sv_seconds": timings["pure_sv"],
        "cpptb_median_seconds": cpptb_median,
        "pure_sv_median_seconds": sv_median,
        "ratio_to_pure_sv": ratio,
        "max_ratio": args.max_ratio,
        "guard_status": "pass" if ratio_valid else "fail",
        "checksum": f"{last_results['cpptb']['checksum']:08x}",
        "load_samples": load_samples,
        "load_assessment": load_assessment,
    }
    write_report(report)
    load_before = load_samples[0]["load_average"]
    load_after = load_samples[-1]["load_average"]
    print(
        "AES_REGMODEL_BENCHMARK_RESULT "
        f"mode={args.cpptb_mode} repeats={args.repeats} "
        f"cases={report['cases']} runs={args.runs} "
        f"cpptb_median_s={cpptb_median:.6f} pure_sv_median_s={sv_median:.6f} "
        f"ratio={ratio:.3f}x checksum={report['checksum']} "
        f"load_before={load_before[0]:.2f}/{load_before[1]:.2f}/{load_before[2]:.2f} "
        f"load_after={load_after[0]:.2f}/{load_after[1]:.2f}/{load_after[2]:.2f}"
    )
    if not environment_valid:
        print(
            "AES_REGMODEL_BENCHMARK_ENVIRONMENT_ERROR: "
            f"normalized one-minute load reached "
            f"{load_assessment['maximum_normalized_load_1m']:.3f}, above "
            f"{args.max_normalized_load:.3f}; timing ratio is diagnostic only",
            file=sys.stderr,
        )
        return 3
    if ratio > args.max_ratio and not args.allow_overhead:
        print(
            "AES_REGMODEL_PERFORMANCE_GUARD_ERROR: "
            f"cpptb/pure-SV ratio {ratio:.3f}x exceeds {args.max_ratio:.3f}x",
            file=sys.stderr,
        )
        return 2
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, RuntimeError) as error:
        print(f"AES_REGMODEL_BENCHMARK_ERROR: {error}", file=sys.stderr)
        sys.exit(1)
