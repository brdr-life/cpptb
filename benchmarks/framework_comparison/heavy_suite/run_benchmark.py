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


REPO = Path(__file__).resolve().parents[3]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from benchmarks.framework_comparison.heavy_suite.workload import (  # noqa: E402
    DEFAULT_ITERATIONS,
    DESCRIPTIONS,
    MODES,
    WORKLOADS,
    expected_result,
    rotated_mode_order,
)


SUITE_DIR = Path(__file__).resolve().parent
BUILD_DIR = REPO / "build" / "benchmarks" / "framework_comparison" / "heavy_suite"
RESULT_DIR = SUITE_DIR / "results"
SV_BINARY = BUILD_DIR / "pure_sv_obj" / "Vheavy_benchmark_sv_tb"
VPI_BINARY = BUILD_DIR / "cpp_vpi_obj" / "Vheavy_benchmark_vpi_top"
COCOTB_RUNNER = SUITE_DIR / "testbenches" / "cocotb" / "run_cocotb.py"
COCOTB_PYTHON = os.environ.get("COCOTB_BENCH_PYTHON", "3.12")
RESULT_RE = re.compile(r"HEAVY_BENCH_RESULT\s+(?P<fields>.*)")
MAX_CPP_DPI_OVER_PURE_SV = 1.10


class BenchmarkError(RuntimeError):
    pass


def parse_result(output: str) -> dict:
    for line in output.splitlines():
        match = RESULT_RE.search(line)
        if not match:
            continue
        result = {}
        for token in match.group("fields").split():
            if "=" not in token:
                continue
            key, value = token.split("=", 1)
            try:
                result[key] = int(value, 0)
            except ValueError:
                try:
                    result[key] = float(value)
                except ValueError:
                    result[key] = value
        return result
    raise BenchmarkError("heavy benchmark output has no HEAVY_BENCH_RESULT line")


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
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    if completed.returncode != 0:
        raise BenchmarkError(
            f"command failed ({completed.returncode}): {' '.join(map(str, command))}\n"
            f"{completed.stdout}"
        )
    return completed.stdout, elapsed_ms


def command_for(mode: str, workload: str, iterations: int):
    if mode == "pure_sv":
        return [SV_BINARY, f"+HEAVY_BENCH_ITERS={iterations}", f"+HEAVY_BENCH_WORKLOAD={workload}"], None
    if mode == "cpp_dpi":
        return [BUILD_DIR / f"cpp_dpi_{workload}" / "Vdpi_heavy_benchmark", f"+HEAVY_BENCH_ITERS={iterations}"], None
    if mode == "cpp_vpi":
        env = os.environ.copy()
        env.update({"HEAVY_BENCH_WORKLOAD": workload, "HEAVY_BENCH_ITERS": str(iterations)})
        return [VPI_BINARY], env
    if mode == "cocotb":
        env = os.environ.copy()
        env["UV_CACHE_DIR"] = str(REPO / "build" / "uv-cache")
        return [
            "uv", "run", "--offline", "--no-project", "--python", COCOTB_PYTHON,
            "--with", "cocotb==2.0.1", "python", COCOTB_RUNNER,
            "--workload", workload, "--iters", str(iterations), "--no-build",
        ], env
    raise ValueError(f"unknown mode: {mode}")


def validate(result: dict, mode: str, workload: str, iterations: int, sim_cycles=None):
    expected = expected_result(workload, iterations)
    mismatches = {
        key: (result.get(key), value)
        for key, value in expected.items()
        if result.get(key) != value
    }
    if result.get("mode") != mode:
        mismatches["mode"] = (result.get("mode"), mode)
    if result.get("workload") != workload:
        mismatches["workload"] = (result.get("workload"), workload)
    if sim_cycles is not None and result.get("sim_cycles") != sim_cycles:
        mismatches["sim_cycles"] = (result.get("sim_cycles"), sim_cycles)
    if mismatches:
        detail = ", ".join(f"{key}: {actual!r} != {wanted!r}" for key, (actual, wanted) in mismatches.items())
        raise BenchmarkError(f"{workload}/{mode} semantic mismatch: {detail}")


def sample(mode: str, workload: str, iterations: int, sim_cycles=None) -> dict:
    command, env = command_for(mode, workload, iterations)
    output, process_ms = run_command(command, env=env)
    result = parse_result(output)
    validate(result, mode, workload, iterations, sim_cycles)
    result["process_wall_ms"] = process_ms
    result["command"] = [str(part) for part in command]
    try:
        result["load_average_1m"] = os.getloadavg()[0]
    except OSError:
        result["load_average_1m"] = None
    return result


def summarize(samples: list[dict]) -> dict:
    process = [entry["process_wall_ms"] for entry in samples]
    internal = [float(entry["wall_ms"]) for entry in samples if "wall_ms" in entry]
    result = {
        "runs": len(samples),
        "process_wall_ms_median": statistics.median(process),
        "process_wall_ms_min": min(process),
        "process_wall_ms_max": max(process),
        "samples": samples,
    }
    if internal:
        result["internal_wall_ms_median"] = statistics.median(internal)
    return result


def compare(workload: str, iterations: int, runs: int, journal) -> dict:
    preflight_iterations = min(iterations, 3)
    print(f"[{workload}] semantic preflight ({preflight_iterations})", flush=True)
    reference = None
    preflight = {}
    for mode in MODES:
        entry = sample(mode, workload, preflight_iterations, reference)
        reference = entry["sim_cycles"] if reference is None else reference
        preflight[mode] = entry

    print(f"[{workload}] warmup ({iterations:,})", flush=True)
    reference = None
    warmup = {}
    for mode in MODES:
        entry = sample(mode, workload, iterations, reference)
        reference = entry["sim_cycles"] if reference is None else reference
        warmup[mode] = entry

    samples = {mode: [] for mode in MODES}
    for round_index in range(runs):
        order = rotated_mode_order(round_index)
        print(f"[{workload}] round {round_index + 1}/{runs}: {' -> '.join(order)}", flush=True)
        round_entries = {}
        for slot, mode in enumerate(order, start=1):
            entry = sample(mode, workload, iterations, reference)
            entry.update({"round": round_index + 1, "slot": slot, "order": list(order)})
            samples[mode].append(entry)
            round_entries[mode] = entry
            journal.write(json.dumps(entry, sort_keys=True) + "\n")
            journal.flush()
            os.fsync(journal.fileno())
        evidence = {
            mode: tuple(round_entries[mode][key] for key in ("transactions", "checks", "sim_cycles", "checksum", "failures"))
            for mode in MODES
        }
        if len(set(evidence.values())) != 1:
            raise BenchmarkError(f"{workload}: cross-mode evidence mismatch: {evidence}")

    modes = {mode: summarize(entries) for mode, entries in samples.items()}
    sv_ms = modes["pure_sv"]["process_wall_ms_median"]
    dpi_ms = modes["cpp_dpi"]["process_wall_ms_median"]
    ratios = {f"{mode}_over_pure_sv": modes[mode]["process_wall_ms_median"] / sv_ms for mode in MODES}
    ratios["cpp_vpi_over_cpp_dpi"] = modes["cpp_vpi"]["process_wall_ms_median"] / dpi_ms
    ratios["cocotb_over_cpp_dpi"] = modes["cocotb"]["process_wall_ms_median"] / dpi_ms
    return {
        "description": DESCRIPTIONS[workload].__dict__,
        "iterations": iterations,
        "reference_sim_cycles": reference,
        "preflight": preflight,
        "warmup": warmup,
        "modes": modes,
        "ratios": ratios,
    }


def metadata(argv) -> dict:
    def output(command):
        completed = subprocess.run(command, cwd=REPO, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
        return completed.stdout.strip() if completed.returncode == 0 else None

    return {
        "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "host": platform.node(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "verilator": output(["verilator", "--version"]),
        "cocotb": "2.0.1",
        "command": [sys.executable, str(Path(__file__).relative_to(REPO)), *argv],
    }


def render_markdown(result: dict) -> str:
    lines = [
        "# Heavy framework comparison",
        "",
        "Median whole-process wall time; ratios are normalized to the exact matching pure-SV testbench.",
        "",
        "| Workload | Units | Cycles | Pure SV | C++ DPI | C++ VPI | Cocotb |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for workload in WORKLOADS:
        if workload not in result["workloads"]:
            continue
        entry = result["workloads"][workload]
        modes = entry["modes"]
        ratios = entry["ratios"]
        lines.append(
            f"| {entry['description']['title']} | {entry['iterations']:,} | {entry['reference_sim_cycles']:,} "
            f"| {modes['pure_sv']['process_wall_ms_median']:.1f} ms / 1.00x "
            f"| {modes['cpp_dpi']['process_wall_ms_median']:.1f} ms / {ratios['cpp_dpi_over_pure_sv']:.2f}x "
            f"| {modes['cpp_vpi']['process_wall_ms_median']:.1f} ms / {ratios['cpp_vpi_over_pure_sv']:.2f}x "
            f"| {modes['cocotb']['process_wall_ms_median']:.1f} ms / {ratios['cocotb_over_pure_sv']:.2f}x |"
        )
    guard = result["performance_guard"]
    lines.extend([
        "",
        "Runs are serialized and mode order rotates each round. Every sample is accepted only after transactions, checks, simulation cycles, checksum, and failures match across all four modes.",
        "",
        f"C++ DPI / pure-SV 1.10x guard: **{guard['status']}**. Heavy-suite runs are exploratory, so this is advisory unless `--enforce-guard` is passed.",
        "",
    ])
    for name, ratio in guard["violations"].items():
        lines.append(f"- `{name}`: `{ratio:.3f}x`")
    return "\n".join(lines)


def parse_args(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--workload", action="append", choices=WORKLOADS, dest="workloads")
    parser.add_argument("--iters", type=int, help="override workload-specific iteration counts")
    parser.add_argument("--runs", type=int, default=4)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--enforce-guard", action="store_true")
    parser.add_argument("--output-stem", default="latest")
    args = parser.parse_args(argv)
    if args.iters is not None and args.iters <= 0:
        parser.error("--iters must be positive")
    if args.runs < 4 or args.runs % 4:
        parser.error("--runs must be a multiple of four and at least four")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", args.output_stem):
        parser.error("--output-stem must be a simple filename stem")
    return args


def main(argv=None):
    args = parse_args(argv)
    workloads = tuple(args.workloads or WORKLOADS)
    if not args.skip_build:
        output, _ = run_command(["make", "framework-comparison-heavy-build"])
        if output.strip():
            print(output, end="")

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    journal_path = RESULT_DIR / f"{args.output_stem}.jsonl"
    journal_path.write_text("", encoding="utf-8")
    result = {
        "status": "running",
        "metadata": metadata(sys.argv[1:] if argv is None else list(argv)),
        "methodology": {
            "serial_processes": True,
            "mode_order": "four-mode cyclic rotation",
            "warmups_per_mode": 1,
            "runs_per_mode": args.runs,
            "semantic_gate": ["transactions", "checks", "sim_cycles", "checksum", "failures"],
            "primary_metric": "median process wall time",
        },
        "workloads": {},
    }
    try:
        with journal_path.open("a", encoding="utf-8") as journal:
            for workload in workloads:
                iterations = args.iters or DEFAULT_ITERATIONS[workload]
                result["workloads"][workload] = compare(workload, iterations, args.runs, journal)
        ratios = {
            name: entry["ratios"]["cpp_dpi_over_pure_sv"]
            for name, entry in result["workloads"].items()
        }
        violations = {name: ratio for name, ratio in ratios.items() if ratio > MAX_CPP_DPI_OVER_PURE_SV}
        result["performance_guard"] = {
            "max_ratio": MAX_CPP_DPI_OVER_PURE_SV,
            "enforced": args.enforce_guard,
            "ratios": ratios,
            "violations": violations,
            "status": "failed" if violations else "passed",
        }
        result["status"] = "guard_failure" if violations and args.enforce_guard else "passed"
    except Exception as error:
        result["status"] = "error"
        result["error"] = str(error)
        raise
    finally:
        (RESULT_DIR / f"{args.output_stem}.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        if "performance_guard" in result:
            (RESULT_DIR / f"{args.output_stem}.md").write_text(render_markdown(result) + "\n", encoding="utf-8")

    print(render_markdown(result))
    return 1 if result["status"] == "guard_failure" else 0


if __name__ == "__main__":
    sys.exit(main())
