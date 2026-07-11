#!/usr/bin/env python3
"""Run the paired authoring-core C++ DPI versus pure-SV benchmark."""

from __future__ import annotations

import argparse
import datetime
import json
import math
import os
import platform
import re
import shutil
import socket
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable, Iterable


BENCH_DIR = Path(__file__).resolve().parent
REPO = BENCH_DIR.parents[1]
RESULT_DIR = BENCH_DIR / "results"
sys.path.insert(0, str(BENCH_DIR))
from workload import (  # noqa: E402
    FEATURE_FIELDS,
    KERNELS,
    RESULT_FIELDS,
    expected_checksum,
    expected_counts,
)


DEFAULT_ITERATIONS = 100_000
MIN_ITERATIONS = 10_000
DEFAULT_PAIRS = 15
MIN_PAIRS = 15
MAX_DPI_OVER_SV_RATIO = 1.10
CONFIDENCE = 0.95
RESULT_RE = re.compile(r"^AUTHORING_CORE_RESULT\s+(?P<fields>.+)$")
PERIPHERAL_RESULT_RE = re.compile(
    r"^(?P<name>CPP_DPI_PERIPHERAL_RESULT|PURE_SV_PERIPHERAL_RESULT)\s+(?P<fields>.+)$"
)


class GuardFailure(RuntimeError):
    pass


def _parse_fields(text: str) -> dict[str, object]:
    fields: dict[str, object] = {}
    for item in text.split():
        if "=" not in item:
            raise ValueError(f"invalid result token: {item!r}")
        key, value = item.split("=", 1)
        if not key or key in fields:
            raise ValueError(f"duplicate or empty result field: {key!r}")
        if key in {"mode", "kernel"}:
            fields[key] = value
            continue
        try:
            fields[key] = float(value) if key.endswith("_ms") else int(value, 10)
        except ValueError as error:
            raise ValueError(f"invalid numeric field {key}={value!r}") from error
    return fields


def parse_result(
    output: str,
    expected_mode: str | None = None,
    expected_kernel: str | None = None,
    expected_iterations: int | None = None,
) -> dict[str, object]:
    matches = [RESULT_RE.match(line.strip()) for line in output.splitlines()]
    matches = [match for match in matches if match]
    if len(matches) != 1:
        raise ValueError(f"expected exactly one AUTHORING_CORE_RESULT, found {len(matches)}")
    result = _parse_fields(matches[0].group("fields"))
    missing = [field for field in RESULT_FIELDS if field not in result]
    if missing:
        raise ValueError(f"result missing fields: {missing}")
    if result["mode"] not in {"cpp_dpi", "pure_sv"}:
        raise ValueError(f"invalid mode: {result['mode']!r}")
    if result["kernel"] not in KERNELS:
        raise ValueError(f"invalid kernel: {result['kernel']!r}")
    if expected_mode is not None and result["mode"] != expected_mode:
        raise ValueError(f"mode mismatch: {result['mode']} != {expected_mode}")
    if expected_kernel is not None and result["kernel"] != expected_kernel:
        raise ValueError(f"kernel mismatch: {result['kernel']} != {expected_kernel}")
    if expected_iterations is not None and result["iterations"] != expected_iterations:
        raise ValueError(
            f"iteration mismatch: {result['iterations']} != {expected_iterations}"
        )
    for key, value in result.items():
        if key.endswith("_ms") and (
            not math.isfinite(float(value)) or float(value) < 0
        ):
            raise ValueError(f"{key} must be finite and non-negative")
    for field in RESULT_FIELDS:
        if field in {"mode", "kernel"}:
            continue
        if not isinstance(result[field], int) or int(result[field]) < 0:
            raise ValueError(f"{field} must be a non-negative integer")
    return result


def validate_contract(result: dict[str, object]) -> None:
    kernel = str(result["kernel"])
    iterations = int(result["iterations"])
    expected = expected_counts(kernel, iterations).fields()
    for field, value in expected.items():
        if int(result[field]) != value:
            raise ValueError(
                f"{kernel} contract mismatch for {field}: {result[field]} != {value}"
            )
    checksum = expected_checksum(iterations)
    if int(result["checksum"]) != checksum:
        raise ValueError(
            f"{kernel} checksum mismatch: {result['checksum']} != {checksum}"
        )
    if int(result["failures"]) != 0:
        raise ValueError(f"{kernel} reported {result['failures']} failures")


EQUIVALENCE_FIELDS = (
    "kernel",
    "iterations",
    "transactions",
    "checks",
    "sim_cycles",
    "checksum",
    "failures",
    *FEATURE_FIELDS,
)


def assert_equivalent(cpp_result: dict[str, object], sv_result: dict[str, object]) -> None:
    mismatches = {
        field: (cpp_result[field], sv_result[field])
        for field in EQUIVALENCE_FIELDS
        if cpp_result[field] != sv_result[field]
    }
    if mismatches:
        raise ValueError(f"C++ DPI/pure SV workload mismatch: {mismatches}")


def _finite_values(values: Iterable[float], label: str) -> list[float]:
    converted = [float(value) for value in values]
    if not converted:
        raise ValueError(f"{label} requires at least one sample")
    if any(not math.isfinite(value) for value in converted):
        raise ValueError(f"{label} samples must be finite")
    return converted


def _binomial_cdf_half(n: int, successes: int) -> float:
    if successes < 0:
        return 0.0
    return sum(math.comb(n, value) for value in range(min(successes, n) + 1)) / (2**n)


def one_sided_upper_median_bound(
    values: Iterable[float], confidence: float = CONFIDENCE
) -> dict[str, object]:
    ordered = sorted(_finite_values(values, "median bound"))
    if not 0.0 < confidence < 1.0:
        raise ValueError("confidence must be between zero and one")
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


def two_sided_median_confidence_interval(
    values: Iterable[float], confidence: float = CONFIDENCE
) -> dict[str, object]:
    ordered = sorted(_finite_values(values, "median interval"))
    if not 0.0 < confidence < 1.0:
        raise ValueError("confidence must be between zero and one")
    tail = (1.0 - confidence) / 2.0
    lower_order = 0
    for candidate in range(1, (len(ordered) + 1) // 2 + 1):
        if _binomial_cdf_half(len(ordered), candidate - 1) <= tail:
            lower_order = candidate
        else:
            break
    if lower_order == 0:
        lower, upper, upper_order, coverage = -math.inf, math.inf, None, 1.0
    else:
        upper_order = len(ordered) - lower_order + 1
        lower = ordered[lower_order - 1]
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


def validate_pair_order(cpp_samples: list[dict], sv_samples: list[dict]) -> None:
    if len(cpp_samples) != len(sv_samples) or not cpp_samples:
        raise ValueError("paired sample lists must be non-empty and equal length")
    cpp_by_pair = {int(sample["pair"]): sample for sample in cpp_samples}
    sv_by_pair = {int(sample["pair"]): sample for sample in sv_samples}
    if len(cpp_by_pair) != len(cpp_samples) or len(sv_by_pair) != len(sv_samples):
        raise ValueError("paired samples contain duplicate pair IDs")
    if cpp_by_pair.keys() != sv_by_pair.keys():
        raise ValueError("C++ DPI and pure-SV pair IDs differ")
    for pair in sorted(cpp_by_pair):
        expected = ["cpp_dpi", "pure_sv"] if pair % 2 else ["pure_sv", "cpp_dpi"]
        if cpp_by_pair[pair]["pair_order"] != expected:
            raise ValueError(f"invalid C++ pair order for pair {pair}")
        if sv_by_pair[pair]["pair_order"] != expected:
            raise ValueError(f"invalid SV pair order for pair {pair}")
        if abs(int(cpp_by_pair[pair]["sequence_index"]) - int(sv_by_pair[pair]["sequence_index"])) != 1:
            raise ValueError(f"pair {pair} was not measured adjacently")
        assert_equivalent(cpp_by_pair[pair], sv_by_pair[pair])


def paired_ratio_statistics(
    cpp_samples: list[dict], sv_samples: list[dict]
) -> dict[str, object]:
    validate_pair_order(cpp_samples, sv_samples)
    cpp_by_pair = {int(sample["pair"]): sample for sample in cpp_samples}
    sv_by_pair = {int(sample["pair"]): sample for sample in sv_samples}
    ratios = []
    for pair in sorted(cpp_by_pair):
        cpp_ms = float(cpp_by_pair[pair]["process_wall_ms"])
        sv_ms = float(sv_by_pair[pair]["process_wall_ms"])
        if not math.isfinite(cpp_ms) or cpp_ms <= 0:
            raise ValueError("C++ DPI process samples must be finite and positive")
        if not math.isfinite(sv_ms) or sv_ms <= 0:
            raise ValueError("pure-SV process samples must be finite and positive")
        ratios.append(cpp_ms / sv_ms)
    ratio = statistics.median(ratios)
    return {
        "metric": "median(cpp_dpi_process_wall_ms / pure_sv_process_wall_ms by adjacent pair)",
        "ratio": ratio,
        "overhead_percent": (ratio - 1.0) * 100.0,
        "paired_ratios": ratios,
        "one_sided_95_upper_median_bound": one_sided_upper_median_bound(ratios),
        "two_sided_95_median_ci": two_sided_median_confidence_interval(ratios),
    }


def evaluate_guard(
    cpp_samples: list[dict],
    sv_samples: list[dict],
    max_ratio: float = MAX_DPI_OVER_SV_RATIO,
    final: bool = False,
) -> dict[str, object]:
    if len(cpp_samples) < MIN_PAIRS:
        raise ValueError(f"guard requires at least {MIN_PAIRS} paired samples")
    stats = paired_ratio_statistics(cpp_samples, sv_samples)
    stats["max_ratio"] = max_ratio
    if float(stats["ratio"]) > max_ratio:
        raise GuardFailure(
            f"strict authoring-core guard failed: paired median "
            f"{stats['ratio']:.3f}x > {max_ratio:.2f}x"
        )
    upper = float(stats["one_sided_95_upper_median_bound"]["bound"])
    if upper > max_ratio and not final:
        stats["status"] = "needs_extra_batch"
    elif upper > max_ratio:
        stats["status"] = "passed_inconclusive"
        stats["warning"] = (
            f"median passes at {stats['ratio']:.3f}x, but the one-sided 95% "
            f"upper median bound remains {upper:.3f}x"
        )
    else:
        stats["status"] = "passed"
    return stats


def run_command(command: list[str]) -> tuple[str, float]:
    started = time.perf_counter()
    completed = subprocess.run(
        command,
        cwd=REPO,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    wall_ms = (time.perf_counter() - started) * 1000.0
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed with exit {completed.returncode}: {' '.join(command)}\n"
            f"{completed.stdout}"
        )
    return completed.stdout, wall_ms


def _binary(mode: str, kernel: str) -> Path:
    if mode == "cpp_dpi":
        return REPO / "build" / "benchmarks" / "authoring_core" / f"cpp_dpi_{kernel}" / "Vdpi_authoring_core"
    if mode == "pure_sv":
        return REPO / "build" / "benchmarks" / "authoring_core" / "pure_sv_obj" / "Vauthoring_core_sv_tb"
    raise ValueError(f"unknown mode: {mode}")


def run_sample(mode: str, kernel: str, pair: int, iterations: int) -> dict:
    command = [str(_binary(mode, kernel)), f"+AUTHORING_CORE_ITERS={iterations}"]
    if mode == "pure_sv":
        command.append(f"+AUTHORING_CORE_KERNEL={kernel}")
    output, process_ms = run_command(command)
    result = parse_result(output, mode, kernel, iterations)
    validate_contract(result)
    return {**result, "pair": pair, "process_wall_ms": process_ms}


def collect_batch(
    kernels: list[str],
    iterations: int,
    pair_count: int,
    start_pair: int,
    sample_runner: Callable[[str, str, int, int], dict] = run_sample,
    raw_samples: list[dict] | None = None,
    hard_guard_on_completion: bool = False,
) -> dict[str, dict[str, list[dict]]]:
    raw_samples = raw_samples if raw_samples is not None else []
    collected = {kernel: {"cpp_dpi": [], "pure_sv": []} for kernel in kernels}
    for offset in range(pair_count):
        pair = start_pair + offset
        rotation = offset % len(kernels)
        round_kernels = kernels[rotation:] + kernels[:rotation]
        for kernel in round_kernels:
            order = ["cpp_dpi", "pure_sv"] if pair % 2 else ["pure_sv", "cpp_dpi"]
            pair_samples = {}
            for mode in order:
                sample = sample_runner(mode, kernel, pair, iterations)
                sample["pair_order"] = list(order)
                sample["sequence_index"] = len(raw_samples)
                sample["batch"] = "initial" if start_pair == 1 else "extra"
                raw_samples.append(sample)
                collected[kernel][mode].append(sample)
                pair_samples[mode] = sample
            assert_equivalent(pair_samples["cpp_dpi"], pair_samples["pure_sv"])
            if hard_guard_on_completion and len(collected[kernel]["cpp_dpi"]) == pair_count:
                try:
                    evaluate_guard(
                        collected[kernel]["cpp_dpi"],
                        collected[kernel]["pure_sv"],
                        final=False,
                    )
                except GuardFailure as error:
                    raise GuardFailure(f"kernel {kernel}: {error}") from error
    return collected


def run_comparison(
    kernels: list[str],
    iterations: int,
    pairs: int,
    sample_runner: Callable[[str, str, int, int], dict] = run_sample,
) -> tuple[dict[str, dict], list[dict]]:
    if pairs < MIN_PAIRS:
        raise ValueError(f"comparison requires at least {MIN_PAIRS} pairs")
    raw_samples: list[dict] = []
    for kernel in kernels:
        warm_cpp = sample_runner("cpp_dpi", kernel, 0, iterations)
        warm_sv = sample_runner("pure_sv", kernel, 0, iterations)
        assert_equivalent(warm_cpp, warm_sv)

    initial = collect_batch(
        kernels, iterations, pairs, 1, sample_runner, raw_samples,
        hard_guard_on_completion=True,
    )
    summaries: dict[str, dict] = {}
    uncertain = []
    for kernel in kernels:
        guard = evaluate_guard(
            initial[kernel]["cpp_dpi"], initial[kernel]["pure_sv"], final=False
        )
        summaries[kernel] = {**initial[kernel], "guard": guard}
        if guard["status"] == "needs_extra_batch":
            uncertain.append(kernel)

    if uncertain:
        extra = collect_batch(
            uncertain, iterations, pairs, pairs + 1, sample_runner, raw_samples
        )
        for kernel in uncertain:
            summaries[kernel]["cpp_dpi"].extend(extra[kernel]["cpp_dpi"])
            summaries[kernel]["pure_sv"].extend(extra[kernel]["pure_sv"])
            summaries[kernel]["guard"] = evaluate_guard(
                summaries[kernel]["cpp_dpi"],
                summaries[kernel]["pure_sv"],
                final=True,
            )
            summaries[kernel]["guard"]["extra_batch_collected"] = True
    for kernel in kernels:
        summaries[kernel]["guard"].setdefault("extra_batch_collected", False)
    return summaries, raw_samples


def _parse_peripheral(output: str, expected_name: str) -> dict[str, int]:
    for line in output.splitlines():
        match = PERIPHERAL_RESULT_RE.match(line.strip())
        if match and match.group("name") == expected_name:
            fields = _parse_fields(match.group("fields"))
            return {key: int(fields[key]) for key in ("iterations", "checks", "sim_cycles", "failures")}
    raise ValueError(f"missing {expected_name}")


def run_peripheral_preflight(iterations: int) -> dict[str, object]:
    dpi_binary = REPO / "build/benchmarks/peripheral_suite/cpp_dpi_obj/Vdpi_peripheral_suite"
    sv_binary = REPO / "build/benchmarks/peripheral_suite/pure_sv_obj/Vperipheral_suite_sv_tb"
    dpi_output, dpi_ms = run_command([str(dpi_binary), f"+PERIPHERAL_SUITE_ITERS={iterations}"])
    sv_output, sv_ms = run_command([str(sv_binary), f"+PERIPHERAL_SUITE_ITERS={iterations}"])
    dpi = _parse_peripheral(dpi_output, "CPP_DPI_PERIPHERAL_RESULT")
    sv = _parse_peripheral(sv_output, "PURE_SV_PERIPHERAL_RESULT")
    for field in ("iterations", "checks", "sim_cycles", "failures"):
        if dpi[field] != sv[field]:
            raise ValueError(f"peripheral preflight mismatch for {field}: {dpi[field]} != {sv[field]}")
    if dpi["failures"] != 0:
        raise ValueError("peripheral preflight reported failures")
    return {
        "status": "passed",
        "iterations": iterations,
        "cpp_dpi": {**dpi, "process_wall_ms": dpi_ms},
        "pure_sv": {**sv, "process_wall_ms": sv_ms},
    }


def _command_metadata(command: list[str]) -> dict[str, object]:
    try:
        completed = subprocess.run(
            command, cwd=REPO, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, check=False
        )
        return {"command": command, "returncode": completed.returncode, "output": completed.stdout.strip()}
    except OSError as error:
        return {"command": command, "returncode": None, "output": str(error)}


def collect_metadata(config: dict[str, object]) -> dict[str, object]:
    git_revision = _command_metadata(["git", "rev-parse", "HEAD"])
    git_status = _command_metadata(["git", "status", "--porcelain"])
    compiler = _command_metadata([os.environ.get("CXX", "c++"), "--version"])
    verilator = _command_metadata(["verilator", "--version"])
    return {
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "git": {"commit": git_revision["output"] if git_revision["returncode"] == 0 else None,
                "dirty": git_status["returncode"] != 0 or bool(git_status["output"])},
        "host": {"hostname": socket.gethostname(), "platform": platform.platform(),
                 "machine": platform.machine()},
        "python": {"version": platform.python_version(), "executable": sys.executable},
        "compiler": {**compiler, "executable": shutil.which(os.environ.get("CXX", "c++"))},
        "verilator": {**verilator, "executable": shutil.which("verilator")},
        "command": [sys.executable, *sys.argv],
        "config": config,
    }


def _write_results(result: dict[str, object]) -> None:
    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    (RESULT_DIR / "latest.json").write_text(json.dumps(result, indent=2) + "\n")
    lines = [
        "# Authoring-core C++ DPI vs pure SystemVerilog benchmark",
        "",
        f"- Iterations per sample: `{result['iterations']}`",
        f"- Initial adjacent warmed pairs: `{result['pairs']}`",
        f"- Absolute hard guard: `C++ DPI / pure SV <= {MAX_DPI_OVER_SV_RATIO:.2f}x`",
        f"- Peripheral preflight: `{result['preflight']['status']}`",
        "",
        "| Kernel | Paired median | One-sided 95% upper | Status | Extra batch | Normalized/control |",
        "|---|---:|---:|---|---:|---:|",
    ]
    control_ratio = float(result["kernels"]["control"]["guard"]["ratio"])
    for kernel in result["selected_kernels"]:
        guard = result["kernels"][kernel]["guard"]
        upper = float(guard["one_sided_95_upper_median_bound"]["bound"])
        normalized = float(guard["ratio"]) / control_ratio
        lines.append(
            f"| `{kernel}` | {guard['ratio']:.3f}x | {upper:.3f}x | "
            f"`{guard['status']}` | `{guard['extra_batch_collected']}` | {normalized:.3f}x |"
        )
    lines.extend([
        "",
        "The absolute paired ratio is the guard. Normalization against `control` is diagnostic only.",
        "Raw execution order and every sample are preserved in `latest.json`.",
        "",
    ])
    (RESULT_DIR / "latest.md").write_text("\n".join(lines))


def _parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--iters", type=int, default=DEFAULT_ITERATIONS)
    parser.add_argument("--pairs", type=int, default=DEFAULT_PAIRS)
    parser.add_argument("--kernels", default=",".join(KERNELS))
    parser.add_argument("--preflight-iters", type=int, default=1000)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-preflight", action="store_true")
    args = parser.parse_args(argv)
    if args.iters < MIN_ITERATIONS:
        parser.error(f"--iters must be at least {MIN_ITERATIONS}")
    if args.pairs < MIN_PAIRS:
        parser.error(f"--pairs must be at least {MIN_PAIRS}")
    if args.preflight_iters <= 0:
        parser.error("--preflight-iters must be greater than zero")
    args.kernels = args.kernels.split(",")
    if not args.kernels or len(args.kernels) != len(set(args.kernels)):
        parser.error("--kernels must be a non-empty unique comma-separated list")
    unknown = [kernel for kernel in args.kernels if kernel not in KERNELS]
    if unknown:
        parser.error(f"unknown kernels: {unknown}")
    if "control" not in args.kernels:
        parser.error("--kernels must include control for normalization")
    return args


def main(argv: list[str] | None = None) -> int:
    args = _parse_args(argv)
    if not args.skip_build:
        run_command(["make", "authoring-core-build"])
        if not args.skip_preflight:
            run_command(["make", "peripheral-suite-dpi-build", "peripheral-suite-sv-build"])
    preflight = (
        {"status": "skipped"}
        if args.skip_preflight
        else run_peripheral_preflight(args.preflight_iters)
    )
    summaries, raw_samples = run_comparison(
        args.kernels, args.iters, args.pairs
    )
    control_ratio = float(summaries["control"]["guard"]["ratio"])
    for kernel in args.kernels:
        summaries[kernel]["normalized_vs_control"] = (
            float(summaries[kernel]["guard"]["ratio"]) / control_ratio
        )
        warning = summaries[kernel]["guard"].get("warning")
        if warning:
            print(f"WARNING [{kernel}]: {warning}")
    config = {
        "iterations": args.iters,
        "initial_pairs": args.pairs,
        "kernels": args.kernels,
        "skip_build": args.skip_build,
        "skip_preflight": args.skip_preflight,
        "preflight_iterations": args.preflight_iters,
        "max_absolute_ratio": MAX_DPI_OVER_SV_RATIO,
    }
    result = {
        "benchmark": "authoring_core_cpp_dpi_vs_pure_sv",
        "iterations": args.iters,
        "pairs": args.pairs,
        "selected_kernels": args.kernels,
        "workload": "shared deterministic authoring_core_dut request/response contract",
        "metadata": collect_metadata(config),
        "preflight": preflight,
        "raw_samples": raw_samples,
        "kernels": summaries,
    }
    _write_results(result)
    print((RESULT_DIR / "latest.md").read_text())
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (GuardFailure, RuntimeError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
