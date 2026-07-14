#!/usr/bin/env python3
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from benchmarks.framework_comparison.workload import (  # noqa: E402
    AUTHORING_WORKLOADS,
    DESCRIPTIONS,
    MODES,
    expected_authoring_result,
    rotated_mode_order,
)


BENCH_DIR = REPO / "benchmarks" / "framework_comparison"
RESULT_DIR = BENCH_DIR / "results"
AUTHORING_BUILD = REPO / "build" / "benchmarks" / "authoring_core"
PURE_SV_BINARY = AUTHORING_BUILD / "pure_sv_obj" / "Vauthoring_core_sv_tb"
VPI_BINARY = (
    REPO
    / "build"
    / "benchmarks"
    / "framework_comparison"
    / "cpp_vpi_obj"
    / "Vauthoring_core_vpi_top"
)
COCOTB_RUNNER = BENCH_DIR / "testbenches" / "cocotb" / "run_cocotb.py"
COCOTB_PYTHON = os.environ.get("COCOTB_BENCH_PYTHON", "/opt/homebrew/bin/python3.12")
PERIPHERAL_RUNNER = REPO / "benchmarks" / "peripheral_suite" / "run_benchmark.py"
PERIPHERAL_RESULT = REPO / "benchmarks" / "peripheral_suite" / "results" / "latest.json"
MAX_CPP_DPI_OVER_PURE_SV = 1.10

RESULT_RE = re.compile(
    r"(?:AUTHORING_CORE_RESULT|FRAMEWORK_COMPARISON_RESULT)\s+(?P<fields>.*)"
)


class BenchmarkError(RuntimeError):
    pass


def _number(value: str):
    try:
        return int(value, 0)
    except ValueError:
        try:
            return float(value)
        except ValueError:
            return value


def parse_result(output: str) -> dict:
    for line in output.splitlines():
        match = RESULT_RE.search(line)
        if not match:
            continue
        fields = {}
        for token in match.group("fields").split():
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            fields[key] = _number(value)
        if "kernel" in fields and "workload" not in fields:
            fields["workload"] = fields.pop("kernel")
        return fields
    raise BenchmarkError("benchmark output did not contain a recognized result line")


def run_command(command, *, env=None) -> tuple[str, float]:
    start = time.perf_counter()
    completed = subprocess.run(
        [str(part) for part in command],
        cwd=REPO,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    process_wall_ms = (time.perf_counter() - start) * 1000.0
    if completed.returncode != 0:
        raise BenchmarkError(
            f"command failed with exit {completed.returncode}: "
            f"{' '.join(map(str, command))}\n{completed.stdout}"
        )
    return completed.stdout, process_wall_ms


def command_for(mode: str, workload: str, iterations: int) -> tuple[list[str], dict | None]:
    if mode == "pure_sv":
        return (
            [
                str(PURE_SV_BINARY),
                f"+AUTHORING_CORE_ITERS={iterations}",
                f"+AUTHORING_CORE_KERNEL={workload}",
            ],
            None,
        )
    if mode == "cpp_dpi":
        binary = AUTHORING_BUILD / f"cpp_dpi_{workload}" / "Vdpi_authoring_core"
        return ([str(binary), f"+AUTHORING_CORE_ITERS={iterations}"], None)
    if mode == "cpp_vpi":
        environment = os.environ.copy()
        environment.update(
            {
                "FRAMEWORK_COMPARISON_WORKLOAD": workload,
                "FRAMEWORK_COMPARISON_ITERS": str(iterations),
            }
        )
        return ([str(VPI_BINARY)], environment)
    if mode == "cocotb":
        environment = os.environ.copy()
        environment["UV_CACHE_DIR"] = str(REPO / "build" / "uv-cache")
        return (
            [
                "uv",
                "run",
                "--offline",
                "--no-project",
                "--python",
                COCOTB_PYTHON,
                "--with",
                "cocotb==2.0.1",
                "python",
                str(COCOTB_RUNNER),
                "--workload",
                workload,
                "--iters",
                str(iterations),
                "--no-build",
            ],
            environment,
        )
    raise ValueError(f"unknown comparison mode: {mode}")


def validate_authoring_result(
    result: dict, mode: str, workload: str, iterations: int, sim_cycles: int | None
):
    if result.get("mode") != mode:
        raise BenchmarkError(
            f"{workload}/{mode}: reported mode {result.get('mode')!r}"
        )
    if result.get("workload") != workload:
        raise BenchmarkError(
            f"{workload}/{mode}: reported workload {result.get('workload')!r}"
        )
    expected = expected_authoring_result(workload, iterations)
    mismatches = {
        key: (result.get(key), value)
        for key, value in expected.items()
        if result.get(key) != value
    }
    if sim_cycles is not None and result.get("sim_cycles") != sim_cycles:
        mismatches["sim_cycles"] = (result.get("sim_cycles"), sim_cycles)
    if mismatches:
        details = ", ".join(
            f"{key}: actual={actual!r} expected={expected!r}"
            for key, (actual, expected) in mismatches.items()
        )
        raise BenchmarkError(f"{workload}/{mode}: semantic mismatch: {details}")


def run_sample(
    mode: str,
    workload: str,
    iterations: int,
    *,
    sim_cycles: int | None,
) -> dict:
    command, environment = command_for(mode, workload, iterations)
    output, process_wall_ms = run_command(command, env=environment)
    result = parse_result(output)
    validate_authoring_result(result, mode, workload, iterations, sim_cycles)
    result["process_wall_ms"] = process_wall_ms
    result["command"] = command
    try:
        result["load_average_1m"] = os.getloadavg()[0]
    except OSError:
        result["load_average_1m"] = None
    return result


def build_authoring_backends(workloads: tuple[str, ...]):
    targets = [
        str(AUTHORING_BUILD / f"cpp_dpi_{workload}" / "Vdpi_authoring_core")
        for workload in workloads
    ]
    targets.extend(
        [
            "authoring-core-sv-build",
            "framework-comparison-vpi-build",
            "framework-comparison-cocotb-build",
        ]
    )
    output, _ = run_command(["make", *targets])
    if output.strip():
        print(output, end="", flush=True)


def median(values):
    return statistics.median(values)


def summarize(samples: list[dict]) -> dict:
    process_values = [sample["process_wall_ms"] for sample in samples]
    internal_values = [
        float(sample["wall_ms"]) for sample in samples if "wall_ms" in sample
    ]
    result = {
        "runs": len(samples),
        "process_wall_ms_median": median(process_values),
        "process_wall_ms_min": min(process_values),
        "process_wall_ms_max": max(process_values),
        "samples": samples,
    }
    if internal_values:
        result["internal_wall_ms_median"] = median(internal_values)
    return result


def compare_authoring_workload(
    workload: str, iterations: int, runs: int, *, semantic_only: bool, journal
) -> dict:
    print(f"[{workload}] semantic preflight", flush=True)
    preflight = {}
    reference_cycles = None
    for mode in MODES:
        sample = run_sample(
            mode, workload, min(iterations, 100), sim_cycles=reference_cycles
        )
        if reference_cycles is None:
            reference_cycles = sample["sim_cycles"]
        validate_authoring_result(
            sample, mode, workload, min(iterations, 100), reference_cycles
        )
        preflight[mode] = sample

    if semantic_only:
        return {
            "description": DESCRIPTIONS[workload].__dict__,
            "iterations": min(iterations, 100),
            "semantic_only": True,
            "reference_sim_cycles": reference_cycles,
            "preflight": preflight,
        }

    print(f"[{workload}] warming four modes", flush=True)
    warmup = {}
    measured_reference_cycles = None
    for mode in MODES:
        sample = run_sample(
            mode, workload, iterations, sim_cycles=measured_reference_cycles
        )
        if measured_reference_cycles is None:
            measured_reference_cycles = sample["sim_cycles"]
        warmup[mode] = sample

    samples_by_mode = {mode: [] for mode in MODES}
    sequence = 0
    for round_index in range(runs):
        order = rotated_mode_order(round_index)
        print(
            f"[{workload}] round {round_index + 1}/{runs}: {' -> '.join(order)}",
            flush=True,
        )
        round_samples = {}
        for slot, mode in enumerate(order, start=1):
            sample = run_sample(
                mode,
                workload,
                iterations,
                sim_cycles=measured_reference_cycles,
            )
            sample.update(
                {
                    "round": round_index + 1,
                    "slot": slot,
                    "order": list(order),
                    "sequence": sequence,
                }
            )
            sequence += 1
            samples_by_mode[mode].append(sample)
            round_samples[mode] = sample
            journal.write(json.dumps(sample, sort_keys=True) + "\n")
            journal.flush()
            os.fsync(journal.fileno())

        evidence = {
            mode: (
                round_samples[mode]["transactions"],
                round_samples[mode]["checks"],
                round_samples[mode]["sim_cycles"],
                round_samples[mode]["checksum"],
                round_samples[mode]["failures"],
            )
            for mode in MODES
        }
        if len(set(evidence.values())) != 1:
            raise BenchmarkError(
                f"{workload}: cross-mode evidence mismatch in round "
                f"{round_index + 1}: {evidence}"
            )

    summaries = {mode: summarize(samples) for mode, samples in samples_by_mode.items()}
    sv_ms = summaries["pure_sv"]["process_wall_ms_median"]
    dpi_ms = summaries["cpp_dpi"]["process_wall_ms_median"]
    ratios = {
        f"{mode}_over_pure_sv": summaries[mode]["process_wall_ms_median"] / sv_ms
        for mode in MODES
    }
    ratios.update(
        {
            "cpp_vpi_over_cpp_dpi": summaries["cpp_vpi"][
                "process_wall_ms_median"
            ]
            / dpi_ms,
            "cocotb_over_cpp_dpi": summaries["cocotb"][
                "process_wall_ms_median"
            ]
            / dpi_ms,
        }
    )
    return {
        "description": DESCRIPTIONS[workload].__dict__,
        "iterations": iterations,
        "semantic_only": False,
        "reference_sim_cycles": measured_reference_cycles,
        "preflight": preflight,
        "warmup": warmup,
        "modes": summaries,
        "ratios": ratios,
    }


def refresh_peripheral(iterations: int, runs: int):
    print("[peripheral_suite] running existing four-framework benchmark", flush=True)
    command = [
        sys.executable,
        str(PERIPHERAL_RUNNER),
        "--iters",
        str(iterations),
        "--runs",
        str(runs),
        "--comparison-runs",
        "16",
    ]
    output, _ = run_command(command)
    if output.strip():
        print(output, end="", flush=True)


def load_peripheral_summary() -> dict:
    if not PERIPHERAL_RESULT.is_file():
        raise BenchmarkError(
            "peripheral-suite result is missing; run with --refresh-peripheral"
        )
    data = json.loads(PERIPHERAL_RESULT.read_text(encoding="utf-8"))
    if data.get("status") not in {"passed", "success"}:
        raise BenchmarkError(
            f"peripheral-suite result is not successful: {data.get('status')!r}"
        )
    modes = {
        mode: {
            key: data[mode][key]
            for key in data[mode]
            if key
            in {
                "runs",
                "process_wall_ms_median",
                "internal_wall_ms_median",
                "samples",
            }
        }
        for mode in MODES
    }
    ratios = {
        "pure_sv_over_pure_sv": 1.0,
        "cpp_dpi_over_pure_sv": data["speedup"][
            "dpi_over_systemverilog_process_wall"
        ],
        "cpp_vpi_over_pure_sv": data["speedup"][
            "cpp_over_systemverilog_process_wall"
        ],
        "cocotb_over_pure_sv": data["speedup"][
            "cocotb_over_systemverilog_process_wall"
        ],
        "cpp_vpi_over_cpp_dpi": data["speedup"]["cpp_over_dpi_process_wall"],
        "cocotb_over_cpp_dpi": data["speedup"][
            "cocotb_over_dpi_process_wall"
        ],
    }
    return {
        "description": DESCRIPTIONS["peripheral_suite"].__dict__,
        "iterations": data["iterations"],
        "semantic_only": False,
        "reference_sim_cycles": data["pure_sv"]["samples"][0]["sim_cycles"],
        "modes": modes,
        "ratios": ratios,
        "source": str(PERIPHERAL_RESULT.relative_to(REPO)),
        "source_metadata": data.get("metadata", {}),
    }


def command_output(command):
    try:
        completed = subprocess.run(
            command,
            cwd=REPO,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        return completed.stdout.strip() if completed.returncode == 0 else None
    except OSError:
        return None


def metadata(argv) -> dict:
    return {
        "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "host": platform.node(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "python": platform.python_version(),
        "verilator": command_output(["verilator", "--version"]),
        "cocotb": "2.0.1",
        "command": [sys.executable, str(Path(__file__).relative_to(REPO)), *argv],
    }


def evaluate_performance_guard(workloads: dict) -> dict:
    measured = {
        name: workload["ratios"]["cpp_dpi_over_pure_sv"]
        for name, workload in workloads.items()
        if not workload.get("semantic_only") and "ratios" in workload
    }
    violations = {
        name: ratio
        for name, ratio in measured.items()
        if ratio > MAX_CPP_DPI_OVER_PURE_SV
    }
    return {
        "metric": "C++ DPI median process wall / matching pure SV median process wall",
        "max_ratio": MAX_CPP_DPI_OVER_PURE_SV,
        "ratios": measured,
        "violations": violations,
        "status": "hard_failure" if violations else "passed",
    }


def markdown(result: dict) -> str:
    lines = [
        "# Framework performance comparison",
        "",
        "All values are median process wall time. Ratios are normalized to the",
        "matching pure SystemVerilog implementation (`1.00x`).",
        "",
        "| Workload | Iterations | Pure SV | C++ DPI | C++ VPI | Cocotb | VPI / DPI | Cocotb / DPI |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for name, workload in result["workloads"].items():
        if workload.get("semantic_only"):
            continue
        ratios = workload["ratios"]
        modes = workload["modes"]
        lines.append(
            f"| {workload['description']['title']} | {workload['iterations']:,} "
            f"| {modes['pure_sv']['process_wall_ms_median']:.1f} ms / 1.00x "
            f"| {modes['cpp_dpi']['process_wall_ms_median']:.1f} ms / {ratios['cpp_dpi_over_pure_sv']:.2f}x "
            f"| {modes['cpp_vpi']['process_wall_ms_median']:.1f} ms / {ratios['cpp_vpi_over_pure_sv']:.2f}x "
            f"| {modes['cocotb']['process_wall_ms_median']:.1f} ms / {ratios['cocotb_over_pure_sv']:.2f}x "
            f"| {ratios['cpp_vpi_over_cpp_dpi']:.2f}x "
            f"| {ratios['cocotb_over_cpp_dpi']:.2f}x |"
        )
    lines.extend(
        [
            "",
            "Each authoring workload is warmed once per mode and then measured in",
            "rotating four-mode order. Only one simulator process runs at a time.",
            "Samples are accepted only when transactions, checks, simulation cycles,",
            "checksum, feature counters, and failures match across all four modes.",
            "",
        ]
    )
    guard = result.get("performance_guard")
    if guard:
        lines.extend(
            [
                f"Performance guard: **{guard['status']}** "
                f"(C++ DPI / pure SV must be <= {guard['max_ratio']:.2f}x).",
                "",
            ]
        )
        for name, ratio in guard["violations"].items():
            lines.append(f"- `{name}`: `{ratio:.3f}x`")
        if guard["violations"]:
            lines.append("")
    return "\n".join(lines)


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--iters", type=int, default=50_000)
    parser.add_argument("--runs", type=int, default=8)
    parser.add_argument(
        "--workload",
        action="append",
        choices=AUTHORING_WORKLOADS,
        dest="workloads",
    )
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument(
        "--reuse-authoring",
        action="store_true",
        help="reuse complete authoring workloads from results/latest.json",
    )
    parser.add_argument("--semantic-only", action="store_true")
    parser.add_argument("--skip-peripheral", action="store_true")
    parser.add_argument("--refresh-peripheral", action="store_true")
    parser.add_argument("--peripheral-iters", type=int, default=10_000)
    parser.add_argument("--peripheral-runs", type=int, default=3)
    args = parser.parse_args(argv)
    if args.iters <= 0:
        parser.error("--iters must be positive")
    if args.runs < 4 or args.runs % 4:
        parser.error("--runs must be a positive multiple of four and at least four")
    if args.peripheral_iters <= 0 or args.peripheral_runs <= 0:
        parser.error("peripheral iteration and run counts must be positive")
    return args


def main(argv=None):
    args = parse_args(argv)
    selected = tuple(args.workloads or AUTHORING_WORKLOADS)
    previous = None
    latest_path = RESULT_DIR / "latest.json"
    if args.reuse_authoring:
        if not latest_path.is_file():
            raise BenchmarkError("--reuse-authoring requires results/latest.json")
        previous = json.loads(latest_path.read_text(encoding="utf-8"))
        for workload in selected:
            entry = previous.get("workloads", {}).get(workload)
            if not entry or entry.get("semantic_only"):
                raise BenchmarkError(
                    f"--reuse-authoring has no complete result for {workload}"
                )
            if entry.get("iterations") != args.iters:
                raise BenchmarkError(
                    f"--reuse-authoring iteration mismatch for {workload}: "
                    f"{entry.get('iterations')} != {args.iters}"
                )

    if not args.skip_build and not args.reuse_authoring:
        print("building comparison backends", flush=True)
        build_authoring_backends(selected)

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    journal_path = RESULT_DIR / "latest.jsonl"
    if not args.reuse_authoring:
        journal_path.write_text("", encoding="utf-8")

    result = {
        "status": "running",
        "metadata": metadata(sys.argv[1:] if argv is None else list(argv)),
        "methodology": {
            "serial_processes": True,
            "mode_order": "four-mode cyclic rotation",
            "warmups_per_mode": 1,
            "runs_per_authoring_mode": args.runs,
            "semantic_gate": [
                "iterations",
                "transactions",
                "checks",
                "sim_cycles",
                "checksum",
                "failures",
                "wide_echo_137",
                "signal_edges",
            ],
            "primary_metric": "median process wall time",
            "normalization": "matching pure SystemVerilog process median",
        },
        "workloads": {},
    }
    if previous is not None:
        result["metadata"]["reused_authoring_timestamp_utc"] = previous.get(
            "metadata", {}
        ).get("timestamp_utc")
        result["workloads"].update(
            {workload: previous["workloads"][workload] for workload in selected}
        )

    try:
        if previous is None:
            with journal_path.open("a", encoding="utf-8") as journal:
                for workload in selected:
                    result["workloads"][workload] = compare_authoring_workload(
                        workload,
                        args.iters,
                        args.runs,
                        semantic_only=args.semantic_only,
                        journal=journal,
                    )

        if not args.skip_peripheral and not args.semantic_only:
            if args.refresh_peripheral:
                refresh_peripheral(args.peripheral_iters, args.peripheral_runs)
            result["workloads"]["peripheral_suite"] = load_peripheral_summary()
        result["performance_guard"] = evaluate_performance_guard(
            result["workloads"]
        )
        result["status"] = result["performance_guard"]["status"]
    except Exception as error:
        result["status"] = "failed"
        result["error"] = str(error)
        RESULT_DIR.mkdir(parents=True, exist_ok=True)
        (RESULT_DIR / "latest.json").write_text(
            json.dumps(result, indent=2) + "\n", encoding="utf-8"
        )
        raise

    result_json = json.dumps(result, indent=2) + "\n"
    result_markdown = markdown(result)
    (RESULT_DIR / "latest.json").write_text(result_json, encoding="utf-8")
    (RESULT_DIR / "latest.md").write_text(result_markdown, encoding="utf-8")
    print(result_markdown, flush=True)
    return 1 if result["status"] == "hard_failure" else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BenchmarkError as error:
        print(f"framework-comparison: {error}", file=sys.stderr)
        raise SystemExit(1)
