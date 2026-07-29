#!/usr/bin/env python3
"""Corrupt one memory response at a time, and count what the scoreboard catches.

A testbench that passes a thousand tests has said nothing about whether it is
checking anything. This flips one bit of one memory response per run, over a
range of responses, and reports how many of those runs the co-simulation
scoreboard caught. `ports/ibex_icache_cpptb` uses the same idea for the same
reason; the difference is that the reference here is Spike rather than a
memory model, so a caught corruption is Spike and Ibex disagreeing about an
executed instruction rather than a scoreboard rejecting a fetch.

    python3 inject.py                                  # imem on the default entry
    python3 inject.py --bus dmem --count 40
    python3 inject.py --only add-01 --count 100 --no-cosim

`--no-cosim` re-runs the same injections with `IBEX_NO_COSIM=1`, which is the
control: with the reference model gone, every one of them should pass. A run
where the two columns agree means the co-simulation is not what is catching
anything.

Read responses only. A write response carries no payload the core reads: the
agent drives a constant with matching integrity and the load/store unit
discards it, so corrupting one is invisible by construction and would only pad
the "silent" column.

Standard library only, matching the other tools here.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


cpptb = _load("cpptb_run_directed", HERE / "run_directed.py")
uvm = cpptb.uvm

BUILD = HERE / "build"
RUNS = BUILD / "inject"

# Enough dside reads to be worth injecting into, and 227,000 cycles rather than
# the millions the longest entries take.
DEFAULT_ENTRY = "pmp_mseccfg_test_rlb1_l0_0_u0"


def run(entry: dict, binary: Path, directory: Path, bus: str, index: int,
        cycles: int, seconds: int, no_cosim: bool) -> dict:
    env = cpptb.environment()
    env["CPPTB_TEST"] = entry["rtl_test"]
    env["CPPTB_RANDOM_SEED"] = "1"
    env["IBEX_BIN"] = str(binary)
    env["IBEX_SIGNATURE_ADDR"] = uvm.SIGNATURE_ADDR
    env["IBEX_TIMEOUT_CYCLES"] = str(cycles)
    if index:
        env[f"IBEX_CORRUPT_{bus.upper()}"] = str(index)
    if no_cosim:
        env["IBEX_NO_COSIM"] = "1"

    started = time.monotonic()
    status = 0
    try:
        completed = subprocess.run([str(cpptb.BINARY)], cwd=directory, env=env,
                                   text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=seconds)
        output, status = completed.stdout, completed.returncode
    except subprocess.TimeoutExpired as expired:
        output = expired.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        status = None
    elapsed = time.monotonic() - started

    found = cpptb.OUTCOME_LINE.search(output)
    token = found.group(1).split()[0] if found else "no-verdict"
    injected = next((line for line in output.splitlines()
                     if "corrupting" in line), "")
    detail = next((line for line in output.splitlines()
                   if "Cosim mismatch" in line), "")
    # A corrupted instruction fetch can hand Spike a decode it does not survive.
    # ports/core_ibex_uvm records the same two crashes on its own runs, so this
    # is the reference model rather than the port, and it is neither a catch nor
    # a silent pass. It is recorded as itself.
    if status is None:
        token = "wall-clock timeout"
    elif status != 0 and "outcome=" not in output:
        crash = next((line for line in output.splitlines()
                      if "stack smashing" in line or "Segmentation" in line),
                     f"exited {status}")
        token = "reference model crashed"
        detail = crash
    return {"index": index, "outcome": token, "seconds": elapsed,
            "status": status,
            "injected": injected.strip()[:80],
            "detail": detail.strip()[-90:]}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--only", default=DEFAULT_ENTRY,
                        help="the directed entry to inject into")
    parser.add_argument("--bus", default="imem", choices=["imem", "dmem"])
    parser.add_argument("--count", type=int, default=50,
                        help="corrupt read responses 1..N, one per run")
    parser.add_argument("--timeout-cycles", type=int, default=5000000)
    parser.add_argument("--timeout-seconds", type=int, default=600)
    parser.add_argument("--no-cosim", action="store_true",
                        help="also run the whole sweep with the reference "
                             "model off, as a control")
    parser.add_argument("--run-name", default="")
    args = parser.parse_args(argv)

    try:
        entries = uvm.merge(*uvm.parse_testlist(uvm.TESTLIST))
        parameters, defines = uvm.build_tb.config_parameters("opentitan")
        march = uvm.isa_march(defines)
    except (uvm.DirectedError, uvm.build_tb.BuildError) as error:
        print(f"inject: {error}", file=sys.stderr)
        return 1

    by_name = {entry["test"]: entry for entry in entries}
    if args.only not in by_name:
        print(f"inject: no entry named {args.only}", file=sys.stderr)
        return 1
    entry = by_name[args.only]

    if not cpptb.BINARY.is_file():
        print(f"inject: no testbench at {cpptb.BINARY}", file=sys.stderr)
        return 1

    run_dir = RUNS / (args.run_name or
                      time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()))
    if run_dir.exists():
        print(f"inject: {run_dir} already exists", file=sys.stderr)
        return 1
    run_dir.mkdir(parents=True)

    build = uvm.build_one(entry, march, run_dir, False, False)
    if not build["built"]:
        print(f"inject: {build['detail']}", file=sys.stderr)
        return 1

    clean = run(entry, build["binary"], run_dir, args.bus, 0,
                args.timeout_cycles, args.timeout_seconds, False)
    print(f"inject: {entry['test']}, {args.bus}, no injection: "
          f"{clean['outcome']}")
    if clean["outcome"] != "passed":
        print("inject: the entry does not pass without an injection, so "
              "nothing below would mean anything", file=sys.stderr)
        return 1

    results = []
    caught = silent = 0
    for index in range(1, args.count + 1):
        result = run(entry, build["binary"], run_dir, args.bus, index,
                     args.timeout_cycles, args.timeout_seconds, False)
        if result["outcome"] == "reference model crashed":
            pass
        elif not result["injected"]:
            # The run has fewer read responses than this index, so nothing was
            # corrupted; recording it as silent would be a lie.
            result["outcome"] = "not reached"
        elif result["outcome"] == "passed":
            silent += 1
        else:
            caught += 1
        results.append(result)
        print(f"  {index:>4}  {result['outcome']:<18} {result['detail']}",
              flush=True)

    control = []
    if args.no_cosim:
        print("\ninject: the same injections with IBEX_NO_COSIM=1")
        for index in range(1, args.count + 1):
            result = run(entry, build["binary"], run_dir, args.bus, index,
                         args.timeout_cycles, args.timeout_seconds, True)
            control.append(result)
            print(f"  {index:>4}  {result['outcome']}", flush=True)

    reached = [r for r in results if r["outcome"] not in
               ("not reached", "reference model crashed")]
    crashed = sum(1 for r in results
                  if r["outcome"] == "reference model crashed")
    document = {
        "entry": entry["test"], "bus": args.bus, "count": args.count,
        "clean": clean, "results": results, "control": control,
        "testbench": str(cpptb.BINARY),
    }
    (run_dir / "results.json").write_text(json.dumps(document, indent=2),
                                          encoding="utf-8")
    print(f"\n{caught} of {len(reached)} injections caught, {silent} silent")
    if crashed:
        print(f"{crashed} runs crashed the reference model rather than "
              f"reaching a verdict")
    if control:
        control_caught = sum(1 for r in control if r["outcome"] != "passed")
        print(f"with the reference model off: {control_caught} of "
              f"{len(control)} caught")
    print(f"written to {run_dir / 'results.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
