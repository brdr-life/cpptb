#!/usr/bin/env python3
"""Verify that C++ and authored-SystemVerilog logs share one trace."""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path


def run(binary: Path, *, level: str | None = None) -> subprocess.CompletedProcess[str]:
    environment = {**os.environ, "CPPTB_TEST": "mixed_language_logging"}
    if level is not None:
        environment["CPPTB_LOG_LEVEL"] = level
    return subprocess.run(
        [str(binary)],
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def message_factory_count(completed: subprocess.CompletedProcess[str]) -> int | None:
    output = completed.stdout + completed.stderr
    match = re.search(r"CPPTB_MIXED_LOGGING_SV_MESSAGE_FACTORIES=(\d+)", output)
    return int(match.group(1)) if match else None


def record_time(line: str) -> int | None:
    match = re.search(r": (\d+) fs #\d+:", line)
    return int(match.group(1)) if match else None


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_output.py SIMULATOR", file=sys.stderr)
        return 2

    binary = Path(sys.argv[1])
    completed = run(binary)
    if completed.returncode != 0:
        print(completed.stdout, end="", file=sys.stderr)
        print(completed.stderr, end="", file=sys.stderr)
        return completed.returncode

    records = [
        line for line in completed.stderr.splitlines() if line.startswith("cpptb:")
    ]
    sequences = [
        int(match.group(1))
        for line in records
        if (match := re.search(r" #(\d+):", line))
    ]
    sv_records = [line for line in records if " [sv " in line]
    sv_times = [record_time(line) for line in sv_records]
    checks = {
        "five ordered records": sequences == [1, 2, 3, 4, 5],
        "two SystemVerilog records": len(sv_records) == 2,
        "exact SystemVerilog timestamps": sv_times == [25_000_000, 35_000_000],
        "SV source location": all("mixed_logging.sv:" in line for line in sv_records),
        "SV hierarchy": all(".i_dut]" in line for line in sv_records),
        "SV scope": all("[request_monitor]" in line for line in sv_records),
        "C++ source location": sum("testbench.cpp:" in line for line in records) == 3,
        "enabled SV message factories": message_factory_count(completed) == 2,
    }
    failed = [label for label, passed in checks.items() if not passed]
    if failed:
        print("mixed logging output check failed: " + ", ".join(failed), file=sys.stderr)
        print(completed.stderr, file=sys.stderr)
        return 1

    disabled = run(binary, level="off")
    if disabled.returncode != 0:
        print(disabled.stdout, end="", file=sys.stderr)
        print(disabled.stderr, end="", file=sys.stderr)
        return disabled.returncode

    disabled_records = [
        line
        for line in disabled.stderr.splitlines()
        if line.startswith("cpptb:")
    ]
    disabled_checks = {
        "off emits no structured records": not disabled_records,
        "off reaches no SV logging call": message_factory_count(disabled) == 0,
    }
    disabled_failed = [
        label for label, passed in disabled_checks.items() if not passed
    ]
    if disabled_failed:
        print(
            "disabled mixed logging check failed: "
            + ", ".join(disabled_failed),
            file=sys.stderr,
        )
        print(disabled.stdout, file=sys.stderr)
        print(disabled.stderr, file=sys.stderr)
        return 1

    print(
        "MIXED_LOGGING_OUTPUT_PASS records=5 sv_records=2 "
        "sv_timestamps=exact off_sv_calls=0"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
