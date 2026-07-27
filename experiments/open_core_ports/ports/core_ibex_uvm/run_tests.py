#!/usr/bin/env python3
"""Run the core_ibex UVM tests and report what each one does.

The test list is upstream's own `riscv_dv_extension/testlist.yaml`: 57 entries
naming a riscv-dv generator test, the UVM test class that runs it, and the
generator and simulation options for both.

    python3 build_programs.py --all-tests
    python3 run_tests.py +disable_fetch_enable_seq=1

Each entry is run against the program `build_programs.py` generated for that
entry, with that entry's own `sim_opts`, which is what upstream's flow does.
Entries with no program of their own are skipped and listed.

pyflow cannot honour every `gen_opts` -- it is a separate implementation of the
generator from the one upstream runs, and it reads none of Ibex's SystemVerilog
extension -- so `build_programs.py` records what it could not do and this
reports it next to the result. A test whose program does not match its entry is
marked, because its outcome does not mean what the entry's name says.

`--program` runs every distinct UVM class against one program instead, which is
what this did before per-entry generation existed and is still the quickest way
to see whether a change moved everything at once.

`+disable_fetch_enable_seq=1` is not a default here, but no test completes
without it on this port; see README.md.

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
    # The harness cycle budget, not the wall clock: `+timeout_in_cycles`. Worth
    # its own outcome because it is what a program that never signals the end
    # of the test looks like from here.
    (re.compile(r"TEST TIMEOUT!!"), "cycle timeout"),
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


def manifest_tests() -> list[dict]:
    """The per-entry programs build_programs.py has built, if any."""
    manifest = BUILD / "manifest.json"
    if not manifest.is_file():
        return []
    data = json.loads(manifest.read_text(encoding="utf-8"))
    return [test for test in data.get("tests", []) if test.get("bin")]


def environment() -> dict[str, str]:
    import os

    env = dict(os.environ)
    # z3 for Verilator's constraint solver, and Spike's shared libraries.
    env["PATH"] = f"{TOOLS_BIN}:{env.get('PATH', '')}"
    env["LD_LIBRARY_PATH"] = ":".join(
        [str(TOOLS_LIB), str(SPIKE_LIB), env.get("LD_LIBRARY_PATH", "")])
    return env


def run_one(rtl_test: str, program: Path, cycles: int, seconds: int,
            extra: list[str], log_name: str | None = None) -> dict:
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

    log = BUILD / "logs" / f"{log_name or rtl_test}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(output, encoding="utf-8")

    name = log_name or rtl_test
    if timed_out:
        return {"test": name, "outcome": "wall-clock timeout", "detail": ""}
    for pattern, outcome in OUTCOMES:
        found = pattern.search(output)
        if found:
            detail = found.group(1).strip()[:70] if found.groups() else ""
            return {"test": name, "outcome": outcome, "detail": detail}
    return {"test": name, "outcome": "no verdict", "detail": ""}


def report(results: list[dict]) -> int:
    (BUILD / "results.json").write_text(json.dumps(results, indent=2),
                                        encoding="utf-8")
    tally: dict[str, int] = {}
    for result in results:
        tally[result["outcome"]] = tally.get(result["outcome"], 0) + 1
    print("\n" + ", ".join(f"{count} {name}"
                           for name, count in sorted(tally.items())))

    mismatched = [r for r in results if r.get("unsupported")]
    if mismatched:
        print(f"\n{len(mismatched)} of {len(results)} ran a program that does "
              f"not match its testlist entry:")
        for result in mismatched:
            print(f"  {result['test']}")
            for note in result["unsupported"]:
                print(f"    {note}")
    return 0 if tally.get("passed") == len(results) else 1


def run_testlist(args) -> int:
    """One run per testlist entry, each with its own program and sim_opts."""
    tests = manifest_tests()
    if args.only:
        tests = [t for t in tests
                 if t["test"] in args.only or t["rtl_test"] in args.only]
    results = []
    for test in tests:
        program = (HERE / test["bin"]).resolve()
        if not program.is_file():
            print(f"  {test['test']:<44} no program at {program}",
                  file=sys.stderr)
            continue
        # timeout_s is the entry's own wall-clock budget where it has one.
        seconds = int(test["timeout_s"]) if test.get("timeout_s") \
            else args.timeout_seconds
        result = run_one(test["rtl_test"], program, args.timeout_cycles,
                         seconds, test.get("sim_opts", []) + args.extra,
                         log_name=test["test"])
        result["rtl_test"] = test["rtl_test"]
        result["unsupported"] = test.get("unsupported", [])
        results.append(result)
        detail = f"  {result['detail']}" if result["detail"] else ""
        mark = " *" if result["unsupported"] else ""
        print(f"  {test['test']:<44} {result['outcome']}{detail}{mark}",
              flush=True)

    built = {test["test"] for test in tests}
    missing = [test for test, _ in entries()
               if test not in built and not args.only]
    if missing:
        print(f"\n{len(missing)} entries have no program: "
              f"{', '.join(missing)}")
    return report(results)


def run_classes(args) -> int:
    """One run per distinct UVM class, all against the same program."""
    classes = sorted({rtl for _, rtl in entries()})
    if not args.program.is_file():
        raise SystemExit(f"run_tests: no program at {args.program}\n"
                         f"run: python3 {HERE / 'build_programs.py'}")
    results = []
    for rtl_test in classes:
        if args.only and rtl_test not in args.only:
            continue
        result = run_one(rtl_test, args.program.resolve(),
                         args.timeout_cycles, args.timeout_seconds, args.extra)
        results.append(result)
        detail = f"  {result['detail']}" if result["detail"] else ""
        print(f"  {rtl_test:<48} {result['outcome']}{detail}", flush=True)
    return report(results)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--list", action="store_true",
                        help="print the test list and stop")
    parser.add_argument("--program", type=Path,
                        help="run every UVM class against this one program "
                             "instead of the per-entry programs")
    parser.add_argument("--timeout-cycles", type=int, default=200000)
    parser.add_argument("--timeout-seconds", type=int, default=180)
    parser.add_argument("--only", action="append", default=[],
                        help="run only these testlist entries or UVM classes")
    parser.add_argument("extra", nargs="*", help="further plusargs")
    args = parser.parse_args(argv)

    pairs = entries()
    if args.list:
        classes = sorted({rtl for _, rtl in pairs})
        built = {test["test"] for test in manifest_tests()}
        print(f"{len(pairs)} riscv-dv tests over {len(classes)} UVM classes, "
              f"{len(built)} with a program\n")
        for test, rtl in pairs:
            print(f"  {test:<45} {rtl}"
                  f"{'' if test in built else '   (no program)'}")
        return 0

    if not BINARY.is_file():
        raise SystemExit(f"run_tests: no testbench at {BINARY}\n"
                         f"run: python3 {HERE / 'build_tb.py'}")
    if args.program:
        return run_classes(args)
    if not manifest_tests():
        raise SystemExit(
            f"run_tests: no per-entry programs in {BUILD / 'manifest.json'}\n"
            f"run: python3 {HERE / 'build_programs.py'} --all-tests\n"
            f"or pass --program to run every class against one program")
    return run_testlist(args)


if __name__ == "__main__":
    raise SystemExit(main())
