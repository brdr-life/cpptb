#!/usr/bin/env python3
"""Create and verify source fingerprints for authoring-core binaries."""

from __future__ import annotations

import argparse
import datetime
import functools
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess


BENCH_DIR = Path(__file__).resolve().parent
REPO = BENCH_DIR.parents[1]
STAMP_SUFFIX = ".cpptb-build.json"
STAMP_FORMAT = 2

DEFAULT_OPT_FAST = "-O3"
DEFAULT_CONVERGE_LIMIT = "50000000"


def binary_sha256(path: Path) -> str | None:
    try:
        digest = hashlib.sha256()
        with Path(path).open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
    except OSError:
        return None


def _normalized_kernel(mode: str, kernel: str) -> str:
    if mode == "pure_sv" and kernel != "force_direct":
        return "shared"
    return kernel


@functools.lru_cache(maxsize=None)
def _tool_metadata(command: str) -> dict[str, object]:
    arguments = shlex.split(command)
    if not arguments:
        return {
            "command": command,
            "executable": None,
            "returncode": 127,
            "version": "empty tool command",
        }
    resolved = shutil.which(arguments[0])
    try:
        completed = subprocess.run(
            [*arguments, "--version"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
            timeout=10,
        )
        version = completed.stdout.strip()
        returncode = completed.returncode
    except (OSError, subprocess.TimeoutExpired) as error:
        version = str(error)
        returncode = 127
    return {
        "command": command,
        "executable": str(Path(resolved).resolve()) if resolved else None,
        "returncode": returncode,
        "version": version,
    }


def build_configuration(
    *,
    opt_fast: str | None = None,
    converge_limit: str | None = None,
    extra_cflags: str | None = None,
    extra_ldflags: str | None = None,
    cxx: str | None = None,
    cxxflags: str | None = None,
    cppflags: str | None = None,
    ldflags: str | None = None,
    verilator: str | None = None,
) -> dict[str, object]:
    """Return the complete build recipe expected by the benchmark runner."""

    def configured(value: str | None, variable: str, default: str = "") -> str:
        return value if value is not None else os.environ.get(variable, default)

    cxx_command = configured(cxx, "CXX", "c++")
    verilator_command = configured(verilator, "VERILATOR", "verilator")
    return {
        "opt_fast": configured(
            opt_fast, "AUTHORING_CORE_OPT_FAST", DEFAULT_OPT_FAST
        ),
        "converge_limit": configured(
            converge_limit,
            "AUTHORING_CORE_CONVERGE_LIMIT",
            DEFAULT_CONVERGE_LIMIT,
        ),
        "extra_cflags": configured(
            extra_cflags, "AUTHORING_CORE_EXTRA_CFLAGS"
        ),
        "extra_ldflags": configured(
            extra_ldflags, "AUTHORING_CORE_EXTRA_LDFLAGS"
        ),
        "cxxflags": configured(cxxflags, "CXXFLAGS"),
        "cppflags": configured(cppflags, "CPPFLAGS"),
        "ldflags": configured(ldflags, "LDFLAGS"),
        "compiler": _tool_metadata(cxx_command),
        "verilator": _tool_metadata(verilator_command),
    }


def source_paths(mode: str, kernel: str) -> list[Path]:
    kernel = _normalized_kernel(mode, kernel)
    paths = {
        REPO / "Makefile",
        BENCH_DIR / "build_provenance.py",
        BENCH_DIR / "rtl" / "authoring_core_dut.sv",
    }
    if mode == "cpp_dpi":
        paths.update((REPO / "include" / "cpptb").rglob("*.hpp"))
        paths.update((REPO / "include" / "cpptb_vc").rglob("*.hpp"))
        paths.update((REPO / "tools" / "codegen" / "cpptb_codegen").rglob("*.py"))
        paths.update({REPO / "pyproject.toml", REPO / "uv.lock"})
        cpp_dir = BENCH_DIR / "testbenches" / "cpp_dpi"
        paths.update(
            {
                cpp_dir / "authoring_core.dpi.json",
                cpp_dir / "framework" / "authoring_core.hpp",
                cpp_dir / "framework" / "dpi_transport.cpp",
                cpp_dir / "testbench.cpp",
                cpp_dir / "generated" / "authoring_core_binding.hpp",
                cpp_dir / "generated" / "authoring_core_dut.hpp",
                cpp_dir / "generated" / "dpi_authoring_core.sv",
            }
        )
        if kernel == "timing_phases":
            paths.add(REPO / "src" / "verilator_timing_main.cpp")
    elif mode == "pure_sv":
        filename = (
            "force_direct_sv_tb.sv"
            if kernel == "force_direct"
            else "authoring_core_sv_tb.sv"
        )
        paths.add(BENCH_DIR / "testbenches" / "systemverilog" / filename)
    else:
        raise ValueError(f"unknown authoring-core mode: {mode}")
    missing = sorted(path for path in paths if not path.is_file())
    if missing:
        raise FileNotFoundError(f"missing fingerprint inputs: {missing}")
    return sorted(paths, key=lambda path: path.relative_to(REPO).as_posix())


def source_sha256(mode: str, kernel: str) -> tuple[str, list[str]]:
    normalized_kernel = _normalized_kernel(mode, kernel)
    digest = hashlib.sha256()
    digest.update(f"cpptb-authoring-core-v{STAMP_FORMAT}\0".encode())
    digest.update(f"{mode}\0{normalized_kernel}\0".encode())
    relative_paths: list[str] = []
    for path in source_paths(mode, normalized_kernel):
        relative = path.relative_to(REPO).as_posix()
        relative_paths.append(relative)
        digest.update(relative.encode())
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest(), relative_paths


def stamp_path(binary: Path) -> Path:
    binary = Path(binary)
    return binary.with_name(binary.name + STAMP_SUFFIX)


def write_stamp(
    mode: str,
    kernel: str,
    binary: Path,
    configuration: dict[str, object] | None = None,
) -> dict[str, object]:
    binary = Path(binary).resolve()
    binary_hash = binary_sha256(binary)
    if binary_hash is None:
        raise FileNotFoundError(f"cannot fingerprint missing binary: {binary}")
    normalized_kernel = _normalized_kernel(mode, kernel)
    source_hash, sources = source_sha256(mode, normalized_kernel)
    try:
        recorded_binary = str(binary.relative_to(REPO))
    except ValueError:
        recorded_binary = str(binary)
    stamp = {
        "format": STAMP_FORMAT,
        "mode": mode,
        "kernel": normalized_kernel,
        "binary": recorded_binary,
        "binary_sha256": binary_hash,
        "source_sha256": source_hash,
        "sources": sources,
        "build_configuration": configuration or build_configuration(),
        "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    }
    output = stamp_path(binary)
    temporary = output.with_name(f".{output.name}.{os.getpid()}.tmp")
    temporary.write_text(json.dumps(stamp, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, output)
    return stamp


def verify_stamp(
    mode: str,
    kernel: str,
    binary: Path,
    configuration: dict[str, object] | None = None,
) -> dict[str, object]:
    binary = Path(binary).resolve()
    normalized_kernel = _normalized_kernel(mode, kernel)
    output = stamp_path(binary)
    reasons: list[str] = []
    try:
        stamp = json.loads(output.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        return {
            "valid": False,
            "status": "missing_or_invalid",
            "stamp": str(output),
            "reasons": [f"cannot read build stamp: {error}"],
        }
    current_binary_hash = binary_sha256(binary)
    current_source_hash, sources = source_sha256(mode, normalized_kernel)
    current_configuration = configuration or build_configuration()
    expected = {
        "format": STAMP_FORMAT,
        "mode": mode,
        "kernel": normalized_kernel,
        "binary_sha256": current_binary_hash,
        "source_sha256": current_source_hash,
        "build_configuration": current_configuration,
    }
    for field, value in expected.items():
        if stamp.get(field) != value:
            reasons.append(
                f"{field} mismatch: stamped {stamp.get(field)!r}, current {value!r}"
            )
    return {
        "valid": not reasons,
        "status": "current" if not reasons else "stale",
        "stamp": str(output),
        "binary_sha256": current_binary_hash,
        "source_sha256": current_source_hash,
        "sources": sources,
        "build_configuration": current_configuration,
        "reasons": reasons,
    }


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("stamp", "verify"))
    parser.add_argument("--mode", choices=("cpp_dpi", "pure_sv"), required=True)
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--opt-fast")
    parser.add_argument("--converge-limit")
    parser.add_argument("--extra-cflags")
    parser.add_argument("--extra-ldflags")
    parser.add_argument("--cxx")
    parser.add_argument("--cxxflags")
    parser.add_argument("--cppflags")
    parser.add_argument("--ldflags")
    parser.add_argument("--verilator")
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    configuration = build_configuration(
        opt_fast=args.opt_fast,
        converge_limit=args.converge_limit,
        extra_cflags=args.extra_cflags,
        extra_ldflags=args.extra_ldflags,
        cxx=args.cxx,
        cxxflags=args.cxxflags,
        cppflags=args.cppflags,
        ldflags=args.ldflags,
        verilator=args.verilator,
    )
    result = (
        write_stamp(args.mode, args.kernel, args.binary, configuration)
        if args.command == "stamp"
        else verify_stamp(args.mode, args.kernel, args.binary, configuration)
    )
    print(json.dumps(result, indent=2))
    return 0 if args.command == "stamp" or result["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
