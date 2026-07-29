#!/usr/bin/env python3
"""Record the UVM baseline's stimulus and replay it on the cpptb port.

`run_tests.py --compare` puts the two harnesses side by side on rates, because
each draws from its own random stream and the same seed means nothing across
them. That is a statement about two distributions. This is a statement about
one run: the baseline writes down everything its environment does that the DUT
can see, the port drives the same thing at the same pins, and the DUT's outputs
are compared cycle for cycle.

    python3 replay.py                       # eight tests, one seed, both modes
    python3 replay.py --combo --seeds 10 --jobs 7
    python3 replay.py --mode items          # the item stream only
    python3 replay.py --keep                # leave the recordings on disk
    python3 replay.py --report out.json     # the tables from a saved run

Three modes, two of them from the same recording.

  pins   Every DUT input is driven from the recording and every DUT output is
         compared against it, cycle by cycle. The port's scoreboard runs on the
         baseline's stimulus. A divergence here is a difference between the two
         harnesses' wrappers or between the two drive conventions, because the
         design and the simulator are the same.

  items  Only the core item stream comes from the recording. Every delay below
         the sequence -- the driver's waits, the grant and response timing, the
         key device, the ECC masks -- is still drawn by the port. That isolates
         the item distribution, which is what the residual err/resp difference
         in RESULTS.md turns on.

  own    No recording at all: the port generating its own stimulus at the same
         seed, which is the third column the rate tables want.

The recordings are large: about 60 bytes per cycle, so 4 MB for a run of the
smoke test. They are written under build/replay and removed afterwards unless
--keep is given. The recording run is what takes the time, so --jobs is worth
setting to about the number of cores.

Standard library only, matching the other tools here.
"""

from __future__ import annotations

import argparse
import concurrent.futures
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
WORK = HERE / "build" / "replay"

sys.path.insert(0, str(UVM_PORT))

# The eight tests that run a single virtual sequence. The two combo tests
# change mem_err_shift and the caching-ratio flag between children and neither
# is visible at a DUT pin, so their scoreboard cannot be driven from a
# recording; --combo runs the pin comparison for them with the scoreboard left
# out.
TESTS = ["ibex_icache_smoke",
         "ibex_icache_passthru",
         "ibex_icache_caching",
         "ibex_icache_invalidation",
         "ibex_icache_oldval",
         "ibex_icache_back_line",
         "ibex_icache_many_errors",
         "ibex_icache_ecc"]
COMBO_TESTS = ["ibex_icache_stress_all", "ibex_icache_stress_all_with_reset"]

REPORT_RE = re.compile(r"^cpptb-icache (\S+) (.*)$", re.M)
RESULT_RE = re.compile(r"RESULT iterations=\d+ checks=(\d+) sim_cycles=(\d+) "
                       r"wall_ms=([0-9.]+) failures=(\d+)")
PINS_ONLY_RE = re.compile(r"^cpptb-icache (\S+) replay-pins-only "
                          r"cycles=(\d+)$", re.M)


class RunError(Exception):
    pass


# ---------------------------------------------------------------------------
# The recording run
# ---------------------------------------------------------------------------

def uvm_command(test: str, seed: int, prefix: Path, verbosity: str) -> list:
    """The baseline's own command line, plus the recording plusarg."""
    import build_tb
    import run_tests as uvm_run_tests

    cfg = uvm_run_tests.read_sim_cfg(uvm_run_tests.SIM_CFG)
    by_name = {entry["name"]: entry
               for entry in uvm_run_tests.test_list(cfg)}
    if test not in by_name:
        raise RunError(f"{test} is not in {uvm_run_tests.SIM_CFG.name}")
    entry = by_name[test]
    if not uvm_run_tests.BINARY.is_file():
        raise RunError(f"no baseline at {uvm_run_tests.BINARY}\n"
                       f"run: python3 {UVM_PORT / 'build_tb.py'}")
    return [
        str(uvm_run_tests.BINARY),
        f"+verilator+seed+{seed}",
        f"+UVM_TESTNAME={entry['uvm_test']}",
        f"+UVM_TEST_SEQ={entry['uvm_test_seq']}",
        "+UVM_NO_RELNOTES",
        f"+UVM_VERBOSITY={verbosity}",
        *entry["run_opts"],
        f"+icache_record={prefix}",
    ], build_tb.BUILD


def solver_environment() -> dict:
    """z3 on PATH. Verilator pipes every constrained randomize() to it."""
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
                       (":" + environment[name]) if environment.get(name)
                       else "",
                       value)
        environment[name] = value
    if shutil.which("z3", path=environment.get("PATH")) is None:
        raise RunError("z3 is not on PATH even after local_deps.py; the UVM "
                       "baseline cannot solve a constraint without it, and a "
                       "sub-second run is a broken environment rather than a "
                       "result")
    return environment


def record(test: str, seed: int, prefix: Path, verbosity: str,
           timeout: int) -> dict:
    command, cwd = uvm_command(test, seed, prefix, verbosity)
    log = Path(f"{prefix}.log")
    # The sequence trace is opened in append mode, once per sequence start.
    for suffix in (".pins", ".items", ".seq"):
        Path(f"{prefix}{suffix}").unlink(missing_ok=True)
    started = time.monotonic()
    with log.open("w", encoding="utf-8") as handle:
        handle.write(" ".join(command) + "\n\n")
        handle.flush()
        completed = subprocess.run(command, cwd=cwd, stdout=handle,
                                   stderr=subprocess.STDOUT, timeout=timeout,
                                   check=False)
    elapsed = time.monotonic() - started
    text = log.read_text(encoding="utf-8", errors="replace")

    # A run that did not run is not a result. The baseline takes tens of
    # seconds; anything that returns instantly did not get past elaboration.
    if "TEST PASSED CHECKS" not in text:
        raise RunError(f"the baseline's {test} seed {seed} did not pass; "
                       f"see {log}\n{text[-2000:]}")
    for pattern in ("UVM_ERROR ", "UVM_FATAL ", "UVM_WARNING "):
        for line in text.splitlines():
            if line.startswith(pattern) and not line.startswith(pattern + ":"):
                raise RunError(f"the baseline reported {line!r}; see {log}")

    pins = Path(f"{prefix}.pins")
    items = Path(f"{prefix}.items")
    if not pins.is_file() or not items.is_file():
        raise RunError(f"the baseline wrote no recording under {prefix}")
    cycles = sum(1 for line in pins.open(encoding="utf-8")
                 if line.startswith("C "))
    logged = read_uvm_log(log)
    # The item stream is exact in the recording, so the baseline's stimulus
    # counters need no log at all. Everything else it reports is only in the
    # log, and the pin replay is what says whether that reading is right.
    counters = {"items": 0, "branch_items": 0, "insns_requested": 0}
    for line in items.open(encoding="utf-8"):
        if not line.startswith("I "):
            continue
        fields = line.split()
        counters["items"] += 1
        counters["branch_items"] += int(fields[2])
        counters["insns_requested"] += int(fields[7])
    if cycles == 0 or counters["items"] == 0:
        raise RunError(f"the recording under {prefix} is empty")
    for field, value in logged.items():
        if field in counters and counters[field] != value:
            raise RunError(
                f"{log.name}: the item recording counts {counters[field]} "
                f"{field} and the log counts {value}")
        counters[field] = value
    return {"cycles": cycles, "items": counters["items"],
            "seconds": round(elapsed, 1), "log": str(log),
            "log_counted": bool(logged),
            "counters": counters}


# ---------------------------------------------------------------------------
# The baseline's own view of the run
#
# Its scoreboard reports every transaction it receives at UVM_HIGH and nowhere
# else, which is what run_tests.py --compare counts out of the log. That parser
# is used here rather than a second copy of it, so that the pin replay checks
# it: the same run, counted once out of the log and once off the wire, on every
# field either side keeps. A difference between the two harnesses is then a
# difference in what they did rather than in how it was measured.
#
# Four fields cannot be compared. key_answers and key_refusals are not
# recoverable from the pins, because a refusal carries the same pins as an idle
# cycle; ecc_injections and ecc_errors are not in the log at all; and
# child_sequences is kept by the code that starts a sequence, which a pin
# replay does not run.
# ---------------------------------------------------------------------------

NOT_COMPARABLE = {"key_answers", "key_refusals", "ecc_injections",
                  "ecc_errors", "child_sequences"}


def cpptb_run_tests():
    """This port's own run_tests.py, by path.

    UVM_PORT is first on sys.path so that `import run_tests` finds the
    baseline's runner, and this one shares the name.
    """
    import importlib.util
    spec = importlib.util.spec_from_file_location("icache_cpptb_run_tests",
                                                  HERE / "run_tests.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_uvm_log(path: Path) -> dict:
    module = cpptb_run_tests()
    try:
        # ibex_icache_env_cfg pins the clock to 50 MHz.
        return module.read_uvm_log(path, clock_ps=20_000)
    except module.RunError:
        # Below UVM_HIGH the log reports no transactions at all, which that
        # parser refuses rather than reporting as zeros.
        return {}


# ---------------------------------------------------------------------------
# The replay run
# ---------------------------------------------------------------------------

def replay(test: str, seed: int, prefix: Path, mode: str,
           timeout: int) -> dict:
    if not BINARY.is_file():
        raise RunError(f"no simulator at {BINARY}\n"
                       f"run: uv run --frozen cpptb build --project {HERE}")
    environment = dict(os.environ)
    environment["CPPTB_TEST"] = test
    environment["CPPTB_RANDOM_SEED"] = str(seed)
    environment.pop("ICACHE_REPLAY", None)
    environment.pop("ICACHE_ITEMS", None)
    if mode == "pins":
        environment["ICACHE_REPLAY"] = str(prefix)
    elif mode == "items":
        environment["ICACHE_ITEMS"] = str(prefix)
    # mode "own" is the port generating its own stimulus at the same seed,
    # which is the third column the tables below want.

    started = time.monotonic()
    completed = subprocess.run([str(BINARY)], env=environment, text=True,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=timeout,
                               check=False)
    elapsed = time.monotonic() - started
    text = completed.stdout

    result = RESULT_RE.search(text)
    if result is None:
        raise RunError(f"{test} seed {seed} printed no result line:\n"
                       f"{text[-3000:]}")
    counters = {}
    match = REPORT_RE.search(text)
    if match is not None and "replay-pins-only" not in match.group(2):
        for pair in match.group(2).split():
            key, _, value = pair.partition("=")
            counters[key] = int(value)

    divergence = ""
    for index, line in enumerate(text.splitlines()):
        if "replay divergence" in line:
            divergence = "\n".join(text.splitlines()[index:index + 3])
            break
    return {
        "status": "pass" if int(result.group(4)) == 0 else "fail",
        "checks": int(result.group(1)),
        "failures": int(result.group(4)),
        "seconds": round(elapsed, 2),
        "counters": counters,
        "divergence": divergence,
        "output": text,
    }


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def mean(values: list) -> float:
    return statistics.fmean(values) if values else 0.0


def ratio(counters: dict, top: str, bottom: str) -> float:
    below = counters.get(bottom, 0)
    return counters.get(top, 0) / below if below else float("nan")


def print_summary(results: list) -> None:
    for mode in sorted({entry["mode"] for entry in results}):
        rows = [entry for entry in results if entry["mode"] == mode]
        passed = sum(1 for entry in rows if entry["status"] == "pass")
        print(f"\n{mode}: {passed} of {len(rows)} runs passed")

    pins = [entry for entry in results if entry["mode"] == "pins"]
    if pins:
        print("\nthe pin replay: every DUT output compared at every recorded "
              "cycle")
        print(f"  {'test':36}{'cycles':>10}{'checks':>10}"
              f"{'err/resp log':>14}{'err/resp bus':>14}")
        for test in TESTS + COMBO_TESTS:
            these = [entry for entry in pins if entry["name"] == test]
            if not these:
                continue
            logged = [entry for entry in these if entry.get("log_counted", True)]
            line = (f"  {test:36}"
                    f"{mean([entry['cycles'] for entry in these]):10.0f}"
                    f"{mean([entry['checks'] for entry in these]):10.0f}")
            if logged:
                line += (f"{mean([ratio(entry['uvm_counters'], 'mem_response_errors', 'mem_responses') for entry in logged]):14.4f}")
            else:
                line += f"{'-':>14}"
            print(line + f"{mean([ratio(entry['counters'], 'mem_response_errors', 'mem_responses') for entry in these]):14.4f}")
        print("  `log` is what the baseline's own UVM_HIGH log says and `bus` "
              "is what the")
        print("  port counted off the wire while replaying the same run. A "
              "dash means the")
        print("  recording was made below UVM_HIGH, where that log says "
              "nothing.")
        checked = [entry for entry in pins if entry.get("log_counted")]
        disagreed = [(entry["name"], entry["seed"], entry["log_vs_bus"])
                     for entry in checked if entry.get("log_vs_bus")]
        print(f"  every counter of the {len(checked)} UVM_HIGH runs was read "
              f"both ways: "
              + ("they all agree" if not disagreed else "THEY DISAGREE"))
        for name, seed, fields in disagreed:
            print(f"    {name} seed {seed}: " +
                  ", ".join(f"{field} log {left} bus {right}"
                            for field, left, right in fields))

    # The three-way rate table.
    #
    #   uvm    the baseline's own run. Its counters are the ones the pin replay
    #          took off the wire, which is a complete and exact reading of that
    #          run rather than the partial one a UVM_HIGH log gives.
    #   items  the port driven from the baseline's item stream, with its own
    #          delays below the sequence.
    #   own    the port generating everything itself.
    #
    # uvm against own is what run_tests.py --compare reports. uvm against items
    # is the same comparison with the item distribution held fixed.
    columns = [name for name in ("uvm", "items", "own")
               if any(entry["mode"] == ("pins" if name == "uvm" else name)
                      for entry in results)]
    if len(columns) < 2:
        return
    rates = [("insns/item", "insns_requested", "items"),
             ("fetches/insn", "fetches", "insns_requested"),
             ("grants/fetch", "mem_grants", "fetches"),
             ("err/resp", "mem_response_errors", "mem_responses")]

    for label, top, bottom in rates:
        print(f"\n{label}, means over the seeds run")
        print(f"  {'test':36}" + "".join(f"{column:>12}" for column in columns)
              + f"{'items/uvm':>12}{'own/uvm':>12}")
        for test in TESTS + COMBO_TESTS:
            these = [entry for entry in results if entry["name"] == test]
            if not these:
                continue
            values = {}
            for column in columns:
                mode = "pins" if column == "uvm" else column
                rows = [entry for entry in these if entry["mode"] == mode]
                if not rows:
                    continue
                key = "counters" if column != "uvm" else "counters"
                values[column] = mean([ratio(entry[key], top, bottom)
                                       for entry in rows])
            if "uvm" not in values:
                continue
            line = f"  {test:36}"
            for column in columns:
                line += (f"{values[column]:12.4f}" if column in values
                         else f"{'-':>12}")
            for column in ("items", "own"):
                if column in values and values["uvm"]:
                    line += f"{values[column] / values['uvm']:12.3f}"
                else:
                    line += f"{'-':>12}"
            print(line)
    forced = [entry for entry in results if entry["mode"] == "items"]
    if forced:
        total = sum(entry["counters"].get("forced_branches", 0)
                    for entry in forced)
        items = sum(entry["counters"].get("items", 0) for entry in forced)
        print(f"\nitem replays turned {total} of {items} recorded requests "
              f"into branches,")
        print("because they saw an errored fetch where the recorded run did "
              "not. See README.md.")


def main(argv: list | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("tests", nargs="*", help="default is the eight")
    parser.add_argument("--seeds", type=int, default=1)
    parser.add_argument("--seed", type=int, default=123)
    parser.add_argument("--mode", action="append",
                        choices=["pins", "items", "own"],
                        help="repeatable; the default is pins and items")
    parser.add_argument("--combo", action="store_true",
                        help="also replay the two stress tests, pins only")
    parser.add_argument("--keep", action="store_true",
                        help="leave the recordings under build/replay")
    parser.add_argument("--verbosity", default="UVM_HIGH",
                        help="the baseline's verbosity; UVM_HIGH is what lets "
                             "its own transaction counts be read back")
    parser.add_argument("--jobs", type=int,
                        default=max(1, (os.cpu_count() or 4) // 2),
                        help="parallel record-and-replay jobs")
    parser.add_argument("--timeout", type=int, default=3600)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--report", type=Path,
                        help="print the tables from a saved --json and stop")
    args = parser.parse_args(argv)

    if args.report is not None:
        print_summary(json.loads(args.report.read_text()))
        return 0

    tests = args.tests or (TESTS + (COMBO_TESTS if args.combo else []))
    unknown = [name for name in tests if name not in TESTS + COMBO_TESTS]
    if unknown:
        print(f"replay: unknown tests {unknown}", file=sys.stderr)
        return 1
    seeds = [args.seed + index for index in range(args.seeds)]
    modes = args.mode or ["pins", "items"]

    WORK.mkdir(parents=True, exist_ok=True)

    def one_run(test: str, seed: int) -> list:
        """Record the baseline once, then replay it in each mode."""
        prefix = WORK / f"{test}.{seed}"
        recorded = record(test, seed, prefix, args.verbosity, args.timeout)
        lines = [f"recorded  {test:36} seed {seed} "
                 f"{recorded['cycles']:8} cycles "
                 f"{recorded['items']:5} items "
                 f"{recorded['seconds']:6.1f}s"]
        rows = []
        for mode in modes:
            if mode != "pins" and test in COMBO_TESTS:
                continue
            entry = replay(test, seed, prefix, mode, args.timeout)
            if mode == "pins" and entry["status"] == "pass":
                if recorded["log_counted"]:
                    # The same run, counted once out of the baseline's log by
                    # run_tests.py's parser and once off the wire here. Any
                    # disagreement is a defect in that parser, and a silent one
                    # would put a measurement artefact into every comparison
                    # that tool has ever reported.
                    entry["log_vs_bus"] = sorted(
                        (field, recorded["counters"][field],
                         entry["counters"][field])
                        for field in recorded["counters"]
                        if field not in NOT_COMPARABLE and
                        field in entry["counters"] and
                        recorded["counters"][field] != entry["counters"][field])
                else:
                    # Below UVM_HIGH the log says nothing, so the baseline's
                    # own counters are the ones taken off the wire here.
                    entry["log_vs_bus"] = []
                    for field, value in entry["counters"].items():
                        if field not in NOT_COMPARABLE:
                            recorded["counters"][field] = value
            entry.update({"name": test, "seed": seed, "mode": mode,
                          "cycles": recorded["cycles"],
                          "log_counted": recorded["log_counted"],
                          "uvm_counters": dict(recorded["counters"])})
            output = entry.pop("output")
            rows.append(entry)
            mark = "pass" if entry["status"] == "pass" else "FAIL"
            lines.append(f"{mark}      {mode:6} {test:29} seed {seed} "
                         f"{entry['seconds']:6.2f}s  "
                         f"checks {entry['checks']}")
            if entry["status"] != "pass":
                lines.append(output[-3000:])
        if not args.keep:
            for suffix in (".pins", ".items", ".seq", ".log"):
                Path(f"{prefix}{suffix}").unlink(missing_ok=True)
        return lines, rows

    results = []
    try:
        environment = solver_environment()
        os.environ.update(environment)
        jobs = [(test, seed) for test in tests for seed in seeds]
        # The recording run is what takes the time, is single threaded and is
        # a fresh process per job, so this scales with the cores available.
        with concurrent.futures.ThreadPoolExecutor(
                max_workers=args.jobs) as pool:
            futures = {pool.submit(one_run, test, seed): (test, seed)
                       for test, seed in jobs}
            for future in concurrent.futures.as_completed(futures):
                lines, rows = future.result()
                print("\n".join(lines), flush=True)
                results += rows
    except (RunError, subprocess.TimeoutExpired) as error:
        print(f"replay: {error}", file=sys.stderr)
        return 1
    results.sort(key=lambda entry: (entry["name"], entry["seed"],
                                    entry["mode"]))

    print_summary(results)
    if args.json:
        args.json.write_text(json.dumps(results, indent=2) + "\n",
                             encoding="utf-8")
        print(f"\nwrote {args.json}")

    failed = [entry for entry in results if entry["status"] != "pass"]
    print(f"\n{len(results) - len(failed)} of {len(results)} replays passed")
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())
