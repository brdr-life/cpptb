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
    python3 run_suite.py --config bmfull    # bitmanip and PMP
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

from build_tests import CONFIGS, DEFAULT_CONFIG

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

# Which binaries to compare is recorded in the manifest by build_tests.py, so
# the two never disagree about which Ibex configuration is under test.

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
# The checker prints this and then $fatals, so it is the authoritative signal
# that the DUT and the reference model parted company.
MISMATCH_RE = re.compile(r"Co-simulation mismatch at time\s*(\S+)")
MISMATCH_DETAIL_RE = re.compile(r"^(?!FAILURE)(.*(?:ISS|DUT|expected).*)$", re.M)


# An architectural test runs a few hundred thousand cycles, well under a second
# on either harness. Anything past this is not slow, it is stuck, and the wait
# is pure cost: at the old 300s a hung test held the suite for five minutes per
# side. The cpptb testbench stops itself at kCycleLimit and says which test it
# was; the upstream harness has no cycle limit at all, so only this catches it
# there.
RUN_TIMEOUT_S = 30


def run_once(mode: str, test: dict, workdir: Path, binaries: dict) -> dict:
    log = workdir / LOG_NAME
    log.unlink(missing_ok=True)

    environment = None
    if mode == "cpptb":
        command = [str(binaries["cpptb"]), TRACER_OFF]
        environment = dict(os.environ)
        environment.update({
            "ACT_NAME": test["name"],
            "ACT_FIRMWARE": str(HERE / test["vmem"]),
            "ACT_SIG_BEGIN": str(test["sig_begin"]),
            "ACT_SIG_END": str(test["sig_end"]),
        })
    else:
        command = [str(binaries["upstream"]), TRACER_OFF,
                   f"--meminit=ram,{HERE / test['elf']}"]

    started = time.perf_counter()
    completed = subprocess.run(command, cwd=workdir, text=True,
                               env=environment, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=False,
                               timeout=RUN_TIMEOUT_S)
    wall_ms = (time.perf_counter() - started) * 1000.0

    text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
    sample = {"mode": mode, "wall_ms": wall_ms,
              "returncode": completed.returncode,
              "sig": None, "words": None, "result": None, "backdoor": None,
              "cycles": None, "mismatch": None}

    if MISMATCH_RE.search(completed.stdout):
        detail = MISMATCH_DETAIL_RE.search(completed.stdout)
        sample["mismatch"] = (detail.group(1).strip() if detail
                              else "reported, no detail captured")

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


def check_spike(cpptb: dict) -> list[str]:
    """What must hold when the reference is Spike rather than a second harness.

    There is nothing to compare signatures against here, and nothing needs to
    be: the checker has already compared every retired instruction and every
    data memory access against Spike as the program ran, and stops the
    simulation the moment they disagree. So the test passes if the program
    reached its own end and no mismatch was reported.
    """
    problems = []
    if cpptb["mismatch"]:
        problems.append(f"co-simulation mismatch: {cpptb['mismatch']}")
    if cpptb["result"] != "PASS":
        problems.append(f"reported {cpptb['result'] or 'nothing'}")
    if cpptb["backdoor"] is not None and cpptb["sig"] is not None:
        if cpptb["backdoor"] != cpptb["sig"]:
            problems.append(
                f"backdoor read {cpptb['backdoor']} but the program "
                f"reported {cpptb['sig']}")
    return problems


def outcome(sample: dict) -> str:
    """One word for how a run ended, comparable across the two harnesses.

    They detect a program that never finishes differently, and the difference is
    not a disagreement about the program. The cpptb testbench stops at its own
    cycle limit and exits without printing a result; the upstream harness has no
    cycle limit and runs until run_suite kills it. Both mean the same thing, so
    both normalise to the same word -- otherwise every hung test is reported as
    the harnesses contradicting each other.
    """
    if sample["result"] in (None, "TIMEOUT"):
        return "did not complete"
    return sample["result"]


def check(test: dict, upstream: dict, cpptb: dict) -> tuple[list[str], list[str]]:
    """Split what went wrong into two kinds, which mean different things.

    A *disagreement* is the two harnesses reporting different things about the
    same run. That is what this port is being judged on, and any of them is a
    defect in the port.

    A *shared failure* is both harnesses agreeing that the test did not pass.
    That says something about the core or about the test, and nothing about
    cpptb. Counting the two together would let a port that faithfully reproduces
    a core's behaviour look broken, and would hide a real divergence among tests
    that were failing anyway.
    """
    disagreements, shared = [], []

    up_outcome, cp_outcome = outcome(upstream), outcome(cpptb)
    if up_outcome != cp_outcome:
        disagreements.append(
            f"outcomes differ: upstream {up_outcome} vs cpptb {cp_outcome}")
    elif up_outcome != "PASS":
        shared.append(f"both {up_outcome}")

    if up_outcome == cp_outcome != "PASS":
        return disagreements, shared

    if upstream["sig"] != cpptb["sig"]:
        disagreements.append(
            f"signatures differ: upstream {upstream['sig']} "
            f"vs cpptb {cpptb['sig']}")
    elif upstream["words"] != cpptb["words"]:
        disagreements.append(
            f"signature lengths differ: upstream {upstream['words']} "
            f"vs cpptb {cpptb['words']}")

    # The backdoor digest is computed by the host reading the array; the other
    # by the core executing loads. Disagreeing means the backdoor is not seeing
    # the memory the core used, which would make the loader suspect.
    if cpptb["backdoor"] is not None and cpptb["sig"] is not None:
        if cpptb["backdoor"] != cpptb["sig"]:
            disagreements.append(
                f"cpptb backdoor read {cpptb['backdoor']} but the program "
                f"reported {cpptb['sig']}")
    return disagreements, shared


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--filter", default="",
                        help="only tests whose name contains this")
    parser.add_argument("--json", type=Path)
    parser.add_argument("--verbose", action="store_true",
                        help="print a line per test rather than per group")
    parser.add_argument("--config", default=DEFAULT_CONFIG, choices=sorted(CONFIGS),
                        help=f"Ibex configuration to run (default {DEFAULT_CONFIG})")
    args = parser.parse_args(argv)

    manifest_path = HERE / "build" / args.config / "manifest.json"
    if not manifest_path.is_file():
        print(f"run_suite: no manifest at {manifest_path}\n"
              f"run: python3 {HERE / 'build_tests.py'} --config {args.config}",
              file=sys.stderr)
        return 1

    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    binaries = {"upstream": ROOT / manifest["upstream_binary"],
                "cpptb": ROOT / manifest["cpptb_binary"]}
    if manifest.get("reference") == "spike":
        binaries.pop("upstream")  # nothing to compare against; Spike is inline
    for name, binary in binaries.items():
        if not binary.is_file():
            print(f"run_suite: {name} binary missing: {binary}", file=sys.stderr)
            return 1
    tests = manifest["tests"]
    if args.filter:
        tests = [t for t in tests if args.filter in t["name"]]
    if not tests:
        print("run_suite: no tests selected", file=sys.stderr)
        return 1

    work = HERE / "build" / args.config / "run"
    shutil.rmtree(work, ignore_errors=True)
    dirs = {mode: work / mode for mode in ("upstream", "cpptb")}
    for path in dirs.values():
        path.mkdir(parents=True)

    print(f"config {manifest['config']} (Ibex {manifest['ibex_config']}): "
          f"running {len(tests)} test(s) on both harnesses\n")
    results = []
    suite_started = time.perf_counter()
    for index, test in enumerate(tests):
        # Alternate which harness goes first so ordering does not favour one.
        order = (("upstream", "cpptb") if index % 2 == 0
                 else ("cpptb", "upstream"))
        samples = {}
        if manifest.get("reference") == "spike":
            order = ("cpptb",)
        for mode in order:
            try:
                samples[mode] = run_once(mode, test, dirs[mode], binaries)
            except subprocess.TimeoutExpired:
                samples[mode] = {"mode": mode, "wall_ms": float("nan"),
                                 "returncode": -1, "sig": None, "words": None,
                                 "result": "TIMEOUT", "backdoor": None,
                                 "cycles": None, "mismatch": None}

        if manifest.get("reference") == "spike":
            problems, shared = check_spike(samples["cpptb"]), []
        else:
            problems, shared = check(test, samples["upstream"],
                                     samples["cpptb"])
        record = {"name": test["name"], "group": test["group"],
                  "problems": problems, "shared": shared,
                  "cpptb": samples["cpptb"],
                  **({"upstream": samples["upstream"]} if "upstream" in samples
                     else {})}
        results.append(record)

        if args.verbose or problems or shared:
            status = ("ok" if not (problems or shared)
                      else "; ".join(problems + shared))
            # Always upstream then cpptb, never `order`: that alternates per
            # test so the columns would swap on every other line.
            times = "".join(f"{samples[m]['wall_ms']:8.1f} ms"
                            for m in ("upstream", "cpptb") if m in samples)
            print(f"  {test['name']:<28}{times}  {status}")
    suite_wall = time.perf_counter() - suite_started

    failed = [r for r in results if r["problems"]]
    shared_failures = [r for r in results if r["shared"] and not r["problems"]]
    passed = len(results) - len(failed) - len(shared_failures)

    by_group: dict[str, list] = {}
    for record in results:
        by_group.setdefault(record["group"], []).append(record)

    spike = manifest.get("reference") == "spike"
    # Only tests that ran to completion on both sides carry timing. A run that
    # hit the timeout contributes RUN_TIMEOUT_S, which measures the timeout.
    timed = [r for r in results if not (r["problems"] or r["shared"])]
    cp_total = sum(r["cpptb"]["wall_ms"] for r in timed)
    cp_each = [r["cpptb"]["wall_ms"] for r in timed]
    up_total = 0.0 if spike else sum(r["upstream"]["wall_ms"] for r in timed)

    if spike:
        print(f"\n{'group':<16} {'tests':>5} {'cpptb+spike':>13}")
        for group, records in sorted(by_group.items()):
            ok = [r for r in records if r in timed]
            cp = sum(r["cpptb"]["wall_ms"] for r in ok)
            print(f"{group:<16} {len(records):>5} {cp:>12.0f}ms")
        print(f"\n{'total':<16} {len(timed):>5} {cp_total:>12.0f}ms")
        if cp_each:
            print(f"median per test   {statistics.median(cp_each):.1f} ms")
        print(f"suite wall        {suite_wall:.1f} s "
              f"(every instruction checked against Spike)")
    else:
        print(f"\n{'group':<16} {'tests':>5} {'upstream':>10} {'cpptb':>10} {'ratio':>7}")
        for group, records in sorted(by_group.items()):
            ok = [r for r in records if r in timed]
            up = sum(r["upstream"]["wall_ms"] for r in ok)
            cp = sum(r["cpptb"]["wall_ms"] for r in ok)
            print(f"{group:<16} {len(records):>5} {up:>9.0f}ms {cp:>9.0f}ms "
                  f"{cp / up if up else 0:>6.3f}x")
        up_each = [r["upstream"]["wall_ms"] for r in timed]
        print(f"\n{'total':<16} {len(timed):>5} {up_total:>9.0f}ms "
              f"{cp_total:>9.0f}ms {cp_total / up_total if up_total else 0:>6.3f}x")
        if timed:
            print(f"median per test   upstream "
                  f"{statistics.median(up_each):.1f} ms   "
                  f"cpptb {statistics.median(cp_each):.1f} ms")
        print(f"suite wall        {suite_wall:.1f} s (both harnesses, serial)")
    print(f"\n{passed}/{len(results)} tests passed")
    if shared_failures:
        # Both harnesses said the same thing, so this is about the core or the
        # test, not about the port. Named so it can be followed up.
        print(f"{len(shared_failures)} did not pass on this core, both "
              f"harnesses agreeing:")
        groups: dict[str, list[str]] = {}
        for record in shared_failures:
            groups.setdefault(record["shared"][0], []).append(record["name"])
        for reason, names in sorted(groups.items()):
            shown = ", ".join(sorted(names)[:4])
            more = f" and {len(names) - 4} more" if len(names) > 4 else ""
            print(f"  {len(names):>3}  {reason}: {shown}{more}")
    if failed:
        print(f"{len(failed)} disagreed between the harnesses; that is a port "
              f"defect and the comparison is not valid until they do not",
              file=sys.stderr)
    elif spike:
        checked = sum(1 for r in results if r["cpptb"]["result"] == "PASS")
        print(f"every test: {checked} programs ran to completion with Spike in "
              "lockstep and no instruction or memory access disagreed")
    elif not shared_failures:
        print("every test: both harnesses produced the same signature, and "
              "cpptb's backdoor read agreed with what the program reported")
    else:
        print(f"the {passed} that did pass: both harnesses produced the same "
              "signature, and cpptb's backdoor read agreed with the program; "
              "no test had the harnesses disagree")

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({
            "config": manifest["config"],
            "ibex_config": manifest["ibex_config"],
            "tests": len(results), "passed": passed,
            "shared_failures": len(shared_failures),
            "timed": len(timed),
            "upstream_total_ms": up_total, "cpptb_total_ms": cp_total,
            "ratio_cpptb_over_upstream": cp_total / up_total if up_total else None,
            "suite_wall_s": suite_wall,
            "results": results,
        }, indent=2), encoding="utf-8")
        print(f"wrote {args.json}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
