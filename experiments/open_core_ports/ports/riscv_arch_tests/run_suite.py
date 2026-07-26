#!/usr/bin/env python3
"""Run the architectural tests under both harnesses and compare them.

Each test is one program run twice against the same Ibex RTL: once under Ibex's
own Verilator harness, once under the cpptb port. Three pieces of evidence come
back from each run and all three have to line up.

  - the signature digest the program prints on its way out, which both
    harnesses forward from the Simple System character register to their log
  - the digest cpptb computes independently by reading the same memory through
    the backdoor, which agrees only if the backdoor sees what the core saw
  - the number of words digested, so a region of the wrong length is
    distinguishable from one of the wrong content

A test counts as passing only if the two harnesses agree, and a disagreement
invalidates the comparison rather than being reported as a difference in speed.

This measures a different thing from ports/ibex_simple_system, which runs one
40-million-cycle program and reports steady-state throughput. Here there are 96
programs of a few hundred thousand cycles each, so what dominates is per-run
cost: process start, model construction, memory load, reset.

    python3 run_suite.py
    python3 run_suite.py --filter I-        # a subset
    python3 run_suite.py --json out.json

Standard library only, matching the other tools here.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

UPSTREAM = (ROOT / "deps/ibex/build/lowrisc_ibex_ibex_simple_system_0"
            / "sim-verilator/Vibex_simple_system")
CPPTB = (ROOT / "work/riscv_arch_tests/cpptb/riscv_arch_tests"
         / "obj/Vdpi_riscv_arch_tests")
MANIFEST = HERE / "build" / "manifest.json"

# Ibex's tracer writes every retired instruction to disk, which costs more than
# the simulation. Both sides must agree about it or the comparison is mostly
# measuring file I/O.
TRACER_OFF = "+ibex_tracer_enable=0"

# simulator_ctrl.sv opens this in the process's working directory, so each side
# runs somewhere of its own and the file is removed between tests.
LOG_NAME = "ibex_simple_system.log"

SIG_RE = re.compile(r"ACT-SIG ([0-9a-f]{8}) ([0-9a-f]{8})")
RESULT_RE = re.compile(r"ACT-RESULT (\S+)")
BACKDOOR_RE = re.compile(r"backdoor-sig ([0-9a-f]{8}) ([0-9a-f]{8})")
CYCLES_RE = re.compile(r"sim_cycles=(\d+)|Executed cycles:\s*(\d+)")


def run_once(mode: str, test: dict, workdir: Path) -> dict:
    log = workdir / LOG_NAME
    log.unlink(missing_ok=True)

    environment = None
    if mode == "cpptb":
        command = [str(CPPTB), TRACER_OFF]
        environment = dict(os.environ)
        environment.update({
            "ACT_NAME": test["name"],
            "ACT_FIRMWARE": str(HERE / test["vmem"]),
            "ACT_SIG_BEGIN": str(test["sig_begin"]),
            "ACT_SIG_END": str(test["sig_end"]),
        })
    else:
        command = [str(UPSTREAM), TRACER_OFF,
                   f"--meminit=ram,{HERE / test['elf']}"]

    started = time.perf_counter()
    completed = subprocess.run(command, cwd=workdir, text=True,
                               env=environment, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=False,
                               timeout=300)
    wall_ms = (time.perf_counter() - started) * 1000.0

    text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
    sample = {"mode": mode, "wall_ms": wall_ms,
              "returncode": completed.returncode,
              "sig": None, "words": None, "result": None, "backdoor": None,
              "cycles": None}

    match = SIG_RE.search(text)
    if match:
        sample["sig"], sample["words"] = match.group(1), match.group(2)
    match = RESULT_RE.search(text)
    if match:
        sample["result"] = match.group(1)
    match = BACKDOOR_RE.search(completed.stdout)
    if match:
        sample["backdoor"] = match.group(1)
    match = CYCLES_RE.search(completed.stdout)
    if match:
        sample["cycles"] = int(match.group(1) or match.group(2))
    return sample


def check(test: dict, upstream: dict, cpptb: dict) -> list[str]:
    """Everything that must hold for this test to count as passing."""
    problems = []
    for name, sample in (("upstream", upstream), ("cpptb", cpptb)):
        if sample["result"] != "PASS":
            problems.append(f"{name} reported {sample['result'] or 'nothing'}")
        if sample["sig"] is None:
            problems.append(f"{name} printed no signature")
    if upstream["sig"] and cpptb["sig"]:
        if upstream["sig"] != cpptb["sig"]:
            problems.append(
                f"signatures differ: upstream {upstream['sig']} "
                f"vs cpptb {cpptb['sig']}")
        if upstream["words"] != cpptb["words"]:
            problems.append(
                f"signature lengths differ: upstream {upstream['words']} "
                f"vs cpptb {cpptb['words']}")
    # The backdoor digest is computed by the host reading the array; the other
    # by the core executing loads. They disagreeing means the backdoor is not
    # seeing the memory the core used, which would make the loader suspect.
    if cpptb["backdoor"] is not None and cpptb["sig"] is not None:
        if cpptb["backdoor"] != cpptb["sig"]:
            problems.append(
                f"cpptb backdoor read {cpptb['backdoor']} but the program "
                f"reported {cpptb['sig']}")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--filter", default="",
                        help="only tests whose name contains this")
    parser.add_argument("--json", type=Path)
    parser.add_argument("--verbose", action="store_true",
                        help="print a line per test rather than per group")
    args = parser.parse_args(argv)

    if not MANIFEST.is_file():
        print(f"run_suite: no manifest at {MANIFEST}\n"
              f"run: python3 {HERE / 'build_tests.py'}", file=sys.stderr)
        return 1
    for name, binary in (("upstream", UPSTREAM), ("cpptb", CPPTB)):
        if not binary.is_file():
            print(f"run_suite: {name} binary missing: {binary}", file=sys.stderr)
            return 1

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    tests = manifest["tests"]
    if args.filter:
        tests = [t for t in tests if args.filter in t["name"]]
    if not tests:
        print("run_suite: no tests selected", file=sys.stderr)
        return 1

    work = HERE / "build" / "run"
    shutil.rmtree(work, ignore_errors=True)
    dirs = {mode: work / mode for mode in ("upstream", "cpptb")}
    for path in dirs.values():
        path.mkdir(parents=True)

    print(f"running {len(tests)} test(s) on both harnesses\n")
    results = []
    suite_started = time.perf_counter()
    for index, test in enumerate(tests):
        # Alternate which harness goes first so ordering does not favour one.
        order = (("upstream", "cpptb") if index % 2 == 0
                 else ("cpptb", "upstream"))
        samples = {}
        for mode in order:
            try:
                samples[mode] = run_once(mode, test, dirs[mode])
            except subprocess.TimeoutExpired:
                samples[mode] = {"mode": mode, "wall_ms": float("nan"),
                                 "returncode": -1, "sig": None, "words": None,
                                 "result": "TIMEOUT", "backdoor": None,
                                 "cycles": None}

        problems = check(test, samples["upstream"], samples["cpptb"])
        record = {"name": test["name"], "group": test["group"],
                  "problems": problems,
                  "upstream": samples["upstream"], "cpptb": samples["cpptb"]}
        results.append(record)

        if args.verbose or problems:
            status = "ok" if not problems else "; ".join(problems)
            print(f"  {test['name']:<28} {samples['upstream']['wall_ms']:7.1f} ms  "
                  f"{samples['cpptb']['wall_ms']:7.1f} ms  {status}")
    suite_wall = time.perf_counter() - suite_started

    failed = [r for r in results if r["problems"]]
    passed = len(results) - len(failed)

    by_group: dict[str, list] = {}
    for record in results:
        by_group.setdefault(record["group"], []).append(record)

    print(f"\n{'group':<16} {'tests':>5} {'upstream':>10} {'cpptb':>10} {'ratio':>7}")
    for group, records in sorted(by_group.items()):
        up = sum(r["upstream"]["wall_ms"] for r in records)
        cp = sum(r["cpptb"]["wall_ms"] for r in records)
        print(f"{group:<16} {len(records):>5} {up:>9.0f}ms {cp:>9.0f}ms "
              f"{cp / up if up else 0:>6.3f}x")

    up_total = sum(r["upstream"]["wall_ms"] for r in results)
    cp_total = sum(r["cpptb"]["wall_ms"] for r in results)
    up_each = [r["upstream"]["wall_ms"] for r in results]
    cp_each = [r["cpptb"]["wall_ms"] for r in results]

    print(f"\n{'total':<16} {len(results):>5} {up_total:>9.0f}ms "
          f"{cp_total:>9.0f}ms {cp_total / up_total if up_total else 0:>6.3f}x")
    print(f"median per test   upstream {statistics.median(up_each):.1f} ms   "
          f"cpptb {statistics.median(cp_each):.1f} ms")
    print(f"suite wall        {suite_wall:.1f} s (both harnesses, serial)")
    print(f"\n{passed}/{len(results)} tests passed")
    if failed:
        print(f"{len(failed)} failed; the comparison is not valid until they do",
              file=sys.stderr)
    else:
        print("every test: both harnesses produced the same signature, and "
              "cpptb's backdoor read agreed with what the program reported")

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({
            "tests": len(results), "passed": passed,
            "upstream_total_ms": up_total, "cpptb_total_ms": cp_total,
            "ratio_cpptb_over_upstream": cp_total / up_total if up_total else None,
            "suite_wall_s": suite_wall,
            "results": results,
        }, indent=2), encoding="utf-8")
        print(f"wrote {args.json}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
