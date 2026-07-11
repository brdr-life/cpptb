#!/usr/bin/env python3
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
BENCH_DIR = REPO / "benchmarks" / "cocotb_cpp_compare"
RESULT_DIR = BENCH_DIR / "results"
CPP_BINARY = REPO / "build" / "benchmarks" / "cocotb_cpp_compare" / "apb_event_bench_host"
COCOTB_RUNNER = BENCH_DIR / "run_cocotb.py"
COCOTB_PYTHON = os.environ.get("COCOTB_BENCH_PYTHON", "/opt/homebrew/bin/python3.12")

RESULT_RE = re.compile(r"(?P<name>[A-Z_]+_BENCH_RESULT)\s+(?P<fields>.*)")


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
    return {
        "label": label,
        "runs": len(samples),
        "internal_wall_ms_median": median([sample["internal_wall_ms"] for sample in samples]),
        "process_wall_ms_median": median([sample["process_wall_ms"] for sample in samples]),
        "samples": samples,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iters", type=int, default=1000)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args()

    if not args.skip_build:
        run_command(["make", "cpp-apb-event-bench-build"])
        run_command(
            [
                "uv",
                "run",
                "--python",
                COCOTB_PYTHON,
                "--with",
                "cocotb",
                "python",
                str(COCOTB_RUNNER),
                "--iters",
                str(args.iters),
                "--build-only",
            ]
        )

    cpp_samples = []
    cocotb_samples = []

    for run_index in range(args.runs):
        cpp_env = os.environ.copy()
        cpp_env["CPPTB_BENCH_ITERS"] = str(args.iters)
        cpp_output, cpp_process_ms = run_command([str(CPP_BINARY)], env=cpp_env)
        cpp_result = parse_result(cpp_output, "CPPTB_BENCH_RESULT")
        cpp_samples.append(
            {
                "run": run_index + 1,
                "internal_wall_ms": float(cpp_result["wall_ms"]),
                "process_wall_ms": cpp_process_ms,
                "checks": int(cpp_result["checks"]),
                "failures": int(cpp_result["failures"]),
            }
        )

        cocotb_output, cocotb_process_ms = run_command(
            [
                "uv",
                "run",
                "--python",
                COCOTB_PYTHON,
                "--with",
                "cocotb",
                "python",
                str(COCOTB_RUNNER),
                "--iters",
                str(args.iters),
                "--no-build",
            ]
        )
        cocotb_result = parse_result(cocotb_output, "COCOTB_BENCH_RESULT")
        cocotb_samples.append(
            {
                "run": run_index + 1,
                "internal_wall_ms": float(cocotb_result["wall_ms"]),
                "process_wall_ms": cocotb_process_ms,
                "checks": int(cocotb_result["checks"]),
                "failures": int(cocotb_result["failures"]),
            }
        )

    cpp_summary = summarize("cpptb", cpp_samples)
    cocotb_summary = summarize("cocotb", cocotb_samples)

    internal_speedup = (
        cocotb_summary["internal_wall_ms_median"]
        / cpp_summary["internal_wall_ms_median"]
    )
    process_speedup = (
        cocotb_summary["process_wall_ms_median"]
        / cpp_summary["process_wall_ms_median"]
    )

    result = {
        "iterations": args.iters,
        "runs": args.runs,
        "design": "vpi_apb_event_unit / apb_event_unit_peakrdl",
        "cpp": cpp_summary,
        "cocotb": cocotb_summary,
        "speedup": {
            "internal_wall": internal_speedup,
            "process_wall": process_speedup,
        },
    }

    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    json_path = RESULT_DIR / "latest.json"
    md_path = RESULT_DIR / "latest.md"
    json_path.write_text(json.dumps(result, indent=2) + "\n")
    md_path.write_text(
        "\n".join(
            [
                "# cocotb vs C++ coroutine benchmark",
                "",
                f"- Design: `{result['design']}`",
                f"- Iterations per run: `{args.iters}`",
                f"- Runs: `{args.runs}`",
                f"- C++ internal median: `{cpp_summary['internal_wall_ms_median']:.3f} ms`",
                f"- cocotb internal median: `{cocotb_summary['internal_wall_ms_median']:.3f} ms`",
                f"- Internal speedup: `{internal_speedup:.2f}x`",
                f"- C++ process median: `{cpp_summary['process_wall_ms_median']:.3f} ms`",
                f"- cocotb process median: `{cocotb_summary['process_wall_ms_median']:.3f} ms`",
                f"- Process speedup: `{process_speedup:.2f}x`",
                "",
            ]
        )
    )

    print(md_path.read_text())
    print(f"Wrote {json_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
