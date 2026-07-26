#!/usr/bin/env python3
"""Compare the cpptb port against Ibex's own Verilator harness.

Both run the same design at the same commit, the same CoreMark binary, and the
same Verilator, so the framework driving the simulation is what differs.

Runs alternate to spread drift across both sides, and every run is checked for
the evidence CoreMark produces about itself. A faster run that computed the
wrong answer is not a better run, so a mismatch invalidates the comparison
rather than being reported as a speedup.

    python3 compare.py --runs 5

Standard library only.
"""

from __future__ import annotations

import argparse
import json
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
IBEX = ROOT / "deps" / "ibex"

UPSTREAM = (IBEX / "build/lowrisc_ibex_ibex_simple_system_0"
            / "sim-verilator/Vibex_simple_system")
CPPTB = (ROOT / "work/ibex_simple_system/cpptb/ibex_simple_system"
         / "obj/Vdpi_ibex_simple_system")
COREMARK_ELF = IBEX / "examples/sw/benchmarks/coremark/coremark.elf"

# The tracer writes every retired instruction to disk. Left on it costs more
# wall time than the simulation, so both sides must agree about it.
TRACER_OFF = "+ibex_tracer_enable=0"

# What CoreMark says about its own run. Comparing these stops a wrong-but-fast
# result from looking like a win.
EXPECTED = {"ticks": 40637770, "iterations": 100, "score": "1.230382"}


def parse_log(log: Path) -> dict:
    if not log.is_file():
        return {}
    text = log.read_text(encoding="utf-8", errors="replace")
    found = {"validated": "Correct operation validated" in text}
    for key, pattern in (
        ("ticks", r"Total ticks\s*:\s*(\d+)"),
        ("iterations", r"Iterations\s*:\s*(\d+)"),
        ("score", r"CoreMark 1\.0 : ([\d.]+)"),
    ):
        match = re.search(pattern, text)
        if match:
            found[key] = (int(match.group(1)) if key != "score"
                          else match.group(1))
    return found


def run_once(mode: str) -> dict:
    log = HERE / "ibex_simple_system.log" if mode == "cpptb" else IBEX / "ibex_simple_system.log"
    log.unlink(missing_ok=True)
    cwd = HERE if mode == "cpptb" else IBEX

    if mode == "cpptb":
        command = [str(CPPTB), TRACER_OFF]
    else:
        command = [str(UPSTREAM), TRACER_OFF, f"--meminit=ram,{COREMARK_ELF}"]

    started = time.perf_counter()
    completed = subprocess.run(command, cwd=cwd, text=True,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=False)
    wall_ms = (time.perf_counter() - started) * 1000.0

    evidence = parse_log(log)
    sim_cycles = None
    match = re.search(r"sim_cycles=(\d+)", completed.stdout)
    if match:
        sim_cycles = int(match.group(1))
    else:
        match = re.search(r"Executed cycles:\s*(\d+)", completed.stdout)
        if match:
            sim_cycles = int(match.group(1))

    return {"mode": mode, "wall_ms": wall_ms, "sim_cycles": sim_cycles,
            "returncode": completed.returncode, **evidence}


def check(sample: dict) -> list[str]:
    problems = []
    if not sample.get("validated"):
        problems.append("CoreMark did not report a validated result")
    for key, expected in EXPECTED.items():
        actual = sample.get(key)
        if actual != expected:
            problems.append(f"{key}: {actual!r} != {expected!r}")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args(argv)

    for name, binary in (("upstream", UPSTREAM), ("cpptb", CPPTB)):
        if not binary.is_file():
            print(f"compare: {name} binary missing: {binary}", file=sys.stderr)
            return 1

    samples: list[dict] = []
    for index in range(args.runs):
        # Alternate which side goes first so ordering does not favour one.
        order = ("upstream", "cpptb") if index % 2 == 0 else ("cpptb", "upstream")
        for mode in order:
            sample = run_once(mode)
            problems = check(sample)
            status = "ok" if not problems else "; ".join(problems)
            print(f"  {mode:8s} run {index + 1}: "
                  f"{sample['wall_ms']:8.1f} ms  cycles={sample['sim_cycles']}  {status}")
            sample["problems"] = problems
            samples.append(sample)

    invalid = [s for s in samples if s["problems"]]
    if invalid:
        print(f"\ncomparison invalid: {len(invalid)} run(s) failed their "
              "semantic check", file=sys.stderr)
        return 1

    result = {"runs": args.runs, "samples": samples, "modes": {}}
    for mode in ("upstream", "cpptb"):
        walls = [s["wall_ms"] for s in samples if s["mode"] == mode]
        result["modes"][mode] = {
            "median_wall_ms": statistics.median(walls),
            "min_wall_ms": min(walls),
            "max_wall_ms": max(walls),
        }

    up = result["modes"]["upstream"]["median_wall_ms"]
    cp = result["modes"]["cpptb"]["median_wall_ms"]
    result["ratio_cpptb_over_upstream"] = cp / up

    print(f"\nupstream median {up:8.1f} ms")
    print(f"cpptb    median {cp:8.1f} ms")
    print(f"ratio            {cp / up:.3f}x  (cpptb / upstream)")
    print("both sides validated CoreMark and agree on ticks, iterations "
          "and score")

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
