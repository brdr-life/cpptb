#!/usr/bin/env python3
"""Build and run core_ibex's directed tests against the cpptb port.

`directed_tests/directed_testlist.yaml` is 944 hand-written C and assembly
entries with no generator in the flow. `ports/core_ibex_uvm` runs them under the
UVM environment on Verilator and passes 912; this runs the same entries against
the cpptb testbench next to it.

**The two harnesses run byte-identical binaries.** Everything about reading the
testlist, patching the linker script, adding the `-march` and the `TEST_CASE_1`
define, and invoking gcc and objcopy is imported from
`ports/core_ibex_uvm/run_directed.py` rather than reimplemented here. A second
copy of that would be a second set of decisions about what upstream's `gcc_opts`
mean, and a difference between the two harnesses that is really a difference
between two compiles is the least interesting failure there is.

    python3 ../core_ibex_uvm/build_tb.py --config opentitan   # for --compare
    uv run --frozen cpptb build --project .
    python3 run_directed.py --group riscv-tests
    python3 run_directed.py                       # all 944
    python3 run_directed.py --only add-01 --compare

What differs from the UVM runner, and only this:

  * the simulation is `Vdpi_core_ibex_cpptb` with the program named by an
    environment variable, where upstream's is `core_ibex_tb` with plusargs;
  * the verdict is read off the one `cpptb-core-ibex ... outcome=` line the
    testbench prints, and cross-checked against the framework's own
    `failures=` count. A log where those two disagree is reported as
    unreadable rather than as a result, which is the check
    `ports/core_ibex_uvm/run_directed.py` makes against the UVM report summary
    and for the same reason;
  * `--jobs` is not capped at four. The UVM runner caps it because every
    simulation spawns a z3 subprocess for its constraint solving; there is no
    solver here.

Every run writes `build/directed/<run>/`, holding one subdirectory per entry and
that run's `results.json`, and refuses to write into a directory that already
exists. The results file names the run, the command that produced it, the
testbench binary and whether the run covered the whole testlist; every record in
it names its own log by a path relative to `build/`. That is
`ports/core_ibex_uvm/run_directed.py`'s shape, and it is there because results
files that do not say which run produced them have caused wrong numbers on this
project twice.

Standard library only, matching the other tools here.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
UVM_PORT = HERE.parent / "core_ibex_uvm"
sys.path.insert(0, str(UVM_PORT))

import run_directed as uvm  # noqa: E402  the baseline's runner, reused whole

BUILD = HERE / "build"
RUNS = BUILD / "directed"
BINARY = (ROOT / "work" / "core_ibex_cpptb" / "cpptb" / "core_ibex_cpptb" /
          "obj" / "Vdpi_core_ibex_cpptb")
SPIKE_LIB = ROOT / "deps" / "spike_cosim" / "install" / "lib"
TOOLS_LIB = ROOT / "deps" / ".tools" / "root" / "usr" / "lib" / "x86_64-linux-gnu"

SIGNATURE_ADDR = uvm.SIGNATURE_ADDR

# The one directed entry that does not name core_ibex_base_test. Its class is
# ported; every other rtl_test in the file is the base test.
SUPPORTED_TESTS = {
    "core_ibex_base_test",
    "core_ibex_mcounteren_lock_test",
}

OUTCOME_LINE = re.compile(r"^cpptb-core-ibex \S+ outcome=(.+)$", re.MULTILINE)
# The testbench prints one word; these are the spellings
# ports/core_ibex_uvm/run_directed.py uses, so the two results files can be
# compared entry by entry without a translation table on the reading side.
OUTCOME_NAMES = {
    "passed": "passed",
    "self-check-failed": "self-check failed",
    "cosim-mismatch": "cosim mismatch",
    "double-faults": "double faults",
    "cycle-timeout": "cycle timeout",
    "malformed-handshake": "malformed handshake",
    "no-verdict": "no verdict",
}
COUNTER = re.compile(r"(\w+)=([0-9a-fx]+)")
RESULT_LINE = re.compile(
    r"^CPP_DPI_\w+_RESULT .*failures=(\d+)", re.MULTILINE)


def environment() -> dict[str, str]:
    env = dict(os.environ)
    # Spike's shared libraries. Unlike the UVM runner this needs no z3: cpptb
    # draws its delays directly, and nothing in this testbench solves a
    # constraint.
    env["LD_LIBRARY_PATH"] = ":".join(
        [str(TOOLS_LIB), str(SPIKE_LIB), env.get("LD_LIBRARY_PATH", "")])
    return env


def counters(text: str) -> dict[str, int]:
    found = OUTCOME_LINE.search(text)
    if not found:
        return {}
    values: dict[str, int] = {}
    for name, value in COUNTER.findall(found.group(1)):
        try:
            values[name] = int(value, 0)
        except ValueError:
            continue
    return values


def run_one(entry: dict, binary: Path, directory: Path, cycles: int,
            seconds: int, seed: int, extra_env: dict[str, str]) -> dict:
    env = environment()
    env["CPPTB_TEST"] = entry["rtl_test"]
    env["CPPTB_RANDOM_SEED"] = str(seed)
    env["IBEX_BIN"] = str(binary)
    env["IBEX_SIGNATURE_ADDR"] = SIGNATURE_ADDR
    env["IBEX_TIMEOUT_CYCLES"] = str(cycles)
    env.update(extra_env)

    started = time.monotonic()
    status = 0
    try:
        completed = subprocess.run([str(BINARY)], cwd=directory, env=env,
                                   text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=seconds)
        output, timed_out, status = completed.stdout, False, completed.returncode
    except subprocess.TimeoutExpired as expired:
        output = expired.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        timed_out = True
    elapsed = time.monotonic() - started

    log = directory / "sim.log"
    log.write_text(output, encoding="utf-8")
    where = {"log": log.relative_to(BUILD).as_posix(), "seconds": elapsed}

    if timed_out:
        return {"outcome": "wall-clock timeout", "detail": "", **where}
    if status < 0:
        return {"outcome": "killed",
                "detail": f"signal {-status} after {elapsed:.1f}s, "
                          f"{len(output)} bytes of output", **where}

    found = OUTCOME_LINE.search(output)
    if not found:
        return {"outcome": "no verdict",
                "detail": "the testbench printed no outcome line", **where}
    token = found.group(1).split()[0]
    if token not in OUTCOME_NAMES:
        return {"outcome": "unreadable log",
                "detail": f"the testbench printed outcome={token}, which this "
                          f"reader does not know", **where}
    outcome = OUTCOME_NAMES[token]
    values = counters(output)

    # The framework counts its own failures, and the testbench records one for
    # every ending that is not a pass. If the two disagree, this reader is
    # matching something it should not, and that is worth stopping on rather
    # than recording as a result.
    failures = RESULT_LINE.search(output)
    if failures is None:
        return {"outcome": "unreadable log",
                "detail": "no framework result line to confirm the verdict",
                **where}
    if (int(failures.group(1)) == 0) != (outcome == "passed"):
        return {"outcome": "unreadable log",
                "detail": f"outcome={outcome} with failures="
                          f"{failures.group(1)}", **where}

    detail = ""
    for line in output.splitlines():
        if line.startswith("cpptb-core-ibex detail:"):
            detail = line.split(":", 1)[1].strip()[:70]
            break
    return {"outcome": outcome, "detail": detail, "counters": values, **where}


def handle(entry: dict, run: Path, march: str, cycles: int, wall_seconds: int,
           seed: int, extra_env: dict[str, str], stock_ld: bool,
           stock_defines: bool) -> dict:
    directory = run / entry["test"]
    result = {"test": entry["test"], "group": entry["config"],
              "rtl_test": entry["rtl_test"],
              "dir": directory.relative_to(BUILD).as_posix()}

    if entry["rtl_test"] not in SUPPORTED_TESTS:
        result["outcome"] = "unsupported class"
        result["detail"] = f"{entry['rtl_test']} is not ported"
        return result

    build = uvm.build_one(entry, march, directory, stock_ld, stock_defines)
    result["notes"] = build["notes"]
    if not build["built"]:
        result["outcome"] = "build failed"
        result["detail"] = build["detail"]
        result["log"] = (directory / "compile.log").relative_to(BUILD).as_posix()
        return result
    result["bytes"] = build["bytes"]

    seconds = wall_seconds or int(entry.get("timeout_s", 300))
    result.update(run_one(entry, build["binary"], directory, cycles, seconds,
                          seed, extra_env))

    # The ePMP programs compute a verdict and then throw it away: syscalls.c's
    # tohost_exit(code) signals TEST_PASS whatever code is. The code is still in
    # the trace, and this is where the UVM runner reads it from too.
    if entry["config"] == "epmp-tests" and result["outcome"] == "passed":
        trace = directory / "trace_core_00000000.log"
        code, missing = uvm.epmp_exit_code(trace)
        result["exit_code"] = code
        result["trace"] = trace.relative_to(BUILD).as_posix()
        if code is None:
            result["outcome"] = "no verdict"
            result["detail"] = missing
        elif code != 0:
            result["outcome"] = "self-check failed"
            result["detail"] = (f"the program exited {code}"
                                if code != uvm.EPMP_TRAP_CODE else
                                f"the program took an unhandled trap "
                                f"(handle_trap exits {uvm.EPMP_TRAP_CODE})")
    return result


def report(results: list[dict], inapplicable: list[dict], total: int,
           run: Path, header: dict) -> int:
    output = run / "results.json"
    document = {**header, "results": results, "inapplicable": inapplicable}
    output.write_text(json.dumps(document, indent=2), encoding="utf-8")

    tally: dict[str, int] = {}
    for result in results:
        tally[result["outcome"]] = tally.get(result["outcome"], 0) + 1

    print(f"\ncpptb {header['config']}: {len(results)} run of {total} entries")
    for outcome, count in sorted(tally.items(), key=lambda item: -item[1]):
        print(f"  {count:>5}  {outcome}")

    groups: dict[str, dict[str, int]] = {}
    for result in results:
        by_outcome = groups.setdefault(result["group"], {})
        by_outcome[result["outcome"]] = by_outcome.get(result["outcome"], 0) + 1
    if len(groups) > 1:
        print()
        for group, by_outcome in sorted(groups.items()):
            summary = ", ".join(f"{count} {outcome}" for outcome, count
                                in sorted(by_outcome.items(),
                                          key=lambda item: -item[1]))
            print(f"  {group:<18} {summary}")

    if inapplicable:
        print(f"  {len(inapplicable):>5}  inapplicable to this configuration")

    seconds = sum(r.get("seconds", 0.0) for r in results)
    steps = sum((r.get("counters") or {}).get("cosim_matched", 0)
                for r in results)
    cycles = sum((r.get("counters") or {}).get("cycles", 0) for r in results)
    print(f"\n{cycles:,} cycles and {steps:,} co-simulated instructions in "
          f"{seconds:,.0f} simulator-seconds")

    if not header.get("complete"):
        print(f"\nthis run covered {len(results) + len(inapplicable)} of the "
              f"{header['entries']} entries in the testlist, so it is not a "
              f"result for the testlist. It supersedes an earlier run only for "
              f"the entries it names.")
    print(f"\nwritten to {output}")
    return 0 if tally.get("passed", 0) == len(results) and results else 1


def open_run(name: str, config: str) -> Path:
    run = RUNS / (name or
                  f"{config}-{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}")
    if run.exists():
        raise uvm.DirectedError(
            f"{run} already exists. A run writes its logs and its results file "
            f"into a directory of its own and will not write over another "
            f"run's; pass --run-name for a different one, or move this aside.")
    run.mkdir(parents=True)
    return run


def index() -> int:
    runs = sorted(p for p in RUNS.glob("*/results.json"))
    if not runs:
        print(f"no runs under {RUNS}")
        return 1
    for path in sorted(runs, key=lambda p: p.stat().st_mtime):
        data = json.loads(path.read_text(encoding="utf-8"))
        results = data.get("results", [])
        passed = sum(1 for r in results if r.get("outcome") == "passed")
        scope = "all" if data.get("complete") else "part"
        print(f"  {data.get('run', path.parent.name):<34} "
              f"{data.get('config', '?'):<10} {data.get('started', ''):<21} "
              f"{scope}  {passed} of {len(results)} passed  "
              f"{path.relative_to(BUILD)}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", default="opentitan",
                        help="the Ibex configuration the testbench was built "
                             "for; only opentitan is committed, and every "
                             "directed entry needs PMPEnable")
    parser.add_argument("--group", action="append", default=[])
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--pattern", action="append", default=[])
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--stride", type=int, default=1,
                        help="take every Nth entry, for a spread sample")
    parser.add_argument("--offset", type=int, default=0)
    parser.add_argument("--jobs", type=int, default=4,
                        help="entries in flight at once; not capped, because "
                             "nothing here spawns a constraint solver")
    parser.add_argument("--timeout-cycles", type=int, default=5000000)
    parser.add_argument("--timeout-seconds", type=int, default=0)
    parser.add_argument("--seed", type=int, default=1,
                        help="CPPTB_RANDOM_SEED, which picks the memory "
                             "delays, the fetch-enable stimulus and whether "
                             "spurious dside responses are on")
    parser.add_argument("--stock-ld", action="store_true")
    parser.add_argument("--stock-defines", action="store_true")
    parser.add_argument("--env", action="append", default=[],
                        help="extra NAME=VALUE for the simulation, such as "
                             "IBEX_CORRUPT_DMEM=1")
    parser.add_argument("--run-name", default="")
    parser.add_argument("--index", action="store_true")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args(argv)

    if args.index:
        return index()

    try:
        entries = uvm.merge(*uvm.parse_testlist(uvm.TESTLIST))
        parameters, defines = uvm.build_tb.config_parameters(args.config)
        march = uvm.isa_march(defines)
        chosen = uvm.select(entries, args)
    except (uvm.DirectedError, uvm.build_tb.BuildError) as error:
        print(f"run_directed: {error}", file=sys.stderr)
        return 1

    runnable, inapplicable = [], []
    for entry in chosen:
        reason = uvm.applicability(entry, parameters, defines)
        if reason:
            inapplicable.append({"test": entry["test"],
                                 "group": entry["config"], "reason": reason})
        else:
            runnable.append(entry)

    if args.list:
        print(f"{len(entries)} directed entries, {len(chosen)} selected, "
              f"{len(runnable)} applicable to --config {args.config}")
        for entry in chosen:
            print(f"  {entry['test']:<58} {entry['config']:<18} "
                  f"{entry['rtl_test']}")
        return 0

    if not BINARY.is_file():
        print(f"run_directed: no testbench at {BINARY}\n"
              f"build it with: uv run --frozen cpptb build --project {HERE}",
              file=sys.stderr)
        return 1
    for tool in (uvm.GCC, uvm.OBJCOPY):
        if not tool.is_file():
            print(f"run_directed: no {tool.name} at {tool}", file=sys.stderr)
            return 1

    extra_env: dict[str, str] = {}
    for assignment in args.env:
        name, _, value = assignment.partition("=")
        extra_env[name] = value

    try:
        run = open_run(args.run_name, args.config)
    except uvm.DirectedError as error:
        print(f"run_directed: {error}", file=sys.stderr)
        return 1

    header = {
        "run": run.name,
        "harness": "cpptb",
        "config": args.config,
        "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "complete": len(chosen) == len(entries),
        "entries": len(entries),
        "selected": len(chosen),
        "seed": args.seed,
        "stock_ld": args.stock_ld,
        "stock_defines": args.stock_defines,
        "testbench": str(BINARY),
        "march": march,
        "extra_env": extra_env,
        "command": shlex.join([sys.executable, str(HERE / "run_directed.py")]
                              + (argv if argv is not None else sys.argv[1:])),
    }

    jobs = max(1, args.jobs)
    results: list[dict] = []
    print(f"run_directed: {len(runnable)} entries on --config {args.config}, "
          f"{len(inapplicable)} inapplicable, {jobs} at a time")
    print(f"run_directed: writing to {run}")
    wall_started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {pool.submit(handle, entry, run, march, args.timeout_cycles,
                               args.timeout_seconds, args.seed, extra_env,
                               args.stock_ld, args.stock_defines): entry
                   for entry in runnable}
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            results.append(result)
            detail = f"  {result['detail']}" if result.get("detail") else ""
            print(f"  {result['test']:<58} {result['outcome']}{detail}",
                  flush=True)

    results.sort(key=lambda r: (r["group"], r["test"]))
    header["finished"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    header["wall_seconds"] = time.monotonic() - wall_started
    header["jobs"] = jobs
    return report(results, inapplicable, len(chosen), run, header)


if __name__ == "__main__":
    raise SystemExit(main())
