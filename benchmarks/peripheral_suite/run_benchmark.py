#!/usr/bin/env python3
import argparse
import datetime
import json
import math
import os
import platform
import re
import shlex
import shutil
import socket
import statistics
import subprocess
import sys
import time
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
BENCH_DIR = REPO / "benchmarks" / "peripheral_suite"
RESULT_DIR = BENCH_DIR / "results"
MAX_DPI_OVER_SV_PROCESS_RATIO = 1.10
MIN_COMPARISON_PAIRS = 15
DEFAULT_ITERATIONS = 10_000
DEFAULT_SLOW_RUNS = 3
CPP_VPI_BINARY = REPO / "build" / "benchmarks" / "peripheral_suite" / "peripheral_suite_host"
PURE_SV_BINARY = (
    REPO
    / "build"
    / "benchmarks"
    / "peripheral_suite"
    / "pure_sv_obj"
    / "Vperipheral_suite_sv_tb"
)
CPP_DPI_BINARY = (
    REPO
    / "build"
    / "benchmarks"
    / "peripheral_suite"
    / "cpp_dpi_obj"
    / "Vdpi_peripheral_suite"
)
COCOTB_RUNNER = BENCH_DIR / "cocotb" / "run_cocotb.py"
COCOTB_PYTHON = os.environ.get("COCOTB_BENCH_PYTHON", "/opt/homebrew/bin/python3.12")

RESULT_RE = re.compile(r"(?P<name>[A-Z_]+_RESULT)\s+(?P<fields>.*)")


def run_command(command, env=None):
    start = time.perf_counter()
    completed = subprocess.run(
        command,
        cwd=REPO,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    wall_ms = (time.perf_counter() - start) * 1000.0
    if completed.returncode != 0:
        print(completed.stdout)
        raise SystemExit(
            f"command failed with exit {completed.returncode}: {' '.join(map(str, command))}"
        )
    return completed.stdout, wall_ms


def _metadata_command(command):
    try:
        completed = subprocess.run(
            command,
            cwd=REPO,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
    except OSError as error:
        return {"command": list(command), "returncode": None, "output": str(error)}
    return {
        "command": list(command),
        "returncode": completed.returncode,
        "output": completed.stdout.strip(),
    }


def collect_metadata(config, argv=None):
    git_revision = _metadata_command(["git", "rev-parse", "HEAD"])
    git_status = _metadata_command(["git", "status", "--porcelain"])

    compiler_command = shlex.split(os.environ.get("CXX", "c++")) or ["c++"]
    compiler = _metadata_command([*compiler_command, "--version"])
    compiler["executable"] = shutil.which(compiler_command[0])
    compiler["environment"] = {
        name: os.environ[name]
        for name in ("CXX", "CPPFLAGS", "CXXFLAGS", "LDFLAGS")
        if name in os.environ
    }

    verilator = _metadata_command(["verilator", "--version"])
    verilator["executable"] = shutil.which("verilator")
    verilator["root"] = _metadata_command(
        ["verilator", "--getenv", "VERILATOR_ROOT"]
    )["output"]
    verilator["environment"] = {
        name: os.environ[name]
        for name in ("VERILATOR_ROOT", "VERILATOR_FLAGS")
        if name in os.environ
    }

    return {
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "git": {
            "commit": git_revision["output"] if git_revision["returncode"] == 0 else None,
            "dirty": git_status["returncode"] != 0 or bool(git_status["output"]),
        },
        "host": {
            "hostname": socket.gethostname(),
            "platform": platform.platform(),
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "processor": platform.processor(),
        },
        "python": {
            "version": platform.python_version(),
            "implementation": platform.python_implementation(),
            "compiler": platform.python_compiler(),
            "executable": sys.executable,
        },
        "compiler": compiler,
        "verilator": verilator,
        "command": list(argv if argv is not None else [sys.executable, *sys.argv]),
        "config": config,
    }


def parse_result(output, expected_name):
    for line in output.splitlines():
        match = RESULT_RE.search(line)
        if not match or match.group("name") != expected_name:
            continue

        fields = {}
        for item in match.group("fields").split():
            if "=" not in item:
                continue
            key, value = item.split("=", 1)
            try:
                if "." in value:
                    fields[key] = float(value)
                else:
                    fields[key] = int(value)
            except ValueError:
                fields[key] = value
        return fields

    print(output)
    raise SystemExit(f"missing {expected_name} in command output")


def median(values):
    return statistics.median(values) if values else 0.0


def summarize(label, samples):
    summary = {
        "label": label,
        "runs": len(samples),
        "process_wall_ms_median": median(
            [sample["process_wall_ms"] for sample in samples]
        ),
        "samples": samples,
    }
    if samples and all("internal_wall_ms" in sample for sample in samples):
        summary["internal_wall_ms_median"] = median(
            [sample["internal_wall_ms"] for sample in samples]
        )
    return summary


def assert_same_workload(sample_sets):
    if not sample_sets:
        raise SystemExit("no benchmark sample sets were provided")

    run_counts = {mode: len(samples) for mode, samples in sample_sets.items()}
    if not run_counts or any(count == 0 for count in run_counts.values()):
        raise SystemExit(f"benchmark sample sets must be non-empty: {run_counts}")
    if len(set(run_counts.values())) != 1:
        raise SystemExit(f"benchmark run-count mismatch: {run_counts}")

    samples_by_mode = {}
    for mode, samples in sample_sets.items():
        run_ids = [int(sample["run"]) for sample in samples]
        if len(run_ids) != len(set(run_ids)):
            raise SystemExit(f"duplicate run IDs for {mode}: {run_ids}")
        samples_by_mode[mode] = {int(sample["run"]): sample for sample in samples}

    run_id_sets = {mode: set(samples) for mode, samples in samples_by_mode.items()}
    if len({frozenset(run_ids) for run_ids in run_id_sets.values()}) != 1:
        raise SystemExit(f"benchmark run-ID mismatch: {run_id_sets}")

    for run_id in sorted(next(iter(run_id_sets.values()))):
        failures = {
            mode: samples[run_id]["failures"]
            for mode, samples in samples_by_mode.items()
        }
        if any(value != 0 for value in failures.values()):
            raise SystemExit(f"benchmark failures in run {run_id}: {failures}")

        checks = {
            mode: samples[run_id]["checks"]
            for mode, samples in samples_by_mode.items()
        }
        if len(set(checks.values())) != 1:
            raise SystemExit(f"check-count mismatch in run {run_id}: {checks}")

        sim_cycles = {
            mode: int(samples[run_id]["sim_cycles"])
            for mode, samples in samples_by_mode.items()
        }
        if len(set(sim_cycles.values())) != 1:
            raise SystemExit(f"sim-cycle mismatch in run {run_id}: {sim_cycles}")


def _binomial_cdf_half(n, successes):
    if successes < 0:
        return 0.0
    successes = min(successes, n)
    return sum(math.comb(n, value) for value in range(successes + 1)) / (2**n)


def one_sided_upper_median_bound(values, confidence=0.95):
    """Return an exact distribution-free upper confidence bound for a median."""
    if not values:
        raise ValueError("median confidence bound requires at least one value")
    if not 0.0 < confidence < 1.0:
        raise ValueError("confidence must be between zero and one")

    ordered = sorted(float(value) for value in values)
    for order in range(1, len(ordered) + 1):
        coverage = _binomial_cdf_half(len(ordered), order - 1)
        if coverage >= confidence:
            return {
                "confidence": confidence,
                "bound": ordered[order - 1],
                "order_statistic": order,
                "coverage": coverage,
                "method": "exact binomial order-statistic bound",
            }
    return {
        "confidence": confidence,
        "bound": math.inf,
        "order_statistic": None,
        "coverage": 1.0,
        "method": "exact binomial order-statistic bound",
    }


def two_sided_median_confidence_interval(values, confidence=0.95):
    """Return the narrowest central exact distribution-free median interval."""
    if not values:
        raise ValueError("median confidence interval requires at least one value")
    if not 0.0 < confidence < 1.0:
        raise ValueError("confidence must be between zero and one")

    ordered = sorted(float(value) for value in values)
    tail_probability = (1.0 - confidence) / 2.0
    lower_order = 0
    for candidate in range(1, (len(ordered) + 1) // 2 + 1):
        if _binomial_cdf_half(len(ordered), candidate - 1) <= tail_probability:
            lower_order = candidate
        else:
            break

    if lower_order == 0:
        lower = -math.inf
        upper = math.inf
        coverage = 1.0
        upper_order = None
    else:
        lower = ordered[lower_order - 1]
        upper_order = len(ordered) - lower_order + 1
        upper = ordered[upper_order - 1]
        coverage = 1.0 - 2.0 * _binomial_cdf_half(len(ordered), lower_order - 1)

    return {
        "confidence": confidence,
        "lower": lower,
        "upper": upper,
        "lower_order_statistic": lower_order or None,
        "upper_order_statistic": upper_order,
        "coverage": coverage,
        "method": "exact central binomial order-statistic interval",
    }


def _samples_by_run(summary, label):
    samples = summary.get("samples", [])
    run_ids = [int(sample["run"]) for sample in samples]
    if len(run_ids) != len(set(run_ids)):
        raise SystemExit(f"duplicate run IDs in {label} samples: {run_ids}")
    return {int(sample["run"]): sample for sample in samples}


def paired_ratio_statistics(
    numerator_summary,
    denominator_summary,
    numerator_label,
    denominator_label,
):
    numerator_by_run = _samples_by_run(numerator_summary, numerator_label)
    denominator_by_run = _samples_by_run(denominator_summary, denominator_label)
    if not numerator_by_run or numerator_by_run.keys() != denominator_by_run.keys():
        raise SystemExit(
            f"{numerator_label} and {denominator_label} samples must have matching run IDs"
        )

    paired_ratios = []
    for run_id in sorted(numerator_by_run):
        numerator_ms = float(numerator_by_run[run_id]["process_wall_ms"])
        denominator_ms = float(denominator_by_run[run_id]["process_wall_ms"])
        if not math.isfinite(numerator_ms) or numerator_ms < 0:
            raise SystemExit(f"{numerator_label} process time must be finite and non-negative")
        if not math.isfinite(denominator_ms) or denominator_ms <= 0:
            raise SystemExit(
                f"{denominator_label} process time must be finite and greater than zero"
            )
        paired_ratios.append(numerator_ms / denominator_ms)

    interval = two_sided_median_confidence_interval(paired_ratios)
    if interval["upper"] < 1.0:
        direction = f"{numerator_label}_faster"
    elif interval["lower"] > 1.0:
        direction = f"{numerator_label}_slower"
    else:
        direction = "inconclusive"

    return {
        "metric": f"median({numerator_label}_process_wall / {denominator_label}_process_wall by run)",
        "ratio": median(paired_ratios),
        "overhead_percent": (median(paired_ratios) - 1.0) * 100.0,
        "paired_ratios": paired_ratios,
        "one_sided_95_upper_median_bound": one_sided_upper_median_bound(
            paired_ratios
        ),
        "two_sided_95_median_ci": interval,
        "direction": direction,
    }


def check_dpi_vs_pure_sv(
    cpp_dpi_summary,
    pure_sv_summary,
    max_ratio=None,
    final=True,
):
    if max_ratio is None:
        max_ratio = MAX_DPI_OVER_SV_PROCESS_RATIO
    if len(cpp_dpi_summary.get("samples", [])) < MIN_COMPARISON_PAIRS:
        raise SystemExit(
            f"C++ DPI/pure SV guard requires at least {MIN_COMPARISON_PAIRS} paired runs"
        )

    result = paired_ratio_statistics(
        cpp_dpi_summary,
        pure_sv_summary,
        "cpp_dpi",
        "pure_sv",
    )
    result.update(
        {
            "max_ratio": max_ratio,
            "cpp_dpi_process_wall_ms": float(
                cpp_dpi_summary["process_wall_ms_median"]
            ),
            "pure_sv_process_wall_ms": float(
                pure_sv_summary["process_wall_ms_median"]
            ),
        }
    )

    if result["ratio"] > max_ratio:
        raise SystemExit(
            f"paired tracked C++ DPI/pure SV process ratio exceeded {max_ratio:.2f}x: "
            f"median ratio = {result['ratio']:.3f}x "
            f"({result['overhead_percent']:+.2f}%).\n"
            f"Independent medians: {result['cpp_dpi_process_wall_ms']:.3f} ms DPI, "
            f"{result['pure_sv_process_wall_ms']:.3f} ms SV.\n"
            "Stop and consult before adding more framework features."
        )

    upper_bound = result["one_sided_95_upper_median_bound"]["bound"]
    if upper_bound > max_ratio and not final:
        result["status"] = "needs_extra_batch"
    elif upper_bound > max_ratio:
        result["status"] = "passed_inconclusive"
        result["warning"] = (
            f"median tracked C++ DPI/pure SV ratio passes at {result['ratio']:.3f}x, "
            f"but its one-sided 95% upper median bound is {upper_bound:.3f}x"
        )
    else:
        result["status"] = "passed"
    return result


def _dpi_environment(spawn_mode):
    if spawn_mode not in {"tracked", "detached"}:
        raise ValueError(f"unknown DPI spawn mode: {spawn_mode}")
    env = os.environ.copy()
    env.pop("CPPTB_BENCH_DETACHED_SPAWN", None)
    if spawn_mode == "detached":
        env["CPPTB_BENCH_DETACHED_SPAWN"] = "1"
    return env


def run_cpp_dpi_sample(run, iters, spawn_mode="tracked"):
    output, process_ms = run_command(
        [str(CPP_DPI_BINARY), f"+PERIPHERAL_SUITE_ITERS={iters}"],
        env=_dpi_environment(spawn_mode),
    )
    result = parse_result(output, "CPP_DPI_PERIPHERAL_RESULT")
    return {
        "run": run,
        "spawn_mode": spawn_mode,
        "internal_wall_ms": float(result["wall_ms"]),
        "process_wall_ms": process_ms,
        "checks": int(result["checks"]),
        "sim_cycles": int(result["sim_cycles"]),
        "failures": int(result["failures"]),
    }


def run_pure_sv_sample(run, iters):
    output, process_ms = run_command(
        [str(PURE_SV_BINARY), f"+PERIPHERAL_SUITE_ITERS={iters}"]
    )
    result = parse_result(output, "PURE_SV_PERIPHERAL_RESULT")
    return {
        "run": run,
        "process_wall_ms": process_ms,
        "checks": int(result["checks"]),
        "sim_cycles": int(result["sim_cycles"]),
        "failures": int(result["failures"]),
    }


def collect_critical_pairs(
    iters,
    pair_count,
    start_run=1,
    dpi_sample_runner=None,
    sv_sample_runner=None,
):
    dpi_sample_runner = dpi_sample_runner or run_cpp_dpi_sample
    sv_sample_runner = sv_sample_runner or run_pure_sv_sample
    dpi_samples = []
    pure_sv_samples = []

    for offset in range(pair_count):
        run_id = start_run + offset
        if run_id % 2:
            dpi = dpi_sample_runner(run_id, iters, "tracked")
            pure_sv = sv_sample_runner(run_id, iters)
            order = ["cpp_dpi", "pure_sv"]
        else:
            pure_sv = sv_sample_runner(run_id, iters)
            dpi = dpi_sample_runner(run_id, iters, "tracked")
            order = ["pure_sv", "cpp_dpi"]
        dpi["pair_order"] = order
        pure_sv["pair_order"] = order
        dpi_samples.append(dpi)
        pure_sv_samples.append(pure_sv)

    assert_same_workload({"cpp_dpi": dpi_samples, "pure_sv": pure_sv_samples})
    return dpi_samples, pure_sv_samples


def run_critical_comparison(
    iters,
    comparison_runs,
    dpi_sample_runner=None,
    sv_sample_runner=None,
):
    if comparison_runs < MIN_COMPARISON_PAIRS:
        raise SystemExit(
            f"comparison runs must be at least {MIN_COMPARISON_PAIRS}"
        )
    dpi_sample_runner = dpi_sample_runner or run_cpp_dpi_sample
    sv_sample_runner = sv_sample_runner or run_pure_sv_sample

    # Warm both binaries before entering the adjacency-sensitive pair loop.
    dpi_warmup = dpi_sample_runner(0, iters, "tracked")
    sv_warmup = sv_sample_runner(0, iters)
    dpi_warmup["run"] = 0
    sv_warmup["run"] = 0
    assert_same_workload({"cpp_dpi": [dpi_warmup], "pure_sv": [sv_warmup]})

    dpi_samples, pure_sv_samples = collect_critical_pairs(
        iters,
        comparison_runs,
        dpi_sample_runner=dpi_sample_runner,
        sv_sample_runner=sv_sample_runner,
    )
    guard = check_dpi_vs_pure_sv(
        summarize("cpp_dpi", dpi_samples),
        summarize("pure_sv", pure_sv_samples),
        final=False,
    )
    extra_batch_collected = guard["status"] == "needs_extra_batch"
    if extra_batch_collected:
        extra_dpi, extra_sv = collect_critical_pairs(
            iters,
            comparison_runs,
            start_run=comparison_runs + 1,
            dpi_sample_runner=dpi_sample_runner,
            sv_sample_runner=sv_sample_runner,
        )
        dpi_samples.extend(extra_dpi)
        pure_sv_samples.extend(extra_sv)
        guard = check_dpi_vs_pure_sv(
            summarize("cpp_dpi", dpi_samples),
            summarize("pure_sv", pure_sv_samples),
            final=True,
        )

    guard["requested_pairs"] = comparison_runs
    guard["measured_pairs"] = len(dpi_samples)
    guard["extra_batch_collected"] = extra_batch_collected
    return dpi_samples, pure_sv_samples, guard


def run_cpp_vpi_sample(run, iters):
    env = os.environ.copy()
    env["PERIPHERAL_SUITE_ITERS"] = str(iters)
    output, process_ms = run_command([str(CPP_VPI_BINARY)], env=env)
    result = parse_result(output, "CPP_VPI_PERIPHERAL_RESULT")
    return {
        "run": run,
        "internal_wall_ms": float(result["wall_ms"]),
        "process_wall_ms": process_ms,
        "checks": int(result["checks"]),
        "sim_cycles": int(result["sim_cycles"]),
        "failures": int(result["failures"]),
    }


def run_cocotb_sample(run, iters):
    output, process_ms = run_command(
        [
            "uv",
            "run",
            "--no-project",
            "--python",
            COCOTB_PYTHON,
            "--with",
            "cocotb",
            "python",
            str(COCOTB_RUNNER),
            "--iters",
            str(iters),
            "--no-build",
        ]
    )
    result = parse_result(output, "COCOTB_PERIPHERAL_RESULT")
    return {
        "run": run,
        "internal_wall_ms": float(result["wall_ms"]),
        "process_wall_ms": process_ms,
        "checks": int(result["checks"]),
        "sim_cycles": int(result.get("sim_cycles", 0)),
        "failures": int(result["failures"]),
    }


def _parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--iters", type=int, default=DEFAULT_ITERATIONS)
    parser.add_argument("--runs", type=int, default=DEFAULT_SLOW_RUNS)
    parser.add_argument(
        "--comparison-runs",
        type=int,
        default=MIN_COMPARISON_PAIRS,
        help="number of adjacent tracked DPI/SV pairs in the initial guard batch",
    )
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args(argv)
    if args.iters <= 0:
        parser.error("--iters must be greater than zero")
    if args.runs <= 0:
        parser.error("--runs must be greater than zero")
    if args.comparison_runs < MIN_COMPARISON_PAIRS:
        parser.error(f"--comparison-runs must be at least {MIN_COMPARISON_PAIRS}")
    return args


def main(argv=None):
    args = _parse_args(argv)

    build_commands = [
        ["make", "peripheral-suite-build"],
        ["make", "peripheral-suite-sv-build"],
        ["make", "peripheral-suite-dpi-build"],
        [
            "uv",
            "run",
            "--no-project",
            "--python",
            COCOTB_PYTHON,
            "--with",
            "cocotb",
            "python",
            str(COCOTB_RUNNER),
            "--iters",
            str(args.iters),
            "--build-only",
        ],
    ]
    if not args.skip_build:
        for command in build_commands:
            run_command(command)

    cpp_dpi_samples, pure_sv_samples, performance_guard = run_critical_comparison(
        args.iters, args.comparison_runs
    )
    if "warning" in performance_guard:
        print(f"WARNING: {performance_guard['warning']}")

    # Slow integrations are intentionally outside the adjacency-sensitive guard loop.
    cpp_vpi_samples = [
        run_cpp_vpi_sample(run_id, args.iters)
        for run_id in range(1, args.runs + 1)
    ]
    cocotb_samples = [
        run_cocotb_sample(run_id, args.iters)
        for run_id in range(1, args.runs + 1)
    ]
    assert_same_workload({"cpp_vpi": cpp_vpi_samples, "cocotb": cocotb_samples})
    assert_same_workload(
        {
            "cpp_vpi": [cpp_vpi_samples[0]],
            "cpp_dpi": [cpp_dpi_samples[0]],
            "pure_sv": [pure_sv_samples[0]],
            "cocotb": [cocotb_samples[0]],
        }
    )

    cpp_vpi_summary = summarize("cpp_vpi", cpp_vpi_samples)
    cpp_dpi_summary = summarize("cpp_dpi", cpp_dpi_samples)
    pure_sv_summary = summarize("pure_sv", pure_sv_samples)
    cocotb_summary = summarize("cocotb", cocotb_samples)

    cpp_over_dpi_internal = (
        cpp_vpi_summary["internal_wall_ms_median"]
        / cpp_dpi_summary["internal_wall_ms_median"]
    )
    cpp_over_dpi_process = (
        cpp_vpi_summary["process_wall_ms_median"]
        / cpp_dpi_summary["process_wall_ms_median"]
    )
    cocotb_over_cpp_internal = (
        cocotb_summary["internal_wall_ms_median"]
        / cpp_vpi_summary["internal_wall_ms_median"]
    )
    cocotb_over_dpi_internal = (
        cocotb_summary["internal_wall_ms_median"]
        / cpp_dpi_summary["internal_wall_ms_median"]
    )
    cocotb_over_cpp_process = (
        cocotb_summary["process_wall_ms_median"]
        / cpp_vpi_summary["process_wall_ms_median"]
    )
    cocotb_over_dpi_process = (
        cocotb_summary["process_wall_ms_median"]
        / cpp_dpi_summary["process_wall_ms_median"]
    )
    dpi_over_sv_process = performance_guard["ratio"]
    cpp_over_sv_process = (
        cpp_vpi_summary["process_wall_ms_median"]
        / pure_sv_summary["process_wall_ms_median"]
    )
    cocotb_over_sv_process = (
        cocotb_summary["process_wall_ms_median"]
        / pure_sv_summary["process_wall_ms_median"]
    )

    config = {
        "iterations": args.iters,
        "comparison_runs_requested": args.comparison_runs,
        "comparison_runs_measured": len(cpp_dpi_samples),
        "slow_runs": args.runs,
        "tracked_dpi_guard_ratio": MAX_DPI_OVER_SV_PROCESS_RATIO,
        "dpi_spawn_mode": "tracked",
        "skip_build": args.skip_build,
        "build_commands": build_commands,
        "binaries": {
            "cpp_vpi": str(CPP_VPI_BINARY),
            "cpp_dpi": str(CPP_DPI_BINARY),
            "pure_sv": str(PURE_SV_BINARY),
            "cocotb_runner": str(COCOTB_RUNNER),
        },
    }
    result = {
        "iterations": args.iters,
        "runs": args.runs,
        "comparison_runs": len(cpp_dpi_samples),
        "design": "peripheral_suite: APB timer + APB SPI + APB I2C",
        "cpp_dpi_spawn_mode": "tracked",
        "metadata": collect_metadata(config),
        "cpp_vpi": cpp_vpi_summary,
        "cpp_dpi": cpp_dpi_summary,
        "pure_sv": pure_sv_summary,
        "cocotb": cocotb_summary,
        "performance_guard": performance_guard,
        "speedup": {
            "cpp_over_dpi_internal_wall": cpp_over_dpi_internal,
            "cpp_over_dpi_process_wall": cpp_over_dpi_process,
            "cocotb_over_cpp_internal_wall": cocotb_over_cpp_internal,
            "cocotb_over_dpi_internal_wall": cocotb_over_dpi_internal,
            "cocotb_over_cpp_process_wall": cocotb_over_cpp_process,
            "cocotb_over_dpi_process_wall": cocotb_over_dpi_process,
            "dpi_over_systemverilog_process_wall": dpi_over_sv_process,
            "cpp_over_systemverilog_process_wall": cpp_over_sv_process,
            "cocotb_over_systemverilog_process_wall": cocotb_over_sv_process,
        },
    }

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    json_path = RESULT_DIR / "latest.json"
    md_path = RESULT_DIR / "latest.md"
    upper_bound = performance_guard["one_sided_95_upper_median_bound"]["bound"]
    interval = performance_guard["two_sided_95_median_ci"]
    md_path.write_text(
        "\n".join(
            [
                "# Peripheral-suite pure SV vs DPI C++ vs VPI C++ vs cocotb benchmark",
                "",
                f"- Design: `{result['design']}`",
                f"- Iterations per run: `{args.iters}`",
                f"- Critical tracked DPI/SV pairs: `{len(cpp_dpi_samples)}`",
                f"- Slower VPI/cocotb runs: `{args.runs}`",
                "- C++ DPI spawn mode: `tracked`",
                f"- C++ VPI internal median: `{cpp_vpi_summary['internal_wall_ms_median']:.3f} ms`",
                f"- C++ DPI internal median: `{cpp_dpi_summary['internal_wall_ms_median']:.3f} ms`",
                f"- cocotb internal median: `{cocotb_summary['internal_wall_ms_median']:.3f} ms`",
                f"- C++ VPI/C++ DPI internal ratio: `{cpp_over_dpi_internal:.2f}x`",
                f"- cocotb/C++ DPI internal ratio: `{cocotb_over_dpi_internal:.2f}x`",
                f"- cocotb/C++ VPI internal ratio: `{cocotb_over_cpp_internal:.2f}x`",
                f"- C++ VPI process median: `{cpp_vpi_summary['process_wall_ms_median']:.3f} ms`",
                f"- C++ DPI process median: `{cpp_dpi_summary['process_wall_ms_median']:.3f} ms`",
                f"- Pure SV process median: `{pure_sv_summary['process_wall_ms_median']:.3f} ms`",
                f"- cocotb process median: `{cocotb_summary['process_wall_ms_median']:.3f} ms`",
                f"- C++ VPI/C++ DPI process ratio: `{cpp_over_dpi_process:.2f}x`",
                f"- cocotb/C++ DPI process ratio: `{cocotb_over_dpi_process:.2f}x`",
                f"- cocotb/C++ VPI process ratio: `{cocotb_over_cpp_process:.2f}x`",
                f"- C++ DPI/pure SV paired process ratio: `{dpi_over_sv_process:.3f}x`",
                f"- One-sided 95% upper median bound: `{upper_bound:.3f}x`",
                f"- Two-sided 95% median CI: `[{interval['lower']:.3f}, {interval['upper']:.3f}]x`",
                f"- CI direction: `{performance_guard['direction']}`",
                f"- C++ VPI/pure SV process ratio: `{cpp_over_sv_process:.2f}x`",
                f"- cocotb/pure SV process ratio: `{cocotb_over_sv_process:.2f}x`",
                f"- C++ DPI performance guard: `{performance_guard['status']}` "
                f"(`{performance_guard['ratio']:.3f}x` <= "
                f"`{performance_guard['max_ratio']:.2f}x`)",
                "",
                "DPI and pure SV are warmed, measured as adjacent pairs, and run first",
                "in alternating order. Their reported ratio is the median paired ratio.",
                "The slower VPI and cocotb loops run only after the critical comparison.",
                "",
            ]
        )
    )
    json_path.write_text(json.dumps(result, indent=2) + "\n")

    print(md_path.read_text())
    print(f"Wrote {json_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
