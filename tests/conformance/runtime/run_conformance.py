#!/usr/bin/env python3
"""Build and run the cpptb scheduler conformance suite."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[3]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from cpptb_codegen import CodegenError, generate


SUITE_DIR = Path(__file__).resolve().parent
MANIFEST_PATH = SUITE_DIR / "scheduler_conformance.dpi.json"
BUILD_ROOT = REPO / "build" / "cpptb"
RESULT_PATTERN = re.compile(
    r"CPPTB_CONFORMANCE_RESULT "
    r"iterations=(?P<iterations>\d+) "
    r"checks=(?P<checks>\d+) "
    r"sim_cycles=(?P<sim_cycles>\d+) "
    r"wall_ms=(?P<wall_ms>[0-9.]+) "
    r"failures=(?P<failures>\d+)"
)


def load_manifest() -> dict[str, Any]:
    return json.loads(MANIFEST_PATH.read_text())


def simulator_name(manifest: dict[str, Any], override: str | None) -> str:
    return override or manifest.get("simulator", "verilator")


def build_dir(timing_backend: str) -> Path:
    suffixes = {
        "direct": "conformance_obj",
        "vpi": "conformance_vpi_obj",
        "sv-dpi-inline": "conformance_sv_dpi_inline_obj",
        "sv-dpi-nba": "conformance_sv_dpi_nba_obj",
        "sv-dpi-calendar": "conformance_sv_dpi_calendar_obj",
    }
    suffix = suffixes[timing_backend]
    return BUILD_ROOT / suffix


def build_verilator(manifest: dict[str, Any], timing_backend: str) -> Path:
    options = manifest.get("simulator_options", {}).get("verilator", {})
    compile_args = options.get("compile_args", [])
    if not isinstance(compile_args, list):
        raise SystemExit("simulator_options.verilator.compile_args must be a list")

    output_dir = build_dir(timing_backend)
    output_dir.mkdir(parents=True, exist_ok=True)
    sources = [
        str((MANIFEST_PATH.parent / source).resolve())
        for source in manifest["sources"]
    ]
    wrapper = str(
        (MANIFEST_PATH.parent / manifest["outputs"]["sv_wrapper"]).resolve()
    )
    cpp_sources = [
        str(SUITE_DIR / "dpi_transport.cpp"),
        str(SUITE_DIR / "framework.cpp"),
        str(SUITE_DIR / "testbench.cpp"),
    ]
    sv_dpi_timing = timing_backend.startswith("sv-dpi-")
    timing_bridge = bool(options.get("timing_bridge", False)) and not sv_dpi_timing
    if sv_dpi_timing:
        compile_args = [argument for argument in compile_args if argument != "--vpi"]
    if timing_bridge:
        compile_args = [argument for argument in compile_args if argument != "--binary"]
        for argument in ("--cc", "--exe", "--build", "--vpi"):
            if argument not in compile_args:
                compile_args.append(argument)
        cpp_sources.append(str(REPO / "src" / "verilator_timing_main.cpp"))
    top_class = f"V{manifest['top_module']}"
    cflags = f"-std=c++20 -I{REPO} -I{REPO / 'include'}"
    sv_defines: list[str] = []
    if timing_bridge:
        cflags += f" -DCPPTB_VERILATED_TOP={top_class}"
        if timing_backend == "direct":
            cflags += " -DCPPTB_VERILATOR_DIRECT_TIMING"
    if sv_dpi_timing:
        cflags += " -DCPPTB_SV_DPI_TIMING"
        sv_defines.append("-DCPPTB_SV_DPI_TIMING")
        if timing_backend == "sv-dpi-inline":
            # The inline pump violates the phase contract and dpi_runtime.hpp
            # rejects it at compile time. This suite builds it on purpose, to
            # measure what it does, which is what the escape hatch is for.
            cflags += " -DCPPTB_ALLOW_INVALID_TIMING"
        if timing_backend == "sv-dpi-nba":
            cflags += " -DCPPTB_SV_DPI_NBA_TIMING"
            sv_defines.append("-DCPPTB_SV_DPI_NBA_TIMING")
        if timing_backend == "sv-dpi-calendar":
            cflags += (
                " -DCPPTB_SV_DPI_NBA_TIMING"
                " -DCPPTB_SV_DPI_CALENDAR_TIMING"
            )
            sv_defines.extend(
                (
                    "-DCPPTB_SV_DPI_NBA_TIMING",
                    "-DCPPTB_SV_DPI_CALENDAR_TIMING",
                )
            )
    command = [
        "verilator",
        *compile_args,
        *sv_defines,
        "-CFLAGS",
        cflags,
        "--Mdir",
        str(output_dir),
        "--top-module",
        manifest["top_module"],
        *sources,
        wrapper,
        *cpp_sources,
    ]
    completed = subprocess.run(command, cwd=REPO, check=False)
    if completed.returncode != 0:
        raise SystemExit(f"Verilator conformance build failed with {completed.returncode}")
    return output_dir / top_class


def build(manifest: dict[str, Any], simulator: str, timing_backend: str) -> Path:
    try:
        generate(MANIFEST_PATH)
    except CodegenError as error:
        raise SystemExit(f"conformance code generation failed: {error}") from error

    if simulator == "verilator":
        return build_verilator(manifest, timing_backend)
    configured = ", ".join(sorted(manifest.get("simulator_options", {})))
    raise SystemExit(
        f"simulator {simulator!r} has no conformance runner; configured: {configured}"
    )


def binary_path(
    manifest: dict[str, Any], simulator: str, timing_backend: str
) -> Path:
    if simulator == "verilator":
        return build_dir(timing_backend) / f"V{manifest['top_module']}"
    raise SystemExit(f"simulator {simulator!r} has no conformance binary mapping")


def parse_result(output: str) -> dict[str, int | float]:
    match = RESULT_PATTERN.search(output)
    if match is None:
        raise SystemExit("conformance result line was not found in simulator output")
    return {
        "iterations": int(match.group("iterations")),
        "checks": int(match.group("checks")),
        "sim_cycles": int(match.group("sim_cycles")),
        "wall_ms": float(match.group("wall_ms")),
        "failures": int(match.group("failures")),
    }


def validate_result(contract: dict[str, Any], result: dict[str, int | float]) -> None:
    expected = {
        "iterations": int(contract["expected_iterations"]),
        "checks": int(contract["expected_checks"]),
        "sim_cycles": int(contract["expected_sim_cycles"]),
        "failures": int(contract["expected_failures"]),
    }
    mismatches = [
        f"{key}={result[key]} expected {value}"
        for key, value in expected.items()
        if result[key] != value
    ]
    if mismatches:
        raise SystemExit("conformance result mismatch: " + ", ".join(mismatches))


def run(
    manifest: dict[str, Any], simulator: str, binary: Path, timing_backend: str
) -> None:
    if not binary.exists():
        raise SystemExit(f"conformance binary does not exist: {binary}")
    iterations = int(manifest["conformance"]["expected_iterations"])
    plusarg = manifest["run"]["iteration_plusarg"]
    completed = subprocess.run(
        [str(binary), f"+{plusarg}={iterations}"],
        cwd=REPO,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    print(completed.stdout, end="")
    if completed.returncode != 0:
        raise SystemExit(
            f"{simulator} conformance simulation failed with {completed.returncode}"
        )
    result = parse_result(completed.stdout)
    validate_result(manifest["conformance"], result)
    print(
        "CPPTB_CONFORMANCE_PASS "
        f"simulator={simulator} timing_backend={timing_backend} "
        f"checks={result['checks']} "
        f"sim_cycles={result['sim_cycles']}"
    )

    for positive_case in manifest["conformance"].get("positive_cases", []):
        case_name = str(positive_case["case"])
        case_iterations = int(positive_case["expected_iterations"])
        environment = os.environ.copy()
        environment["CPPTB_CONFORMANCE_POSITIVE_CASE"] = case_name
        case_run = subprocess.run(
            [str(binary), f"+{plusarg}={case_iterations}"],
            cwd=REPO,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        print(case_run.stdout, end="")
        if case_run.returncode != 0:
            raise SystemExit(
                f"positive conformance case {case_name!r} failed with "
                f"{case_run.returncode}"
            )
        case_result = parse_result(case_run.stdout)
        validate_result(positive_case, case_result)
        print(
            "CPPTB_CONFORMANCE_CASE_PASS "
            f"simulator={simulator} case={case_name} "
            f"checks={case_result['checks']} "
            f"sim_cycles={case_result['sim_cycles']}"
        )

    for violation in manifest["conformance"].get("negative_cases", []):
        violation_iterations = int(violation["iterations"])
        expected_message = str(violation["message"])
        environment = os.environ.copy()
        if negative_case := violation.get("case"):
            environment["CPPTB_CONFORMANCE_NEGATIVE_CASE"] = str(negative_case)
        violation_run = subprocess.run(
            [str(binary), f"+{plusarg}={violation_iterations}"],
            cwd=REPO,
            env=environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        if violation_run.returncode == 0:
            raise SystemExit(
                f"negative mode {violation_iterations} unexpectedly passed"
            )
        if expected_message not in violation_run.stdout:
            print(violation_run.stdout, end="")
            raise SystemExit(
                f"negative mode {violation_iterations} did not report "
                f"{expected_message!r}"
            )
        print(
            "CPPTB_CONFORMANCE_NEGATIVE_PASS "
            f"simulator={simulator} mode={violation_iterations}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--simulator", help="override the manifest simulator")
    parser.add_argument(
        "--timing-backend",
        choices=(
            "direct",
            "vpi",
            "sv-dpi-inline",
            "sv-dpi-nba",
            "sv-dpi-calendar",
        ),
        default="direct",
        help="Verilator timing dispatch backend (default: direct)",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--build-only", action="store_true")
    mode.add_argument("--no-build", action="store_true")
    args = parser.parse_args()

    manifest = load_manifest()
    simulator = simulator_name(manifest, args.simulator)
    binary = (
        binary_path(manifest, simulator, args.timing_backend)
        if args.no_build
        else build(manifest, simulator, args.timing_backend)
    )
    if not args.build_only:
        run(manifest, simulator, binary, args.timing_backend)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
