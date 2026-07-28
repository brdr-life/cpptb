#!/usr/bin/env python3
"""Run the ported icache tests, and compare them against the UVM baseline.

    python3 run_tests.py                    # the cpptb port, three seeds
    python3 run_tests.py --seeds 10
    python3 run_tests.py ibex_icache_caching
    python3 run_tests.py --compare          # also run ports/ibex_icache_uvm

The two harnesses cannot be given the same stimulus. Each draws from its own
random stream, so the same seed means nothing across them and there is no
transaction log to replay. What is comparable is the generator: both draw the
same fields from the same buckets at the same weights, so per-item rates
should agree, and every check either side makes must pass on both.

--compare therefore reports rates rather than totals. `insns/item` is what the
stimulus asked for, `fetches/insn` is how much of it the cache delivered,
`grants/fetch` is how often a fetch reached memory, and `err/resp` is how often
memory answered with an error. Totals are printed too, but they scale with
num_trans, which is drawn independently on each side.

The UVM numbers are counted out of a UVM_HIGH log, which is the only place
that environment reports its per-transaction activity. Those logs are tens of
megabytes each and land in ports/ibex_icache_uvm/build/results.

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
UVM_PORT = ROOT / "ports" / "ibex_icache_uvm"
BINARY = (ROOT / "work" / "ibex_icache_cpptb" / "cpptb" / "ibex_icache_cpptb" /
          "obj" / "Vdpi_ibex_icache_cpptb")

TESTS = ["ibex_icache_smoke", "ibex_icache_passthru", "ibex_icache_caching"]

# One line per run, written by report() in testbench.cpp.
REPORT_RE = re.compile(r"^cpptb-icache (\S+) (.*)$", re.M)
RESULT_RE = re.compile(r"RESULT iterations=\d+ checks=(\d+) sim_cycles=(\d+) "
                       r"wall_ms=([0-9.]+) failures=(\d+)")

# The counters both sides produce, in the order the tables print them.
FIELDS = ["items", "branch_items", "insns_requested", "fetches",
          "fetch_errors", "branches", "invalidations", "new_seeds",
          "mem_grants", "mem_responses", "mem_response_errors",
          "windows_completed", "windows_checked", "cycles"]


class RunError(Exception):
    pass


# ---------------------------------------------------------------------------
# The cpptb side
# ---------------------------------------------------------------------------

def run_cpptb(test: str, seed: int, timeout: int) -> dict:
    if not BINARY.is_file():
        raise RunError(f"no simulator at {BINARY}\n"
                       f"run: uv run --frozen cpptb build --project {HERE}")
    environment = dict(os.environ)
    environment["CPPTB_TEST"] = test
    environment["CPPTB_RANDOM_SEED"] = str(seed)

    started = time.monotonic()
    completed = subprocess.run([str(BINARY)], env=environment, text=True,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=timeout,
                               check=False)
    elapsed = time.monotonic() - started
    text = completed.stdout

    match = REPORT_RE.search(text)
    if match is None:
        raise RunError(f"{test} seed {seed} printed no report line:\n"
                       f"{text[-2000:]}")
    counters = {}
    for pair in match.group(2).split():
        key, _, value = pair.partition("=")
        counters[key] = int(value)

    result = RESULT_RE.search(text)
    if result is None:
        raise RunError(f"{test} seed {seed} printed no result line")
    return {
        "harness": "cpptb", "name": test, "seed": seed,
        "status": "pass" if int(result.group(4)) == 0 else "fail",
        "checks": int(result.group(1)),
        "failures": int(result.group(4)),
        "seconds": round(elapsed, 2),
        "counters": counters,
    }


# ---------------------------------------------------------------------------
# The UVM baseline
#
# Its scoreboard reports every transaction it receives at UVM_HIGH and nowhere
# else, so the counters are recovered from the log. Each `received ...` line
# opens a printed item whose fields follow it; the field lines are not tagged
# with the component, so the parser counts a fixed number of lines after each
# header rather than trying to recognise where a table ends. The core driver
# prints the item it was handed with the same field names, which is where the
# stimulus counters come from.
# ---------------------------------------------------------------------------

CORE_HEADER = "received core transaction"
MEM_HEADER = "received mem transaction"
SEED_HEADER = "received new seed"
DRIVER_HEADER = "core_driver.sv:32"
ITEM_LINES = 12
TIME_RE = re.compile(r"UVM_INFO @ *(\d+) ps")
NUM_INSNS_RE = re.compile(r"^  num_insns +integral +32 +'h([0-9a-f]+)")
NEW_SEED_RE = re.compile(r"^  new_seed +integral +32 +'h([0-9a-f]+)")
IS_GRANT_RE = re.compile(r"^  is_grant +integral +1 +'h([01])")
ERR_RE = re.compile(r"^  err +integral +1 +'h([01])")


def read_uvm_log(path: Path, clock_ps: int) -> dict:
    counters = {field: 0 for field in FIELDS}
    last_time = 0
    left = 0
    section = ""
    in_fetch = False

    with path.open(encoding="utf-8", errors="replace") as handle:
        for line in handle:
            stamp = TIME_RE.search(line)
            if stamp is not None:
                last_time = max(last_time, int(stamp.group(1)))

            if CORE_HEADER in line:
                left, section = ITEM_LINES, "core"
                continue
            if MEM_HEADER in line:
                left, section = ITEM_LINES, "mem"
                continue
            if SEED_HEADER in line:
                counters["new_seeds"] += 1
                continue
            if "Completed window" in line:
                counters["windows_completed"] += 1
                continue
            if "Fetch ratio" in line:
                counters["windows_checked"] += 1
                continue
            if DRIVER_HEADER in line and "rcvd item" in line:
                left, section = ITEM_LINES, "driver"
                counters["items"] += 1
                continue

            if left <= 0:
                continue
            left -= 1

            if section == "core":
                if "ICacheCoreBusTransTypeFetch" in line:
                    counters["fetches"] += 1
                    in_fetch = True
                elif "ICacheCoreBusTransTypeBranch" in line:
                    counters["branches"] += 1
                    in_fetch = False
                elif "ICacheCoreBusTransTypeInvalidate" in line:
                    counters["invalidations"] += 1
                    in_fetch = False
                elif "ICacheCoreBusTransType" in line:
                    in_fetch = False
                elif in_fetch:
                    match = ERR_RE.match(line)
                    if match is not None and match.group(1) == "1":
                        counters["fetch_errors"] += 1
                        in_fetch = False
            elif section == "mem":
                match = IS_GRANT_RE.match(line)
                if match is not None:
                    key = "mem_grants" if match.group(1) == "1" \
                        else "mem_responses"
                    counters[key] += 1
                    continue
                match = ERR_RE.match(line)
                if match is not None and match.group(1) == "1":
                    counters["mem_response_errors"] += 1
            elif section == "driver":
                if "ICacheCoreTransTypeBranch" in line:
                    counters["branch_items"] += 1
                match = NUM_INSNS_RE.match(line)
                if match is not None:
                    counters["insns_requested"] += int(match.group(1), 16)

    counters["cycles"] = last_time // clock_ps
    return counters


def run_uvm(tests: list[str], seeds: list[int], jobs: int,
            timeout: int) -> list[dict]:
    runner = UVM_PORT / "run_tests.py"
    if not runner.is_file():
        raise RunError(f"no UVM baseline at {UVM_PORT}")

    # An SMT solver has to be on PATH: Verilator pipes every constrained
    # randomize() to `z3 --in`, and without one the baseline dies at time zero
    # with the real explanation only in a warning at the top of the run.
    environment = dict(os.environ)
    exports = subprocess.run([sys.executable, str(ROOT / "local_deps.py"),
                              "--env"], text=True, stdout=subprocess.PIPE,
                             check=True).stdout
    for line in exports.splitlines():
        match = re.match(r'export (\w+)="([^"]*)"', line)
        if match is None:
            continue
        name, value = match.group(1), match.group(2)
        value = re.sub(r'\$\{' + name + r':\+:\$\{' + name + r'\}\}',
                       (":" + environment[name]) if environment.get(name) else "",
                       value)
        environment[name] = value
    if shutil.which("z3", path=environment.get("PATH")) is None:
        raise RunError("z3 is not on PATH even after local_deps.py; the UVM "
                       "baseline cannot solve a constraint without it, and a "
                       "sub-second run is a broken environment rather than a "
                       "result")

    command = [sys.executable, str(runner), *tests,
               "--verbosity", "UVM_HIGH", "--jobs", str(jobs),
               "--seed", str(seeds[0]), "--reseed", str(len(seeds)),
               "--timeout", str(timeout)]
    started = time.monotonic()
    completed = subprocess.run(command, cwd=UVM_PORT, env=environment,
                               text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=False)
    elapsed = time.monotonic() - started
    if completed.returncode != 0:
        raise RunError(f"the UVM baseline did not pass:\n{completed.stdout}")

    summary = json.loads(
        (UVM_PORT / "build" / "results" / "summary.json").read_text())
    by_run = {(entry["name"], entry["seed"]): entry for entry in summary}

    results = []
    for test in tests:
        for seed in seeds:
            entry = by_run.get((test, seed))
            if entry is None:
                raise RunError(f"the baseline ran no {test} at seed {seed}")
            log = UVM_PORT / entry["log"]
            results.append({
                "harness": "uvm", "name": test, "seed": seed,
                "status": entry["status"], "failures": 0,
                "seconds": entry["seconds"],
                # ibex_icache_env_cfg pins the clock to 50 MHz.
                "counters": read_uvm_log(log, clock_ps=20_000),
            })
    print(f"the UVM baseline took {elapsed:.0f}s for "
          f"{len(tests) * len(seeds)} runs")
    return results


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def rates(counters: dict) -> dict:
    def ratio(top: str, bottom: str) -> float:
        below = counters.get(bottom, 0)
        return counters.get(top, 0) / below if below else 0.0

    return {
        "insns/item": ratio("insns_requested", "items"),
        "fetches/insn": ratio("fetches", "insns_requested"),
        "grants/fetch": ratio("mem_grants", "fetches"),
        "err/resp": ratio("mem_response_errors", "mem_responses"),
        "cycles/item": ratio("cycles", "items"),
        "fetches/cycle": ratio("fetches", "cycles"),
    }


def mean(values: list[float]) -> float:
    return statistics.fmean(values) if values else 0.0


def print_totals(results: list[dict]) -> None:
    harnesses = sorted({entry["harness"] for entry in results})
    width = max(len(field) for field in FIELDS) + 2
    for test in TESTS:
        rows = [entry for entry in results if entry["name"] == test]
        if not rows:
            continue
        print(f"\n{test}: mean over "
              f"{len({entry['seed'] for entry in rows})} seeds")
        header = "  " + "field".ljust(width)
        for harness in harnesses:
            header += harness.rjust(14)
        print(header)
        for field in FIELDS:
            line = "  " + field.ljust(width)
            for harness in harnesses:
                values = [entry["counters"].get(field, 0) for entry in rows
                          if entry["harness"] == harness]
                line += f"{mean(values):14.1f}"
            print(line)
        line = "  " + "seconds".ljust(width)
        for harness in harnesses:
            values = [entry["seconds"] for entry in rows
                      if entry["harness"] == harness]
            line += f"{mean(values):14.2f}"
        print(line)

        print("  " + "-" * (width + 14 * len(harnesses)))
        names = list(rates({}).keys())
        for name in names:
            line = "  " + name.ljust(width)
            for harness in harnesses:
                values = [rates(entry["counters"])[name] for entry in rows
                          if entry["harness"] == harness]
                line += f"{mean(values):14.4f}"
            print(line)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("tests", nargs="*", help="default is all three")
    parser.add_argument("--seeds", type=int, default=3)
    parser.add_argument("--seed", type=int, default=123,
                        help="first seed; the baseline's default is 123")
    parser.add_argument("--compare", action="store_true",
                        help="also run ports/ibex_icache_uvm and tabulate both")
    parser.add_argument("--jobs", type=int, default=3,
                        help="parallel UVM runs")
    parser.add_argument("--timeout", type=int, default=3600)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args(argv)

    tests = args.tests or TESTS
    unknown = [name for name in tests if name not in TESTS]
    if unknown:
        print(f"run_tests: unknown tests {unknown}; have {TESTS}",
              file=sys.stderr)
        return 1
    seeds = [args.seed + index for index in range(args.seeds)]

    results: list[dict] = []
    try:
        for test in tests:
            for seed in seeds:
                entry = run_cpptb(test, seed, args.timeout)
                results.append(entry)
                mark = "pass" if entry["status"] == "pass" else "FAIL"
                print(f"{mark}  cpptb {entry['name']:22} seed {seed} "
                      f"{entry['seconds']:6.2f}s  "
                      f"checks {entry['checks']}")
        if args.compare:
            results += run_uvm(tests, seeds, args.jobs, args.timeout)
    except (RunError, subprocess.TimeoutExpired) as error:
        print(f"run_tests: {error}", file=sys.stderr)
        return 1

    print_totals(results)

    if args.json:
        args.json.write_text(json.dumps(results, indent=2) + "\n",
                             encoding="utf-8")
        print(f"\nwrote {args.json}")

    failed = [entry for entry in results if entry["status"] != "pass"]
    print(f"\n{len(results) - len(failed)} of {len(results)} runs passed")
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
