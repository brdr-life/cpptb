#!/usr/bin/env python3
"""Run the core_ibex UVM tests and report what each one does.

The test list is upstream's own `riscv_dv_extension/testlist.yaml`: 57 entries
naming a riscv-dv generator test and the UVM test class that runs it. This
takes the distinct UVM classes and runs each against a generated program,
reporting the outcome rather than a pass/fail it cannot yet honestly give.

    python3 run_tests.py --list
    python3 run_tests.py --timeout-cycles 200000

The upstream flow generates a *different* program per entry, with the generator
options from the same YAML. This does not: it runs every class against one
program, which is enough to say which classes start, which reach the core, and
where each stops. Turning that into a regression means generating per-entry,
which needs riscv-dv's Ibex extension rather than pyflow. See README.md.

Standard library only, matching the other tools here.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TESTLIST = (ROOT / "deps/ibex/dv/uvm/core_ibex/riscv_dv_extension/testlist.yaml")
BUILD = HERE / "build"
BINARY = BUILD / "obj" / "core_ibex_tb"
SPIKE_LIB = ROOT / "deps" / "spike_cosim" / "install" / "lib"
TOOLS_LIB = ROOT / "deps" / ".tools" / "root" / "usr" / "lib" / "x86_64-linux-gnu"
TOOLS_BIN = ROOT / "deps" / ".tools" / "root" / "usr" / "bin"

SIGNATURE_ADDR = "8ffffffc"

# What the run said about itself, most specific first. The report server prints
# the first two; the rest are how a run that never got there ends.
OUTCOMES = [
    (re.compile(r"--- RISC-V UVM TEST PASSED ---"), "passed"),
    (re.compile(r"Cosim mismatch (.+)"), "cosim mismatch"),
    (re.compile(r"double_fault detector"), "double faults"),
    (re.compile(r"Randomization failed"), "randomize failed"),
    (re.compile(r"--- RISC-V UVM TEST FAILED ---"), "failed"),
]


def entries() -> list[tuple[str, str]]:
    """(riscv-dv test, UVM test class) from upstream's testlist."""
    if not TESTLIST.is_file():
        raise SystemExit(f"run_tests: no testlist at {TESTLIST}")
    pairs, test = [], None
    for line in TESTLIST.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        if stripped.startswith("- test:"):
            test = stripped.split(":", 1)[1].strip()
        elif stripped.startswith("rtl_test:") and test:
            pairs.append((test, stripped.split(":", 1)[1].strip()))
            test = None
    return pairs


def environment() -> dict[str, str]:
    import os

    env = dict(os.environ)
    # z3 for Verilator's constraint solver, and Spike's shared libraries.
    env["PATH"] = f"{TOOLS_BIN}:{env.get('PATH', '')}"
    env["LD_LIBRARY_PATH"] = ":".join(
        [str(TOOLS_LIB), str(SPIKE_LIB), env.get("LD_LIBRARY_PATH", "")])
    return env


def run_one(rtl_test: str, program: Path, cycles: int, seconds: int,
            extra: list[str]) -> dict:
    command = [str(BINARY), f"+UVM_TESTNAME={rtl_test}",
               f"+bin={program}", f"+signature_addr={SIGNATURE_ADDR}",
               f"+timeout_in_cycles={cycles}", *extra]
    try:
        completed = subprocess.run(command, cwd=BUILD, env=environment(),
                                   text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=seconds)
        output, timed_out = completed.stdout, False
    except subprocess.TimeoutExpired as expired:
        output = expired.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        timed_out = True

    log = BUILD / "logs" / f"{rtl_test}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(output, encoding="utf-8")

    if timed_out:
        return {"test": rtl_test, "outcome": "wall-clock timeout", "detail": ""}
    for pattern, name in OUTCOMES:
        found = pattern.search(output)
        if found:
            detail = found.group(1).strip()[:70] if found.groups() else ""
            return {"test": rtl_test, "outcome": name, "detail": detail}
    return {"test": rtl_test, "outcome": "no verdict", "detail": ""}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--list", action="store_true",
                        help="print the test list and stop")
    parser.add_argument("--program", type=Path,
                        default=BUILD / "elf" / "gen_0.bin")
    parser.add_argument("--timeout-cycles", type=int, default=200000)
    parser.add_argument("--timeout-seconds", type=int, default=180)
    parser.add_argument("--only", action="append", default=[],
                        help="run only these UVM test classes")
    parser.add_argument("extra", nargs="*", help="further plusargs")
    args = parser.parse_args(argv)

    pairs = entries()
    classes = sorted({rtl for _, rtl in pairs})
    if args.list:
        print(f"{len(pairs)} riscv-dv tests over {len(classes)} UVM classes\n")
        for test, rtl in pairs:
            print(f"  {test:<45} {rtl}")
        return 0

    if not BINARY.is_file():
        raise SystemExit(f"run_tests: no testbench at {BINARY}\n"
                         f"run: python3 {HERE / 'build_tb.py'}")
    if not args.program.is_file():
        raise SystemExit(f"run_tests: no program at {args.program}\n"
                         f"run: python3 {HERE / 'build_programs.py'}")

    wanted = [c for c in classes if not args.only or c in args.only]
    results = []
    for rtl_test in wanted:
        result = run_one(rtl_test, args.program.resolve(),
                         args.timeout_cycles, args.timeout_seconds, args.extra)
        results.append(result)
        detail = f"  {result['detail']}" if result["detail"] else ""
        print(f"  {rtl_test:<48} {result['outcome']}{detail}", flush=True)

    (BUILD / "results.json").write_text(json.dumps(results, indent=2),
                                        encoding="utf-8")
    tally: dict[str, int] = {}
    for result in results:
        tally[result["outcome"]] = tally.get(result["outcome"], 0) + 1
    print("\n" + ", ".join(f"{count} {name}"
                           for name, count in sorted(tally.items())))
    return 0 if tally.get("passed") == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
