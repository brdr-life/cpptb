#!/usr/bin/env python3
"""Record the exact isolated Verilator invocation and resulting binary hash."""

import argparse
import datetime
import hashlib
import json
import shlex
import subprocess
from pathlib import Path


REPO = Path(__file__).resolve().parents[3]
DEFAULT_OBJECT_DIR = REPO / "build" / "diagnostics" / "runtime_old_obj"
DEFAULT_BINARY = DEFAULT_OBJECT_DIR / "Vdpi_peripheral_suite"
DEFAULT_OUTPUT = Path(__file__).resolve().parent / "build_provenance.json"


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_output(command):
    completed = subprocess.run(
        command,
        cwd=REPO,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=True,
    )
    return completed.stdout.strip()


def verilator_command(object_dir):
    record = object_dir / "Vdpi_peripheral_suite__verFiles.dat"
    for line in record.read_text(encoding="utf-8").splitlines():
        if line.startswith('C "') and line.endswith('"'):
            return ["verilator", *shlex.split(line[3:-1])]
    raise RuntimeError(f"missing Verilator command in {record}")


def make_record(binary=DEFAULT_BINARY, object_dir=DEFAULT_OBJECT_DIR):
    compiler_version = command_output(["c++", "--version"])
    verilator = verilator_command(object_dir)
    dry_build = command_output(
        [
            "make",
            "-n",
            "-B",
            "-C",
            str(object_dir),
            "-f",
            "Vdpi_peripheral_suite.mk",
            "Vdpi_peripheral_suite",
        ]
    )
    compiler_commands = [
        shlex.split(line)
        for line in dry_build.splitlines()
        if line.startswith("c++ ")
    ]
    if not compiler_commands:
        raise RuntimeError("generated Makefile did not report compiler commands")
    source_provenance = Path(__file__).resolve().parent / "provenance.json"
    inputs = {
        REPO / token
        for token in verilator[1:]
        if token.endswith((".cpp", ".sv")) and (REPO / token).is_file()
    }
    inputs.update(
        {
            REPO / "cpptb" / "dpi_runtime.hpp",
            REPO / "cpptb" / "test_result.hpp",
            REPO
            / "benchmarks/peripheral_suite/cpp_dpi/generated/dpi_peripheral_suite.sv",
            REPO
            / "benchmarks/peripheral_suite/cpp_dpi/generated/peripheral_suite_dut.hpp",
            REPO
            / "benchmarks/peripheral_suite/cpp_dpi/generated/peripheral_suite_binding.hpp",
            source_provenance,
        }
    )
    return {
        "schema_version": 1,
        "recorded_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "git_commit": command_output(["git", "rev-parse", "HEAD"]),
        "build_command": [
            "make",
            "peripheral-suite-runtime-old-diagnostic-build",
        ],
        "verilator_command": verilator,
        "compiler": {
            "command": ["c++", "--version"],
            "version": compiler_version,
            "build_commands": compiler_commands,
        },
        "verilator_version": command_output(["verilator", "--version"]),
        "binary": str(binary.relative_to(REPO)),
        "binary_size_bytes": binary.stat().st_size,
        "binary_sha256": sha256(binary),
        "input_sha256": {
            str(path.relative_to(REPO)): sha256(path) for path in sorted(inputs)
        },
    }


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--object-dir", type=Path, default=DEFAULT_OBJECT_DIR)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args(argv)
    record = make_record(args.binary.resolve(), args.object_dir.resolve())
    args.output.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded {record['binary_sha256']} in {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
