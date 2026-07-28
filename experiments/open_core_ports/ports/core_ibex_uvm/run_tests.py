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
import concurrent.futures
import json
import re
import subprocess
import sys
import time
from pathlib import Path

import build_tb

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TESTLIST = (ROOT / "deps/ibex/dv/uvm/core_ibex/riscv_dv_extension/testlist.yaml")
BUILD = HERE / "build"
SPIKE_LIB = ROOT / "deps" / "spike_cosim" / "install" / "lib"
TOOLS_LIB = ROOT / "deps" / ".tools" / "root" / "usr" / "lib" / "x86_64-linux-gnu"
TOOLS_BIN = ROOT / "deps" / ".tools" / "root" / "usr" / "bin"

SIGNATURE_ADDR = "8ffffffc"


def binary(config: str) -> Path:
    """The testbench built for one Ibex configuration.

    build_tb.py holds one build directory per configuration, so the binary a
    run uses is chosen here rather than being whichever configuration was
    built last.
    """
    return BUILD / f"obj_{config}" / "core_ibex_tb"


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


def requirements() -> dict[str, dict[str, list[str]]]:
    """The `rtl_params` each testlist entry states, by entry name.

    19 of the 57 entries name a parameter their stimulus needs: the five
    integrity classes want SecureIbex, ten PMP entries want PMPEnable, and the
    three bitmanip entries name the RV32B values they accept, as a scalar or a
    list. The other 38 state nothing and run on any configuration.
    """
    wanted: dict[str, dict[str, list[str]]] = {}
    test, inside = None, False
    for line in TESTLIST.read_text(encoding="utf-8").splitlines():
        stripped = line.strip()
        indent = len(line) - len(line.lstrip())
        if stripped.startswith("- test:"):
            test = stripped.split(":", 1)[1].strip()
            inside = False
        elif stripped.startswith("rtl_params:"):
            inside = True
        elif inside and indent >= 4 and ":" in stripped:
            name, _, value = stripped.partition(":")
            value = value.strip()
            if value.startswith("[") and value.endswith("]"):
                values = [v.strip().strip('"\'')
                          for v in value[1:-1].split(",") if v.strip()]
            else:
                values = [value.strip('"\'')]
            wanted.setdefault(test, {})[name.strip()] = values
        elif stripped.startswith("- ") or (stripped and indent <= 2):
            inside = False
    return wanted


def inapplicable(test: str, wanted: dict[str, dict[str, list[str]]],
                 parameters: dict[str, str],
                 defines: dict[str, str]) -> str | None:
    """Why an entry does not apply to the built configuration, or None.

    The same comparison upstream's `ibex_cmd.filter_tests_by_config` makes.
    Integer parameters come back from ibex_config.py as `-pvalue+`, the enum
    ones as `+define+IBEX_CFG_*`, so both dictionaries are consulted.
    """
    for name, values in wanted.get(test, {}).items():
        built = parameters.get(name, defines.get(f"IBEX_CFG_{name}"))
        if built is None:
            return f"the build does not set {name}"
        if str(built) not in [str(v) for v in values]:
            return (f"needs {name}={'/'.join(values)}, "
                    f"the build has {name}={built}")
    return None


def manifest_tests() -> list[dict]:
    """The per-entry programs build_programs.py has built, if any."""
    manifest = BUILD / "manifest.json"
    if not manifest.is_file():
        return []
    data = json.loads(manifest.read_text(encoding="utf-8"))
    return [test for test in data.get("tests", []) if test.get("bin")]


def manifest_failures() -> list[dict]:
    """The entries build_programs.py could not build, with the reason."""
    manifest = BUILD / "manifest.json"
    if not manifest.is_file():
        return []
    data = json.loads(manifest.read_text(encoding="utf-8"))
    return [test for test in data.get("tests", []) if not test.get("bin")]


def environment() -> dict[str, str]:
    import os

    env = dict(os.environ)
    # z3 for Verilator's constraint solver, and Spike's shared libraries.
    env["PATH"] = f"{TOOLS_BIN}:{env.get('PATH', '')}"
    env["LD_LIBRARY_PATH"] = ":".join(
        [str(TOOLS_LIB), str(SPIKE_LIB), env.get("LD_LIBRARY_PATH", "")])
    return env


def run_one(testbench: Path, rtl_test: str, program: Path, cycles: int,
            seconds: int, extra: list[str], config: str,
            log_name: str | None = None) -> dict:
    name = log_name or rtl_test
    command = [str(testbench), f"+UVM_TESTNAME={rtl_test}",
               f"+bin={program}", f"+signature_addr={SIGNATURE_ADDR}",
               f"+timeout_in_cycles={cycles}", *extra]
    # A directory each, because the tracer writes trace_core_00000000.log into
    # the working directory and several runs at a time would share one.
    directory = BUILD / "runs" / config / name
    directory.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    status = 0
    try:
        completed = subprocess.run(command, cwd=directory, env=environment(),
                                   text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=seconds)
        output, timed_out, status = completed.stdout, False, completed.returncode
    except subprocess.TimeoutExpired as expired:
        output = expired.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        timed_out = True
    elapsed = time.monotonic() - started

    # One log directory per configuration: the two builds run the same entry
    # names, and a shared path means the second sweep silently overwrites the
    # evidence for the first.
    log = BUILD / "logs" / config / f"{name}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    log.write_text(output, encoding="utf-8")

    if timed_out:
        return {"test": name, "outcome": "wall-clock timeout", "detail": "",
                "seconds": round(elapsed, 1)}
    result = {"test": name, "outcome": "no verdict", "detail": "",
              "seconds": round(elapsed, 1)}
    for pattern, outcome in OUTCOMES:
        found = pattern.search(output)
        if found:
            result["outcome"] = outcome
            result["detail"] = found.group(1).strip()[:70] if found.groups() else ""
            break
    # A run that produced no output at all did not get as far as UVM. The
    # likely cause when several run at once is the OOM reaper: the model is
    # 270 MB before UVM allocates anything.
    if status < 0 or (not output.strip() and result["outcome"] == "no verdict"):
        result["outcome"] = "killed"
        result["detail"] = (f"exit {status}, {len(output)} bytes of output in "
                            f"{elapsed:.1f}s")
        return result
    # Verilator solves constraints by shelling out to `z3 --in`. Without one on
    # PATH every randomize() in the testbench fails, `DV_CHECK_RANDOMIZE_FATAL
    # ends the run, and the whole thing is over in about a tenth of a second.
    # That is a broken environment, not a result, and it reads as a testbench
    # failure unless something says so. The discriminator is whether the run got
    # as far as loading the program: without a solver it does not, because
    # `core_ibex_base_test` randomizes in its build phase.
    if (elapsed < 1.0 and result["outcome"] != "passed"
            and "Loading single test binary" not in output):
        result["outcome"] = "environment"
        result["detail"] = (f"ended in {elapsed:.2f}s without loading the "
                            f"program; is z3 on PATH? see "
                            f"{log.relative_to(HERE)}")
    return result


# What a result is worth, given how far its program is from its entry.
# `build_programs.py` writes the verdict into the manifest; the wording here is
# what it means for a run.
VERDICTS = {
    "faithful": "every gen_opt honoured; the outcome is about the entry",
    "partial": "the program differs from its entry in ways that do not remove "
               "the stimulus the entry is named for",
    "hollow": "an option that defines the entry was dropped; the outcome is "
              "not evidence about the entry's name, whatever it says",
}


def report(results: list[dict], skipped: list[dict], config: str,
           unbuilt: list[dict] | None = None) -> int:
    (BUILD / f"results_{config}.json").write_text(
        json.dumps({"config": config, "results": results,
                    "inapplicable": skipped,
                    "not built": unbuilt or []}, indent=2), encoding="utf-8")
    tally: dict[str, int] = {}
    for result in results:
        tally[result["outcome"]] = tally.get(result["outcome"], 0) + 1
    print("\n" + ", ".join(f"{count} {name}"
                           for name, count in sorted(tally.items())))

    # The same results split by how much the program has to do with the entry.
    # A pass on a hollow program is not coverage of the entry it is named for,
    # so it is counted apart rather than in the total above.
    print()
    for verdict, meaning in VERDICTS.items():
        group = [r for r in results if r.get("verdict") == verdict]
        if not group:
            continue
        passed = sum(1 for r in group if r["outcome"] == "passed")
        print(f"  {verdict:<9} {passed:>2} of {len(group):>2} passed  "
              f"-- {meaning}")

    if unbuilt:
        print(f"\n{len(unbuilt)} entries have no program:")
        for entry in unbuilt:
            print(f"  {entry['test']:<44} {entry.get('error', '')}")

    if skipped:
        print(f"\n{len(skipped)} entries are inapplicable to --config "
              f"{config}, which is neither a pass nor a failure:")
        for entry in skipped:
            print(f"  {entry['test']:<44} {entry['reason']}")

    mismatched = [r for r in results if r.get("unsupported")]
    if mismatched:
        print(f"\n{len(mismatched)} of {len(results)} ran a program that does "
              f"not match its testlist entry:")
        for result in mismatched:
            print(f"  {result['test']}  ({result.get('verdict', '?')})")
            for note in result["unsupported"]:
                print(f"    {note}")
    faithful = [r for r in results if r.get("verdict") == "faithful"]
    return 0 if all(r["outcome"] == "passed" for r in faithful) else 1


def run_testlist(args) -> int:
    """One run per testlist entry, each with its own program and sim_opts."""
    tests = manifest_tests()
    if args.only:
        tests = [t for t in tests
                 if t["test"] in args.only or t["rtl_test"] in args.only]

    wanted = requirements()
    parameters, defines = build_tb.config_parameters(args.config)
    skipped = []
    runnable = []
    for test in tests:
        reason = inapplicable(test["test"], wanted, parameters, defines)
        if reason:
            skipped.append({"test": test["test"], "reason": reason})
        else:
            runnable.append(test)

    def one(test: dict) -> dict | None:
        program = (HERE / test["bin"]).resolve()
        if not program.is_file():
            print(f"  {test['test']:<44} no program at {program}",
                  file=sys.stderr)
            return None
        # timeout_s is the entry's own wall-clock budget where it has one.
        seconds = int(test["timeout_s"]) if test.get("timeout_s") \
            else args.timeout_seconds
        result = run_one(binary(args.config), test["rtl_test"], program,
                         args.timeout_cycles, seconds,
                         test.get("sim_opts", []) + args.extra, args.config,
                         log_name=test["test"])
        result["rtl_test"] = test["rtl_test"]
        result["unsupported"] = test.get("unsupported", [])
        result["verdict"] = test.get("verdict", "unknown")
        detail = f"  {result['detail']}" if result["detail"] else ""
        print(f"  {test['test']:<44} {result['outcome']:<18} "
              f"{result['verdict']:<9} {result['seconds']:>6.1f}s{detail}",
              flush=True)
        return result

    # The binary is 270 MB of model plus a UVM environment, so this is memory
    # rather than CPU bound above a handful of jobs; four is what run_directed.py
    # settled on for the same reason.
    jobs = max(1, min(args.jobs, 4))
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        results = [r for r in pool.map(one, runnable) if r is not None]
    results.sort(key=lambda result: result["test"])

    unbuilt = manifest_failures() if not args.only else []
    built = {test["test"] for test in tests} | {t["test"] for t in unbuilt}
    unbuilt += [{"test": test, "error": "never built"}
                for test, _ in entries()
                if test not in built and not args.only]
    return report(results, skipped, args.config, unbuilt)


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
        result = run_one(binary(args.config), rtl_test,
                         args.program.resolve(), args.timeout_cycles,
                         args.timeout_seconds, args.extra, args.config)
        results.append(result)
        detail = f"  {result['detail']}" if result["detail"] else ""
        print(f"  {rtl_test:<48} {result['outcome']}{detail}", flush=True)
    return report(results, [], args.config)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", default="small",
                        help="the Ibex configuration to run against; the "
                             "binary is build/obj_<config>/core_ibex_tb")
    parser.add_argument("--list", action="store_true",
                        help="print the test list and stop")
    parser.add_argument("--program", type=Path,
                        help="run every UVM class against this one program "
                             "instead of the per-entry programs")
    parser.add_argument("--timeout-cycles", type=int, default=500000)
    parser.add_argument("--timeout-seconds", type=int, default=180)
    parser.add_argument("--jobs", type=int, default=4,
                        help="tests to run at a time; capped at 4")
    parser.add_argument("--only", action="append", default=[],
                        help="run only these testlist entries or UVM classes")
    parser.add_argument("extra", nargs="*", help="further plusargs")
    args = parser.parse_args(argv)

    pairs = entries()
    if args.list:
        classes = sorted({rtl for _, rtl in pairs})
        verdicts = {test["test"]: test.get("verdict", "unknown")
                    for test in manifest_tests()}
        wanted = requirements()
        parameters, defines = build_tb.config_parameters(args.config)
        skipped = {test: inapplicable(test, wanted, parameters, defines)
                   for test, _ in pairs}
        print(f"{len(pairs)} riscv-dv tests over {len(classes)} UVM classes, "
              f"{len(verdicts)} with a program, "
              f"{sum(1 for r in skipped.values() if r)} inapplicable to "
              f"--config {args.config}\n")
        for test, rtl in pairs:
            marks = []
            if test not in verdicts:
                marks.append("no program")
            if skipped[test]:
                marks.append(skipped[test])
            print(f"  {test:<45} {verdicts.get(test, ''):<9} {rtl}"
                  f"{'   (' + '; '.join(marks) + ')' if marks else ''}")
        return 0

    if not binary(args.config).is_file():
        raise SystemExit(
            f"run_tests: no testbench at {binary(args.config)}\n"
            f"run: python3 {HERE / 'build_tb.py'} --config {args.config}")
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
