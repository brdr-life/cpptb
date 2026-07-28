#!/usr/bin/env python3
"""Run the icache testbench's tests, from upstream's own test list.

`dv/ibex_icache_sim_cfg.hjson` is what upstream hands to OpenTitan's dvsim. It
names ten tests, each a UVM test class plus a virtual sequence, the plusargs
they all run with, and the patterns that decide a pass. All of that is read
from that file here rather than transcribed, so an upstream change to the list
is a change in what gets run.

    python3 run_tests.py                     # all ten
    python3 run_tests.py --regression smoke  # the nine in the smoke regression
    python3 run_tests.py ibex_icache_ecc --verbosity UVM_HIGH

dvsim runs each test `reseed` times with random seeds; that is 50 per test
here, which is far more simulation than this is for. One seed per test by
default, fixed, with `--reseed` to ask for more.

Standard library only, matching the other tools here.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import re
import subprocess
import sys
import time
from pathlib import Path

import build_tb

HERE = Path(__file__).resolve().parent
SIM_CFG = build_tb.ICACHE / "dv" / "ibex_icache_sim_cfg.hjson"
BINARY = build_tb.OBJ / "ibex_icache_tb"
RESULTS = build_tb.BUILD / "results"

# dvsim's default seed when the Makefile asks for one run. Fixed so that a
# rerun of a failure is the same run.
DEFAULT_SEED = 123


class RunError(Exception):
    pass


# ---------------------------------------------------------------------------
# Reading the sim cfg
# ---------------------------------------------------------------------------

# hjson is JSON with comments, unquoted keys and no commas. The subset this
# file uses is small enough to convert to JSON with three substitutions, which
# keeps the tool standard library only. Anything it cannot convert is an error.
COMMENT = re.compile(r"^\s*//.*$", re.M)
BARE_KEY = re.compile(r"^(\s*)([A-Za-z_][A-Za-z0-9_]*)\s*:", re.M)
KEY_LINE = re.compile(r'^(\s*"[A-Za-z_][A-Za-z0-9_]*":)[ \t]*(\S.*?)[ \t]*$', re.M)
NUMBER = re.compile(r"^-?\d+(\.\d+)?$")
NEEDS_COMMA = re.compile(r"([}\]\"0-9A-Za-z_.])\s*\n(\s*)(?=[\"{\[]|[A-Za-z_])")


def _quote_bare_value(match: re.Match) -> str:
    """Quote an hjson value that JSON would need quotes on.

    A bare value runs to the end of the line and may contain anything, which is
    why this is not a character class: `fusesoc_core` is the VLNV
    `lowrisc:dv:ibex_icache_sim:0.1`, colons and all.
    """
    key, value = match.group(1), match.group(2)
    if value[0] in "\"{[" or NUMBER.match(value) or value in ("true", "false",
                                                              "null"):
        return f"{key} {value}"
    return f'{key} "{value}"'


def read_sim_cfg(path: Path) -> dict:
    """The parts of ibex_icache_sim_cfg.hjson this runner needs."""
    if not path.is_file():
        raise RunError(f"no sim cfg at {path}")
    text = COMMENT.sub("", path.read_text(encoding="utf-8"))
    text = BARE_KEY.sub(r'\1"\2":', text)
    text = KEY_LINE.sub(_quote_bare_value, text)
    # hjson allows a newline in place of a comma. Insert one wherever a value
    # ends and another key or element begins, then let json reject anything
    # this did not handle.
    previous = None
    while previous != text:
        previous = text
        text = NEEDS_COMMA.sub(r"\1,\n\2", text)
    try:
        data = json.loads(text)
    except json.JSONDecodeError as error:
        raise RunError(f"{path.name}: this reader could not convert it to "
                       f"JSON ({error}); upstream has changed") from error

    for key in ("tests", "uvm_test", "uvm_test_seq", "run_opts"):
        if key not in data:
            raise RunError(f"{path.name}: no `{key}` key")
    return data


def test_list(cfg: dict) -> list[dict]:
    """Each test with its class and sequence filled in from the defaults."""
    tests = []
    for entry in cfg["tests"]:
        unknown = set(entry) - {"name", "uvm_test", "uvm_test_seq", "run_opts",
                                "reseed", "build_mode"}
        if unknown:
            raise RunError(f"test {entry.get('name')!r} has keys this runner "
                           f"does not know: {sorted(unknown)}")
        tests.append({
            "name": entry["name"],
            "uvm_test": entry.get("uvm_test", cfg["uvm_test"]),
            "uvm_test_seq": entry.get("uvm_test_seq", cfg["uvm_test_seq"]),
            "run_opts": list(cfg["run_opts"]) + list(entry.get("run_opts", [])),
        })
    return tests


# ---------------------------------------------------------------------------
# Deciding a result
#
# The patterns are common_sim_cfg.hjson's, which is what dvsim applies. A UVM
# warning is a failure there and is kept as one here.
# ---------------------------------------------------------------------------

PASS_PATTERNS = [re.compile(r"^TEST PASSED (UVM_)?CHECKS$", re.M)]
FAIL_PATTERNS = [
    re.compile(r"^UVM_ERROR\s[^:].*$", re.M),
    re.compile(r"^UVM_FATAL\s[^:].*$", re.M),
    re.compile(r"^UVM_WARNING\s[^:].*$", re.M),
    re.compile(r"^Assert failed: ", re.M),
    re.compile(r"^\s*Offending '.*'", re.M),
    re.compile(r"^TEST FAILED (UVM_)?CHECKS$", re.M),
    re.compile(r"^Error:.*$", re.M),
]


def verdict(text: str, returncode: int, timed_out: bool) -> tuple[str, str]:
    """(pass|fail, reason)."""
    if timed_out:
        return "fail", "wall-clock timeout"
    failures = []
    for pattern in FAIL_PATTERNS:
        failures += pattern.findall(text) and [
            match.group(0).strip()
            for match in pattern.finditer(text)]
    if failures:
        return "fail", failures[0][:200]
    if not any(pattern.search(text) for pattern in PASS_PATTERNS):
        if returncode != 0:
            return "fail", f"exit status {returncode}, no pass line"
        return "fail", "no pass line in the log"
    return "pass", ""


# ---------------------------------------------------------------------------
# Running
# ---------------------------------------------------------------------------

def run_one(test: dict, seed: int, verbosity: str, timeout: int) -> dict:
    log = RESULTS / f"{test['name']}.{seed}.log"
    command = [
        str(BINARY),
        f"+verilator+seed+{seed}",
        f"+UVM_TESTNAME={test['uvm_test']}",
        f"+UVM_TEST_SEQ={test['uvm_test_seq']}",
        "+UVM_NO_RELNOTES",
        f"+UVM_VERBOSITY={verbosity}",
        *test["run_opts"],
    ]
    started = time.monotonic()
    timed_out = False
    with log.open("w", encoding="utf-8") as handle:
        handle.write(" ".join(command) + "\n\n")
        handle.flush()
        try:
            completed = subprocess.run(command, cwd=build_tb.BUILD,
                                       stdout=handle, stderr=subprocess.STDOUT,
                                       timeout=timeout, check=False)
            returncode = completed.returncode
        except subprocess.TimeoutExpired:
            timed_out = True
            returncode = -1
    elapsed = time.monotonic() - started
    text = log.read_text(encoding="utf-8", errors="replace")
    status, reason = verdict(text, returncode, timed_out)
    return {"name": test["name"], "seed": seed, "status": status,
            "reason": reason, "seconds": round(elapsed, 1),
            "log": str(log.relative_to(HERE))}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("tests", nargs="*",
                        help="test names to run; default is all of them")
    parser.add_argument("--regression",
                        help="run the named regression from the sim cfg")
    parser.add_argument("--list", action="store_true",
                        help="print the test list and stop")
    parser.add_argument("--reseed", type=int, default=1,
                        help="seeds per test (upstream's default is 50)")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--verbosity", default="UVM_LOW")
    parser.add_argument("--timeout", type=int, default=3600,
                        help="wall-clock seconds per run")
    args = parser.parse_args(argv)

    try:
        cfg = read_sim_cfg(SIM_CFG)
        tests = test_list(cfg)
    except RunError as error:
        print(f"run_tests: {error}", file=sys.stderr)
        return 1

    by_name = {test["name"]: test for test in tests}
    if args.regression:
        groups = {entry["name"]: entry for entry in cfg.get("regressions", [])}
        if args.regression not in groups:
            print(f"run_tests: no regression {args.regression!r}; have "
                  f"{sorted(groups)}", file=sys.stderr)
            return 1
        names = groups[args.regression].get("tests") or list(by_name)
    elif args.tests:
        names = args.tests
    else:
        names = list(by_name)

    unknown = [name for name in names if name not in by_name]
    if unknown:
        print(f"run_tests: unknown tests {unknown}; have {sorted(by_name)}",
              file=sys.stderr)
        return 1

    if args.list:
        width = max(len(name) for name in by_name)
        for name in by_name:
            test = by_name[name]
            print(f"{name:{width}}  {test['uvm_test']:24} "
                  f"{test['uvm_test_seq']}")
        return 0

    if not BINARY.is_file():
        print(f"run_tests: no testbench at {BINARY}\n"
              f"run: python3 build_tb.py", file=sys.stderr)
        return 1

    RESULTS.mkdir(parents=True, exist_ok=True)
    jobs = [(by_name[name], args.seed + index)
            for name in names
            for index in range(args.reseed)]

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(run_one, test, seed, args.verbosity,
                               args.timeout): (test["name"], seed)
                   for test, seed in jobs}
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            results.append(result)
            mark = "pass" if result["status"] == "pass" else "FAIL"
            print(f"{mark}  {result['name']:34} seed {result['seed']} "
                  f"{result['seconds']:7.1f}s  {result['reason']}")

    results.sort(key=lambda r: (r["name"], r["seed"]))
    summary = RESULTS / "summary.json"
    summary.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")

    passed = sum(1 for r in results if r["status"] == "pass")
    print(f"\n{passed} of {len(results)} passed; {summary.relative_to(HERE)}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
