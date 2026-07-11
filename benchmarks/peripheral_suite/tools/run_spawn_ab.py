#!/usr/bin/env python3
"""Reproducible tracked-vs-detached DPI spawn A/B benchmark."""

import argparse
import json
import sys
from pathlib import Path


BENCH_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BENCH_DIR))
import run_benchmark as benchmark  # noqa: E402


DEFAULT_OUTPUT = benchmark.RESULT_DIR / "spawn_ab.json"


def collect_spawn_pairs(iters, runs, sample_runner=None):
    if runs < benchmark.MIN_COMPARISON_PAIRS:
        raise SystemExit(
            f"spawn A/B runs must be at least {benchmark.MIN_COMPARISON_PAIRS}"
        )
    sample_runner = sample_runner or benchmark.run_cpp_dpi_sample

    tracked_warmup = sample_runner(0, iters, "tracked")
    detached_warmup = sample_runner(0, iters, "detached")
    tracked_warmup["run"] = 0
    detached_warmup["run"] = 0
    benchmark.assert_same_workload(
        {"tracked": [tracked_warmup], "detached": [detached_warmup]}
    )

    tracked_samples = []
    detached_samples = []
    for run_id in range(1, runs + 1):
        if run_id % 2:
            tracked = sample_runner(run_id, iters, "tracked")
            detached = sample_runner(run_id, iters, "detached")
            order = ["tracked", "detached"]
        else:
            detached = sample_runner(run_id, iters, "detached")
            tracked = sample_runner(run_id, iters, "tracked")
            order = ["detached", "tracked"]
        tracked["pair_order"] = order
        detached["pair_order"] = order
        tracked_samples.append(tracked)
        detached_samples.append(detached)

    benchmark.assert_same_workload(
        {"tracked": tracked_samples, "detached": detached_samples}
    )
    return tracked_samples, detached_samples


def make_result(iters, runs, tracked_samples, detached_samples, argv=None):
    tracked = benchmark.summarize("tracked", tracked_samples)
    detached = benchmark.summarize("detached", detached_samples)
    comparison = benchmark.paired_ratio_statistics(
        detached,
        tracked,
        "detached",
        "tracked",
    )
    comparison["uncertainty"] = {
        "one_sided_95_upper_median_bound": comparison[
            "one_sided_95_upper_median_bound"
        ],
        "two_sided_95_median_ci": comparison["two_sided_95_median_ci"],
        "direction": comparison["direction"],
    }
    config = {
        "iterations": iters,
        "runs": runs,
        "warmup_runs_per_mode": 1,
        "pair_order": "tracked-first on odd runs, detached-first on even runs",
        "binary": str(benchmark.CPP_DPI_BINARY),
        "detached_environment": {"CPPTB_BENCH_DETACHED_SPAWN": "1"},
        "build_command": ["make", "peripheral-suite-dpi-build"],
    }
    return {
        "benchmark": "cpp_dpi_tracked_vs_detached_spawn",
        "iterations": iters,
        "runs": runs,
        "metadata": benchmark.collect_metadata(config, argv=argv),
        "tracked": tracked,
        "detached": detached,
        "comparison": comparison,
    }


def _parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--iters", type=int, default=benchmark.DEFAULT_ITERATIONS)
    parser.add_argument(
        "--runs", type=int, default=benchmark.MIN_COMPARISON_PAIRS
    )
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args(argv)
    if args.iters <= 0:
        parser.error("--iters must be greater than zero")
    if args.runs < benchmark.MIN_COMPARISON_PAIRS:
        parser.error(f"--runs must be at least {benchmark.MIN_COMPARISON_PAIRS}")
    return args


def main(argv=None):
    args = _parse_args(argv)
    if not args.skip_build:
        benchmark.run_command(["make", "peripheral-suite-dpi-build"])

    tracked_samples, detached_samples = collect_spawn_pairs(args.iters, args.runs)
    command = [sys.executable, str(Path(__file__).resolve())]
    if argv is None:
        command.extend(sys.argv[1:])
    else:
        command.extend(argv)
    result = make_result(
        args.iters,
        args.runs,
        tracked_samples,
        detached_samples,
        argv=command,
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    comparison = result["comparison"]
    interval = comparison["two_sided_95_median_ci"]
    print(
        f"detached/tracked median: {comparison['ratio']:.3f}x; "
        f"two-sided 95% median CI: [{interval['lower']:.3f}, "
        f"{interval['upper']:.3f}]x; direction: {comparison['direction']}"
    )
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
