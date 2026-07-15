#!/usr/bin/env python3
"""Compare timing-phase transport backends against the exact pure-SV twin."""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))

import run_benchmark as benchmark


BACKENDS = {
    "direct": REPO
    / "build/benchmarks/authoring_core/cpp_dpi_timing_phases/Vdpi_authoring_core",
    "portable-vpi": REPO
    / "build/benchmarks/authoring_core/timing_backends/portable_vpi/Vdpi_authoring_core",
    "sv-dpi-inline": REPO
    / "build/benchmarks/authoring_core/timing_backends/sv_dpi_inline/Vdpi_authoring_core",
    "sv-dpi-nba": REPO
    / "build/benchmarks/authoring_core/timing_backends/sv_dpi_nba/Vdpi_authoring_core",
    "sv-dpi-calendar": REPO
    / "build/benchmarks/authoring_core/timing_backends/sv_dpi_calendar/Vdpi_authoring_core",
}
PURE_SV = (
    REPO
    / "build/benchmarks/authoring_core/pure_sv_obj/Vauthoring_core_sv_tb"
)


def sample_runner(binary: Path):
    def run(mode: str, kernel: str, pair: int, iterations: int) -> dict:
        selected = binary if mode == "cpp_dpi" else PURE_SV
        command = [str(selected), f"+AUTHORING_CORE_ITERS={iterations}"]
        if mode == "pure_sv":
            command.append(f"+AUTHORING_CORE_KERNEL={kernel}")
        output, metrics = benchmark.run_command(
            command, include_resource_metrics=True
        )
        assert isinstance(metrics, dict)
        result = benchmark.parse_result(
            output,
            expected_mode=mode,
            expected_kernel=kernel,
            expected_iterations=iterations,
        )
        benchmark.validate_contract(result)
        return {
            **result,
            "pair": pair,
            **metrics,
            "binary": str(selected),
            "binary_sha256": benchmark.binary_sha256(selected),
        }

    return run


def semantic_probe(name: str, binary: Path, iterations: int) -> dict[str, object]:
    completed = subprocess.run(
        [str(binary), f"+AUTHORING_CORE_ITERS={iterations}"],
        cwd=REPO,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    result: dict[str, object] = {
        "backend": name,
        "returncode": completed.returncode,
        "status": "failed" if completed.returncode else "passed",
    }
    try:
        parsed = benchmark.parse_result(
            completed.stdout,
            expected_mode="cpp_dpi",
            expected_kernel="timing_phases",
            expected_iterations=iterations,
        )
        result["failures"] = parsed["failures"]
        result["checks"] = parsed["checks"]
        result["sim_cycles"] = parsed["sim_cycles"]
        benchmark.validate_contract(parsed)
    except ValueError as error:
        result["status"] = "failed"
        result["error"] = str(error)
    return result


def compare(name: str, binary: Path, iterations: int, pairs: int) -> dict[str, object]:
    probe = semantic_probe(name, binary, min(iterations, 1000))
    if probe["status"] != "passed":
        return {"semantic": probe, "benchmark": None}

    summaries, _ = benchmark.run_comparison(
        ["timing_phases"], iterations, pairs, sample_runner(binary)
    )
    summary = summaries["timing_phases"]
    guard = summary["guard"]
    return {
        "semantic": probe,
        "benchmark": {
            "pairs": guard["measured_pairs"],
            "cpp_median_ms": statistics.median(
                sample["process_wall_ms"] for sample in summary["cpp_dpi"]
            ),
            "pure_sv_median_ms": statistics.median(
                sample["process_wall_ms"] for sample in summary["pure_sv"]
            ),
            "ratio": guard["ratio"],
            "overhead_percent": guard["overhead_percent"],
            "dpi_first_ratio": guard["dpi_first_paired_median"],
            "sv_first_ratio": guard["sv_first_paired_median"],
            "independent_ratio": guard["independent_median_ratio"],
            "status": guard["status"],
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iters", type=int, default=100_000)
    parser.add_argument("--pairs", type=int, default=16)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument(
        "--backend",
        action="append",
        choices=tuple(BACKENDS),
        help="backend to measure; repeat to select multiple (default: all)",
    )
    args = parser.parse_args()
    if not args.skip_build:
        subprocess.run(
            ["make", "authoring-core-timing-experiments-build"],
            cwd=REPO,
            check=True,
        )
    selected = args.backend or list(BACKENDS)
    result = {
        "iterations": args.iters,
        "requested_pairs": args.pairs,
        "pure_sv_binary": str(PURE_SV),
        "backends": {
            name: compare(name, BACKENDS[name], args.iters, args.pairs)
            for name in selected
        },
    }
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
