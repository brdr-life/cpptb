#!/usr/bin/env python3
import argparse
import datetime
import hashlib
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
import tempfile
import time
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
BENCH_DIR = REPO / "benchmarks" / "peripheral_suite"
RESULT_DIR = BENCH_DIR / "results"
MAX_DPI_OVER_SV_PROCESS_RATIO = 1.10
# Kept for the standalone tracked/detached tool's established API.
MIN_COMPARISON_PAIRS = 15
MIN_CRITICAL_COMPARISON_PAIRS = 16
EXTRA_COMPARISON_PAIRS = 16
MIN_ORDER_STRATUM_FAILURE_RATIO = 1.05
MAX_INDEPENDENT_PAIRED_DISAGREEMENT = 0.05
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


class CommandExecutionError(RuntimeError):
    def __init__(self, command, returncode, output):
        self.command = list(command)
        self.returncode = returncode
        self.output = output
        super().__init__(
            f"command failed with exit {returncode}: {' '.join(map(str, command))}"
        )


class ResultParseError(RuntimeError):
    def __init__(self, expected_name, output):
        self.expected_name = expected_name
        self.output = output
        super().__init__(f"missing {expected_name} in command output")


class SampleJournal:
    def __init__(self, path, truncate=False):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        if truncate:
            with self.path.open("w", encoding="utf-8") as stream:
                stream.flush()
                os.fsync(stream.fileno())

    def append(self, entry):
        with self.path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(entry, sort_keys=True) + "\n")
            stream.flush()
            os.fsync(stream.fileno())


def atomic_write_text(path, text, replace=None):
    """Durably replace path with text using a sibling temporary file."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    replace = replace or os.replace
    temporary_path = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as stream:
            temporary_path = Path(stream.name)
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        replace(temporary_path, path)
    finally:
        if temporary_path is not None and temporary_path.exists():
            temporary_path.unlink()


def atomic_write_json(path, value, replace=None):
    atomic_write_text(path, json.dumps(value, indent=2) + "\n", replace=replace)


def binary_sha256(path):
    path = Path(path)
    try:
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError:
        return None


def run_command(command, env=None):
    start = time.perf_counter()
    try:
        completed = subprocess.run(
            command,
            cwd=REPO,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
    except OSError as error:
        raise CommandExecutionError(command, None, str(error)) from error
    wall_ms = (time.perf_counter() - start) * 1000.0
    if completed.returncode != 0:
        print(completed.stdout)
        raise CommandExecutionError(command, completed.returncode, completed.stdout)
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
    raise ResultParseError(expected_name, output)


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
    try:
        run_ids = [int(sample["run"]) for sample in samples]
    except (KeyError, TypeError, ValueError) as error:
        return None, f"invalid run ID in {label} samples: {error}"
    if len(run_ids) != len(set(run_ids)):
        return None, f"duplicate run IDs in {label} samples: {run_ids}"
    return {int(sample["run"]): sample for sample in samples}, None


def paired_ratio_statistics(
    numerator_summary,
    denominator_summary,
    numerator_label,
    denominator_label,
):
    numerator_by_run, numerator_error = _samples_by_run(
        numerator_summary, numerator_label
    )
    denominator_by_run, denominator_error = _samples_by_run(
        denominator_summary, denominator_label
    )
    error = numerator_error or denominator_error
    if error:
        return {
            "status": "invalid_samples",
            "error": error,
            "paired_ratios": [],
        }
    if not numerator_by_run or numerator_by_run.keys() != denominator_by_run.keys():
        return {
            "status": "invalid_samples",
            "error": (
                f"{numerator_label} and {denominator_label} samples must have "
                "matching run IDs"
            ),
            "paired_ratios": [],
        }

    paired_ratios = []
    for run_id in sorted(numerator_by_run):
        try:
            numerator_ms = float(numerator_by_run[run_id]["process_wall_ms"])
            denominator_ms = float(denominator_by_run[run_id]["process_wall_ms"])
        except (KeyError, TypeError, ValueError) as error:
            return {
                "status": "invalid_samples",
                "error": f"invalid process time in run {run_id}: {error}",
                "paired_ratios": paired_ratios,
            }
        if not math.isfinite(numerator_ms) or numerator_ms < 0:
            return {
                "status": "invalid_samples",
                "error": (
                    f"{numerator_label} process time must be finite and non-negative"
                ),
                "paired_ratios": paired_ratios,
            }
        if not math.isfinite(denominator_ms) or denominator_ms <= 0:
            return {
                "status": "invalid_samples",
                "error": (
                    f"{denominator_label} process time must be finite and greater than zero"
                ),
                "paired_ratios": paired_ratios,
            }
        paired_ratios.append(numerator_ms / denominator_ms)

    interval = two_sided_median_confidence_interval(paired_ratios)
    if interval["upper"] < 1.0:
        direction = f"{numerator_label}_faster"
    elif interval["lower"] > 1.0:
        direction = f"{numerator_label}_slower"
    else:
        direction = "inconclusive"

    return {
        "status": "ok",
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
    sample_count = len(cpp_dpi_summary.get("samples", []))
    if sample_count < MIN_CRITICAL_COMPARISON_PAIRS or sample_count % 2:
        return {
            "status": "invalid_environment",
            "verdict": "invalid_environment",
            "validity": "invalid",
            "error": (
                "C++ DPI/pure SV guard requires an even count of at least "
                f"{MIN_CRITICAL_COMPARISON_PAIRS} paired runs"
            ),
            "requested_pairs": sample_count,
            "measured_pairs": sample_count,
        }

    result = paired_ratio_statistics(
        cpp_dpi_summary,
        pure_sv_summary,
        "cpp_dpi",
        "pure_sv",
    )
    if result["status"] == "invalid_samples":
        result.update(
            {
                "status": "invalid_environment",
                "verdict": "invalid_environment",
                "validity": "invalid",
                "measured_pairs": sample_count,
            }
        )
        return result

    dpi_by_run, _ = _samples_by_run(cpp_dpi_summary, "cpp_dpi")
    sv_by_run, _ = _samples_by_run(pure_sv_summary, "pure_sv")
    order_strata = {"cpp_dpi_first": [], "pure_sv_first": []}
    invalid_orders = []
    for run_id, ratio in zip(sorted(dpi_by_run), result["paired_ratios"]):
        dpi_order = dpi_by_run[run_id].get("pair_order")
        sv_order = sv_by_run[run_id].get("pair_order")
        order = dpi_order or sv_order
        if order is None:
            order = ["cpp_dpi", "pure_sv"] if run_id % 2 else ["pure_sv", "cpp_dpi"]
        if dpi_order is not None and sv_order is not None and dpi_order != sv_order:
            invalid_orders.append(run_id)
        elif order == ["cpp_dpi", "pure_sv"]:
            order_strata["cpp_dpi_first"].append(ratio)
        elif order == ["pure_sv", "cpp_dpi"]:
            order_strata["pure_sv_first"].append(ratio)
        else:
            invalid_orders.append(run_id)

    if invalid_orders or any(
        len(values) != sample_count // 2 for values in order_strata.values()
    ):
        result.update(
            {
                "status": "invalid_environment",
                "verdict": "invalid_environment",
                "validity": "invalid",
                "error": (
                    "pair ordering must be balanced DPI-first/SV-first; invalid runs: "
                    f"{invalid_orders}"
                ),
            }
        )
        return result

    try:
        dpi_median = float(cpp_dpi_summary["process_wall_ms_median"])
        sv_median = float(pure_sv_summary["process_wall_ms_median"])
    except (KeyError, TypeError, ValueError) as error:
        result.update(
            {
                "status": "invalid_environment",
                "verdict": "invalid_environment",
                "validity": "invalid",
                "error": f"invalid independent process medians: {error}",
            }
        )
        return result
    paired_median = result["ratio"]
    if (
        not math.isfinite(dpi_median)
        or dpi_median < 0
        or not math.isfinite(sv_median)
        or sv_median <= 0
        or paired_median <= 0
    ):
        result.update(
            {
                "status": "invalid_environment",
                "verdict": "invalid_environment",
                "validity": "invalid",
                "error": "independent and paired process medians must be finite and positive",
            }
        )
        return result
    independent_ratio = dpi_median / sv_median
    dpi_first_median = median(order_strata["cpp_dpi_first"])
    sv_first_median = median(order_strata["pure_sv_first"])
    relative_disagreement = abs(independent_ratio - paired_median) / paired_median
    order_stratum_gap = abs(dpi_first_median - sv_first_median) / paired_median
    result.update(
        {
            "max_ratio": max_ratio,
            "cpp_dpi_process_wall_ms": dpi_median,
            "pure_sv_process_wall_ms": sv_median,
            "dpi_first_paired_median": dpi_first_median,
            "sv_first_paired_median": sv_first_median,
            "independent_median_ratio": independent_ratio,
            "independent_paired_relative_disagreement": relative_disagreement,
            "order_stratum_gap": order_stratum_gap,
            "order_strata": order_strata,
        }
    )

    if result["ratio"] > max_ratio:
        valid_failure = (
            dpi_first_median > MIN_ORDER_STRATUM_FAILURE_RATIO
            and sv_first_median > MIN_ORDER_STRATUM_FAILURE_RATIO
            and relative_disagreement <= MAX_INDEPENDENT_PAIRED_DISAGREEMENT
        )
        if valid_failure:
            result.update(
                {
                    "status": "hard_failure",
                    "verdict": "failed",
                    "validity": "valid",
                    "error": (
                        f"paired tracked C++ DPI/pure SV process ratio exceeded "
                        f"{max_ratio:.2f}x: median ratio = {result['ratio']:.3f}x"
                    ),
                }
            )
        else:
            result.update(
                {
                    "status": "invalid_environment",
                    "verdict": "invalid_environment",
                    "validity": "invalid",
                    "error": (
                        "guard threshold was crossed, but order strata or independent "
                        "medians do not validate the measurement environment"
                    ),
                }
            )
        return result

    upper_bound = result["one_sided_95_upper_median_bound"]["bound"]
    if upper_bound > max_ratio and not final:
        result["status"] = "needs_extra_batch"
        result["verdict"] = "inconclusive"
    elif upper_bound > max_ratio:
        result["status"] = "passed_inconclusive"
        result["verdict"] = "passed"
        result["warning"] = (
            f"median tracked C++ DPI/pure SV ratio passes at {result['ratio']:.3f}x, "
            f"but its one-sided 95% upper median bound is {upper_bound:.3f}x"
        )
    else:
        result["status"] = "passed"
        result["verdict"] = "passed"
    result["validity"] = "valid"
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
    journal=None,
    requested_pairs=None,
    binary_hashes=None,
):
    dpi_sample_runner = dpi_sample_runner or run_cpp_dpi_sample
    sv_sample_runner = sv_sample_runner or run_pure_sv_sample
    dpi_samples = []
    pure_sv_samples = []
    requested_pairs = requested_pairs or pair_count
    binary_hashes = binary_hashes or {
        "cpp_dpi": binary_sha256(CPP_DPI_BINARY),
        "pure_sv": binary_sha256(PURE_SV_BINARY),
    }

    for offset in range(pair_count):
        run_id = start_run + offset
        if run_id % 2:
            dpi = dpi_sample_runner(run_id, iters, "tracked")
            dpi.update(
                {
                    "binary_sha256": binary_hashes["cpp_dpi"],
                    "sequence_index": run_id,
                    "slot": 1,
                    "pair_order": ["cpp_dpi", "pure_sv"],
                    "requested_pair_count": requested_pairs,
                    "measured_pair_count": run_id - 1,
                }
            )
            if journal:
                journal.append({"event": "sample", "label": "cpp_dpi", "sample": dpi})
            pure_sv = sv_sample_runner(run_id, iters)
            order = ["cpp_dpi", "pure_sv"]
            pure_sv.update(
                {
                    "binary_sha256": binary_hashes["pure_sv"],
                    "sequence_index": run_id,
                    "slot": 2,
                    "pair_order": order,
                    "requested_pair_count": requested_pairs,
                    "measured_pair_count": run_id,
                }
            )
            if journal:
                journal.append({"event": "sample", "label": "pure_sv", "sample": pure_sv})
        else:
            pure_sv = sv_sample_runner(run_id, iters)
            pure_sv.update(
                {
                    "binary_sha256": binary_hashes["pure_sv"],
                    "sequence_index": run_id,
                    "slot": 1,
                    "pair_order": ["pure_sv", "cpp_dpi"],
                    "requested_pair_count": requested_pairs,
                    "measured_pair_count": run_id - 1,
                }
            )
            if journal:
                journal.append({"event": "sample", "label": "pure_sv", "sample": pure_sv})
            dpi = dpi_sample_runner(run_id, iters, "tracked")
            order = ["pure_sv", "cpp_dpi"]
            dpi.update(
                {
                    "binary_sha256": binary_hashes["cpp_dpi"],
                    "sequence_index": run_id,
                    "slot": 2,
                    "pair_order": order,
                    "requested_pair_count": requested_pairs,
                    "measured_pair_count": run_id,
                }
            )
            if journal:
                journal.append({"event": "sample", "label": "cpp_dpi", "sample": dpi})
        dpi_samples.append(dpi)
        pure_sv_samples.append(pure_sv)

    return dpi_samples, pure_sv_samples


def run_critical_comparison(
    iters,
    comparison_runs,
    dpi_sample_runner=None,
    sv_sample_runner=None,
    journal=None,
):
    if comparison_runs < MIN_CRITICAL_COMPARISON_PAIRS or comparison_runs % 2:
        raise SystemExit(
            "comparison runs must be an even count of at least "
            f"{MIN_CRITICAL_COMPARISON_PAIRS}"
        )
    dpi_sample_runner = dpi_sample_runner or run_cpp_dpi_sample
    sv_sample_runner = sv_sample_runner or run_pure_sv_sample
    binary_hashes = {
        "cpp_dpi": binary_sha256(CPP_DPI_BINARY),
        "pure_sv": binary_sha256(PURE_SV_BINARY),
    }

    # Warm both binaries before entering the adjacency-sensitive pair loop.
    dpi_warmup = dpi_sample_runner(0, iters, "tracked")
    sv_warmup = sv_sample_runner(0, iters)
    dpi_warmup["run"] = 0
    sv_warmup["run"] = 0
    dpi_warmup.update(
        {
            "binary_sha256": binary_hashes["cpp_dpi"],
            "sequence_index": 0,
            "slot": 1,
            "pair_order": ["cpp_dpi", "pure_sv"],
            "requested_pair_count": comparison_runs,
            "measured_pair_count": 0,
            "warmup": True,
        }
    )
    sv_warmup.update(
        {
            "binary_sha256": binary_hashes["pure_sv"],
            "sequence_index": 0,
            "slot": 2,
            "pair_order": ["cpp_dpi", "pure_sv"],
            "requested_pair_count": comparison_runs,
            "measured_pair_count": 0,
            "warmup": True,
        }
    )
    if journal:
        journal.append({"event": "sample", "label": "cpp_dpi", "sample": dpi_warmup})
        journal.append({"event": "sample", "label": "pure_sv", "sample": sv_warmup})
    try:
        assert_same_workload({"cpp_dpi": [dpi_warmup], "pure_sv": [sv_warmup]})
    except SystemExit as error:
        return [], [], {
            "status": "invalid_environment",
            "verdict": "invalid_environment",
            "validity": "invalid",
            "error": str(error),
            "requested_pairs": comparison_runs,
            "measured_pairs": 0,
            "extra_batch_collected": False,
            "binary_sha256": binary_hashes,
            "warmup_samples": {"cpp_dpi": dpi_warmup, "pure_sv": sv_warmup},
        }

    dpi_samples, pure_sv_samples = collect_critical_pairs(
        iters,
        comparison_runs,
        dpi_sample_runner=dpi_sample_runner,
        sv_sample_runner=sv_sample_runner,
        journal=journal,
        requested_pairs=comparison_runs,
        binary_hashes=binary_hashes,
    )
    try:
        assert_same_workload({"cpp_dpi": dpi_samples, "pure_sv": pure_sv_samples})
    except SystemExit as error:
        return dpi_samples, pure_sv_samples, {
            "status": "invalid_environment",
            "verdict": "invalid_environment",
            "validity": "invalid",
            "error": str(error),
            "requested_pairs": comparison_runs,
            "measured_pairs": len(dpi_samples),
            "extra_batch_collected": False,
            "binary_sha256": binary_hashes,
        }
    guard = check_dpi_vs_pure_sv(
        summarize("cpp_dpi", dpi_samples),
        summarize("pure_sv", pure_sv_samples),
        final=False,
    )
    extra_batch_collected = guard["status"] == "needs_extra_batch"
    if extra_batch_collected:
        extra_dpi, extra_sv = collect_critical_pairs(
            iters,
            EXTRA_COMPARISON_PAIRS,
            start_run=comparison_runs + 1,
            dpi_sample_runner=dpi_sample_runner,
            sv_sample_runner=sv_sample_runner,
            journal=journal,
            requested_pairs=comparison_runs + EXTRA_COMPARISON_PAIRS,
            binary_hashes=binary_hashes,
        )
        dpi_samples.extend(extra_dpi)
        pure_sv_samples.extend(extra_sv)
        try:
            assert_same_workload(
                {"cpp_dpi": dpi_samples, "pure_sv": pure_sv_samples}
            )
        except SystemExit as error:
            return dpi_samples, pure_sv_samples, {
                "status": "invalid_environment",
                "verdict": "invalid_environment",
                "validity": "invalid",
                "error": str(error),
                "requested_pairs": comparison_runs,
                "measured_pairs": len(dpi_samples),
                "extra_batch_collected": True,
                "binary_sha256": binary_hashes,
            }
        guard = check_dpi_vs_pure_sv(
            summarize("cpp_dpi", dpi_samples),
            summarize("pure_sv", pure_sv_samples),
            final=True,
        )

    guard["requested_pairs"] = comparison_runs
    guard["measured_pairs"] = len(dpi_samples)
    guard["extra_batch_collected"] = extra_batch_collected
    guard["binary_sha256"] = binary_hashes
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
        default=MIN_CRITICAL_COMPARISON_PAIRS,
        help="number of adjacent tracked DPI/SV pairs in the initial guard batch",
    )
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args(argv)
    if args.iters <= 0:
        parser.error("--iters must be greater than zero")
    if args.runs <= 0:
        parser.error("--runs must be greater than zero")
    if (
        args.comparison_runs < MIN_CRITICAL_COMPARISON_PAIRS
        or args.comparison_runs % 2
    ):
        parser.error(
            "--comparison-runs must be an even count of at least "
            f"{MIN_CRITICAL_COMPARISON_PAIRS}"
        )
    return args


def _journal_samples(journal):
    samples = []
    if journal is None or not journal.path.exists():
        return samples
    for line in journal.path.read_text(encoding="utf-8").splitlines():
        try:
            entry = json.loads(line)
        except json.JSONDecodeError:
            continue
        if entry.get("event") == "sample":
            samples.append(entry)
    return samples


def _failure_markdown(result):
    guard = result.get("performance_guard", {})
    lines = [
        "# Peripheral-suite benchmark diagnostic",
        "",
        f"- Status: `{result['status']}`",
        f"- Verdict: `{result.get('verdict', result['status'])}`",
        f"- Requested critical pairs: `{result.get('requested_pairs', 0)}`",
        f"- Measured critical pairs: `{result.get('measured_pairs', 0)}`",
    ]
    if "ratio" in guard:
        lines.append(f"- Paired DPI/SV median: `{guard['ratio']:.3f}x`")
    if result.get("error"):
        lines.extend([f"- Error: {result['error']}", ""])
    lines.append("Raw evidence is retained in the JSON result and JSONL journal.")
    return "\n".join(lines) + "\n"


def persist_diagnostic_result(result, json_path, md_path, markdown=None):
    atomic_write_json(json_path, result)
    atomic_write_text(md_path, markdown or _failure_markdown(result))


def main(argv=None):
    args = _parse_args(argv)

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    json_path = RESULT_DIR / "latest.json"
    md_path = RESULT_DIR / "latest.md"
    journal = SampleJournal(RESULT_DIR / "latest.jsonl", truncate=True)

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
    try:
        if not args.skip_build:
            for command in build_commands:
                run_command(command)

        cpp_dpi_samples, pure_sv_samples, performance_guard = run_critical_comparison(
            args.iters, args.comparison_runs, journal=journal
        )
    except (CommandExecutionError, ResultParseError) as error:
        result = {
            "status": "command_error",
            "verdict": "invalid_environment",
            "error": str(error),
            "command_failure": {
                "command": getattr(error, "command", None),
                "returncode": getattr(error, "returncode", None),
                "output": error.output,
            },
            "requested_pairs": args.comparison_runs,
            "measured_pairs": len(
                {
                    entry["sample"].get("sequence_index")
                    for entry in _journal_samples(journal)
                    if not entry["sample"].get("warmup")
                }
            ),
            "samples": _journal_samples(journal),
            "metadata": collect_metadata(
                {"iterations": args.iters, "skip_build": args.skip_build}
            ),
        }
        persist_diagnostic_result(result, json_path, md_path)
        return 1

    if performance_guard["status"] in {"hard_failure", "invalid_environment"}:
        result = {
            "status": performance_guard["status"],
            "verdict": performance_guard["verdict"],
            "error": performance_guard.get("error"),
            "iterations": args.iters,
            "requested_pairs": args.comparison_runs,
            "measured_pairs": len(cpp_dpi_samples),
            "cpp_dpi": summarize("cpp_dpi", cpp_dpi_samples),
            "pure_sv": summarize("pure_sv", pure_sv_samples),
            "performance_guard": performance_guard,
            "samples": _journal_samples(journal),
            "metadata": collect_metadata(
                {
                    "iterations": args.iters,
                    "comparison_runs_requested": args.comparison_runs,
                    "comparison_runs_measured": len(cpp_dpi_samples),
                    "skip_build": args.skip_build,
                }
            ),
        }
        persist_diagnostic_result(result, json_path, md_path)
        return 1
    if "warning" in performance_guard:
        print(f"WARNING: {performance_guard['warning']}")

    # Slow integrations are intentionally outside the adjacency-sensitive guard loop.
    try:
        cpp_vpi_samples = []
        cpp_vpi_hash = binary_sha256(CPP_VPI_BINARY)
        for run_id in range(1, args.runs + 1):
            sample = run_cpp_vpi_sample(run_id, args.iters)
            sample.update(
                {
                    "binary_sha256": cpp_vpi_hash,
                    "sequence_index": run_id,
                    "slot": 1,
                    "pair_order": ["cpp_vpi"],
                    "requested_pair_count": args.runs,
                    "measured_pair_count": run_id,
                }
            )
            cpp_vpi_samples.append(sample)
            journal.append({"event": "sample", "label": "cpp_vpi", "sample": sample})

        cocotb_samples = []
        cocotb_hash = binary_sha256(COCOTB_RUNNER)
        for run_id in range(1, args.runs + 1):
            sample = run_cocotb_sample(run_id, args.iters)
            sample.update(
                {
                    "binary_sha256": cocotb_hash,
                    "sequence_index": run_id,
                    "slot": 1,
                    "pair_order": ["cocotb"],
                    "requested_pair_count": args.runs,
                    "measured_pair_count": run_id,
                }
            )
            cocotb_samples.append(sample)
            journal.append({"event": "sample", "label": "cocotb", "sample": sample})
    except (CommandExecutionError, ResultParseError) as error:
        result = {
            "status": "command_error",
            "verdict": "invalid_environment",
            "error": str(error),
            "command_failure": {
                "command": getattr(error, "command", None),
                "returncode": getattr(error, "returncode", None),
                "output": error.output,
            },
            "requested_pairs": args.comparison_runs,
            "measured_pairs": len(cpp_dpi_samples),
            "cpp_dpi": summarize("cpp_dpi", cpp_dpi_samples),
            "pure_sv": summarize("pure_sv", pure_sv_samples),
            "performance_guard": performance_guard,
            "samples": _journal_samples(journal),
            "metadata": collect_metadata(
                {"iterations": args.iters, "skip_build": args.skip_build}
            ),
        }
        persist_diagnostic_result(result, json_path, md_path)
        return 1
    try:
        assert_same_workload({"cpp_vpi": cpp_vpi_samples, "cocotb": cocotb_samples})
        assert_same_workload(
            {
                "cpp_vpi": [cpp_vpi_samples[0]],
                "cpp_dpi": [cpp_dpi_samples[0]],
                "pure_sv": [pure_sv_samples[0]],
                "cocotb": [cocotb_samples[0]],
            }
        )
    except SystemExit as error:
        result = {
            "status": "workload_error",
            "verdict": "invalid_environment",
            "error": str(error),
            "requested_pairs": args.comparison_runs,
            "measured_pairs": len(cpp_dpi_samples),
            "cpp_vpi": summarize("cpp_vpi", cpp_vpi_samples),
            "cpp_dpi": summarize("cpp_dpi", cpp_dpi_samples),
            "pure_sv": summarize("pure_sv", pure_sv_samples),
            "cocotb": summarize("cocotb", cocotb_samples),
            "performance_guard": performance_guard,
            "samples": _journal_samples(journal),
            "metadata": collect_metadata(
                {"iterations": args.iters, "skip_build": args.skip_build}
            ),
        }
        persist_diagnostic_result(result, json_path, md_path)
        return 1

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
        "binary_sha256": {
            "cpp_vpi": cpp_vpi_hash,
            "cpp_dpi": performance_guard["binary_sha256"]["cpp_dpi"],
            "pure_sv": performance_guard["binary_sha256"]["pure_sv"],
            "cocotb_runner": cocotb_hash,
        },
    }
    result = {
        "status": "passed",
        "verdict": "passed",
        "iterations": args.iters,
        "runs": args.runs,
        "comparison_runs": len(cpp_dpi_samples),
        "requested_pairs": args.comparison_runs,
        "measured_pairs": len(cpp_dpi_samples),
        "design": "peripheral_suite: APB timer + APB SPI + APB I2C",
        "cpp_dpi_spawn_mode": "tracked",
        "binary_sha256": config["binary_sha256"],
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

    upper_bound = performance_guard["one_sided_95_upper_median_bound"]["bound"]
    interval = performance_guard["two_sided_95_median_ci"]
    markdown = "\n".join(
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
                f"- DPI-first paired median: `{performance_guard['dpi_first_paired_median']:.3f}x`",
                f"- SV-first paired median: `{performance_guard['sv_first_paired_median']:.3f}x`",
                f"- Independent-median ratio: `{performance_guard['independent_median_ratio']:.3f}x`",
                "- Independent/paired relative disagreement: "
                f"`{performance_guard['independent_paired_relative_disagreement']:.2%}`",
                f"- Order-stratum gap: `{performance_guard['order_stratum_gap']:.2%}`",
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
    persist_diagnostic_result(result, json_path, md_path, markdown=markdown)

    print(md_path.read_text())
    print(f"Wrote {json_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
