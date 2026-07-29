#!/usr/bin/env python3
"""Record what the UVM baseline drives, and replay it against the cpptb port.

Until this existed the two harnesses had only ever been compared on whether
they reached the same verdict on the same program. That is a statement about
two runs of a testbench. This is a statement about one run: the baseline writes
down everything its environment does that the DUT can see, this port drives the
same thing at the same pins, and the DUT's outputs are compared cycle for cycle.

    python3 replay.py                     # the default nine entries
    python3 replay.py --only add-01 --keep
    python3 replay.py --group riscv-tests --limit 4
    python3 replay.py --perturb 5000      # show the comparison is live

**The recording is made by the baseline and replayed here**, which is the
direction that tests the port against the reference. The other direction would
test the reference against the port, and would need a UVM sequence and driver
where the recording needs one `always @(posedge clk)` block.

`+core_ibex_record=<prefix>` is added to `ports/core_ibex_uvm` by one overlay in
its `build_tb.py`, and without the plusarg it does nothing at all, so the
binary that produces a recording is the binary the 912 was measured with. It
writes one file, `<prefix>.pins`, with one line per posedge of `clk`:

    C cycle in ctl instr_rdata data_rdata out instr_addr data_addr data_be \\
      data_wdata rvfi_order rvfi_pc rvfi_rd rvfi_rd_wdata

`in`, `ctl` and `out` pack the one-bit signals and the file's header names the
bits. The scrambling key and nonce are 192 bits and change rarely, so they get a
`K` line only when they change and the reader holds the last value. About 105
bytes a cycle.

Everything in the recording is read in the Active region of a posedge, which is
the value the design samples at that edge and the value a clocking-block input
sees. So a replay that drives those inputs at its drive point, half a cycle
earlier, presents the design with the same stimulus, and one that reads the
outputs at the edge itself reads what the recording read.

Three things are checked per entry:

  * every cycle of the recording is replayed and all thirteen output fields
    match on every one of them;
  * this port's co-simulation scoreboard runs on the baseline's stimulus, and
    the number of instructions it steps Spike through is compared against the
    number the baseline's own scoreboard reported. That is the same check made
    twice by two different pieces of code on the same run;
  * the recording carries only integrity bits that are the SECDED encoding of
    the response data or its inverse, which is what the cpptb wrapper can
    drive. Anything else would be a response this port cannot reproduce, and it
    is reported rather than approximated.

Standard library only, matching the other tools here.
"""

from __future__ import annotations

import argparse
import importlib.util
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


def _load(name: str, path: Path):
    """Import a module under a name of our choosing.

    Both runners are called run_directed.py, so a plain `import` would give the
    same module twice: whichever is first on sys.path. This port's runner
    imports the baseline's itself, so loading it under its own name and then
    taking `.uvm` off it gets both, and gets exactly the objects that runner
    uses rather than a second copy of them.
    """
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


cpptb = _load("cpptb_run_directed", HERE / "run_directed.py")
uvm = cpptb.uvm

BUILD = HERE / "build"
RUNS = BUILD / "replay"

# A spread over the three groups, chosen short: a recording is about 105 bytes a
# cycle, so a 30,000-cycle entry is 3 MB and one of the longer ePMP entries would
# be a hundred times that. The point is cycle-for-cycle agreement, and these
# reach 200,000 cycles of it between them.
DEFAULT_ENTRIES = [
    # riscv-tests: the program's own verdict
    "empty",
    "mcounteren_test",
    "access_pmp_overlap",
    # riscv-arch-tests: checked by the cosim and nothing else
    "add-01",
    "Fencei",
    "cli-01",
    # epmp-tests: PMP configuration sweeps, where fetch errors are the point
    "test_pmp_csr_1_lock00_rlb0_mmwp0_mml0_sec_00",
    "test_pmp_ok_1_u0_rw00_x0_l0_match0_mmwp0_mml0",
    "pmp_mseccfg_test_rlb1_l0_0_u0",
]

BASELINE_MATCHED = re.compile(r"Co-simulation matched\s+(\d+) instructions")
REPLAY_MATCHED = re.compile(
    r"^cpptb-core-ibex replay: (\d+) of (\d+) recorded cycles matched$",
    re.MULTILINE)
REPLAY_INTG = re.compile(r"^cpptb-core-ibex replay: (\d+) responses carried",
                         re.MULTILINE)


def baseline_environment() -> dict[str, str]:
    return uvm.environment()


def cpptb_environment() -> dict[str, str]:
    return cpptb.environment()


def record(entry: dict, binary: Path, directory: Path, prefix: Path,
           cycles: int, seconds: int, record_max: int) -> dict:
    """Run the UVM baseline with the pin recording on."""
    testbench = UVM_PORT / "build" / "obj_opentitan" / "core_ibex_tb"
    command = [str(testbench), f"+UVM_TESTNAME={entry['rtl_test']}",
               f"+bin={binary}", f"+signature_addr={uvm.SIGNATURE_ADDR}",
               f"+timeout_in_cycles={cycles}",
               f"+core_ibex_record={prefix}"]
    if record_max:
        command.append(f"+core_ibex_record_max={record_max}")
    started = time.monotonic()
    try:
        completed = subprocess.run(command, cwd=directory,
                                   env=baseline_environment(), text=True,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=seconds)
        output = completed.stdout
    except subprocess.TimeoutExpired as expired:
        output = expired.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
    elapsed = time.monotonic() - started
    (directory / "baseline.log").write_text(output, encoding="utf-8")

    matched = BASELINE_MATCHED.search(output)
    # A run that ends in a fraction of a second did not simulate anything: it is
    # what a missing z3 looks like, and it has been read as a result on this
    # project before.
    if elapsed < 1.0:
        return {"ok": False,
                "detail": f"the baseline exited after {elapsed:.2f}s; z3 on "
                          f"PATH?", "seconds": elapsed}
    if not Path(f"{prefix}.pins").is_file():
        return {"ok": False, "detail": "no recording was written",
                "seconds": elapsed}
    return {"ok": True,
            "baseline_matched": int(matched.group(1)) if matched else None,
            "baseline_passed": "--- RISC-V UVM TEST PASSED ---" in output,
            "bytes": Path(f"{prefix}.pins").stat().st_size,
            "seconds": elapsed}


def replay(entry: dict, binary: Path, directory: Path, prefix: Path,
           seconds: int, perturb: int) -> dict:
    env = cpptb_environment()
    env["CPPTB_TEST"] = entry["rtl_test"]
    env["IBEX_BIN"] = str(binary)
    env["IBEX_REPLAY"] = str(prefix)
    env["IBEX_SIGNATURE_ADDR"] = uvm.SIGNATURE_ADDR
    if perturb:
        env["IBEX_REPLAY_PERTURB"] = str(perturb)

    started = time.monotonic()
    try:
        completed = subprocess.run([str(cpptb.BINARY)], cwd=directory, env=env,
                                   text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=seconds)
        output = completed.stdout
    except subprocess.TimeoutExpired as expired:
        output = expired.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
    elapsed = time.monotonic() - started
    (directory / "replay.log").write_text(output, encoding="utf-8")

    matched = REPLAY_MATCHED.search(output)
    counters = cpptb.counters(output)
    intg = REPLAY_INTG.search(output)
    return {
        "cycles_matched": int(matched.group(1)) if matched else 0,
        "cycles_recorded": int(matched.group(2)) if matched else 0,
        "cosim_matched": counters.get("cosim_matched", 0),
        "retired": counters.get("retired", 0),
        "intg_unreproducible": int(intg.group(1)) if intg else 0,
        "seconds": elapsed,
        "output": output,
    }


def handle(entry: dict, run: Path, march: str, cycles: int, seconds: int,
           record_max: int, perturb: int, keep: bool) -> dict:
    directory = run / entry["test"]
    result = {"test": entry["test"], "group": entry["config"],
              "rtl_test": entry["rtl_test"],
              "dir": directory.relative_to(BUILD).as_posix()}

    build = uvm.build_one(entry, march, directory, False, False)
    if not build["built"]:
        return {**result, "verdict": "build failed", "detail": build["detail"]}

    prefix = directory / entry["test"]
    recording = record(entry, build["binary"], directory, prefix, cycles,
                       seconds, record_max)
    result["record_seconds"] = recording["seconds"]
    if not recording["ok"]:
        return {**result, "verdict": "no recording",
                "detail": recording["detail"]}
    result["recording_bytes"] = recording["bytes"]
    result["baseline_matched"] = recording["baseline_matched"]

    replayed = replay(entry, build["binary"], directory, prefix, seconds,
                      perturb)
    output = replayed.pop("output")
    result.update(replayed)

    if not keep:
        pins = Path(f"{prefix}.pins")
        if pins.is_file():
            pins.unlink()

    if replayed["cycles_recorded"] == 0:
        result["verdict"] = "no replay"
        result["detail"] = "the replay printed no cycle count"
    elif replayed["cycles_matched"] != replayed["cycles_recorded"]:
        result["verdict"] = "divergence"
        first = next((line for line in output.splitlines()
                      if "replay divergence" in line), "")
        result["detail"] = first[:80]
    elif replayed["intg_unreproducible"]:
        result["verdict"] = "integrity not reproducible"
        result["detail"] = (f"{replayed['intg_unreproducible']} responses "
                            f"carried integrity bits the wrapper cannot drive")
    elif (recording["baseline_matched"] is not None and
          recording["baseline_matched"] != replayed["cosim_matched"]):
        result["verdict"] = "scoreboard disagreement"
        result["detail"] = (f"the baseline stepped Spike "
                            f"{recording['baseline_matched']} times, this port "
                            f"{replayed['cosim_matched']}")
    else:
        result["verdict"] = "matched"
        result["detail"] = ""
    return result


def open_run(name: str) -> Path:
    run = RUNS / (name or time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()))
    if run.exists():
        raise uvm.DirectedError(
            f"{run} already exists; pass --run-name for a different one")
    run.mkdir(parents=True)
    return run


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--only", action="append", default=[],
                        help="record and replay these entries by name")
    parser.add_argument("--group", action="append", default=[])
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--timeout-cycles", type=int, default=5000000)
    parser.add_argument("--timeout-seconds", type=int, default=900)
    parser.add_argument("--record-max", type=int, default=400000,
                        help="stop recording after this many cycles; 0 for the "
                             "whole run. A recording is about 105 bytes a "
                             "cycle.")
    parser.add_argument("--perturb", type=int, default=0,
                        help="move one bit of the first instruction fetched at "
                             "or after this cycle, which is how the comparison "
                             "is shown to be live")
    parser.add_argument("--keep", action="store_true",
                        help="keep the .pins recordings; they are deleted by "
                             "default because they are large")
    parser.add_argument("--run-name", default="")
    args = parser.parse_args(argv)

    try:
        entries = uvm.merge(*uvm.parse_testlist(uvm.TESTLIST))
        parameters, defines = uvm.build_tb.config_parameters("opentitan")
        march = uvm.isa_march(defines)
    except (uvm.DirectedError, uvm.build_tb.BuildError) as error:
        print(f"replay: {error}", file=sys.stderr)
        return 1

    by_name = {entry["test"]: entry for entry in entries}
    if args.only:
        missing = [name for name in args.only if name not in by_name]
        if missing:
            print(f"replay: no such entries: {', '.join(missing)}",
                  file=sys.stderr)
            return 1
        chosen = [by_name[name] for name in args.only]
    elif args.group:
        chosen = [e for e in entries if e["config"] in args.group]
    else:
        chosen = [by_name[name] for name in DEFAULT_ENTRIES if name in by_name]
        if len(chosen) != len(DEFAULT_ENTRIES):
            print("replay: the default entry list names something the testlist "
                  "does not have", file=sys.stderr)
            return 1
    if args.limit:
        chosen = chosen[:args.limit]

    testbench = UVM_PORT / "build" / "obj_opentitan" / "core_ibex_tb"
    if not testbench.is_file():
        print(f"replay: no UVM baseline at {testbench}\n"
              f"build it with: python3 {UVM_PORT / 'build_tb.py'} "
              f"--config opentitan", file=sys.stderr)
        return 1
    if not cpptb.BINARY.is_file():
        print(f"replay: no cpptb testbench at {cpptb.BINARY}", file=sys.stderr)
        return 1

    try:
        run = open_run(args.run_name)
    except uvm.DirectedError as error:
        print(f"replay: {error}", file=sys.stderr)
        return 1

    header = {
        "run": run.name,
        "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "entries": len(chosen),
        "record_max": args.record_max,
        "perturb": args.perturb,
        "baseline": str(testbench),
        "testbench": str(cpptb.BINARY),
        "command": shlex.join([sys.executable, str(HERE / "replay.py")]
                              + (argv if argv is not None else sys.argv[1:])),
    }

    print(f"replay: {len(chosen)} entries, recording from {testbench.name} and "
          f"replaying on {cpptb.BINARY.name}")
    results = []
    # One at a time: the baseline is the slow half and running several at once
    # would make the recording times meaningless.
    for entry in chosen:
        result = handle(entry, run, march, args.timeout_cycles,
                        args.timeout_seconds, args.record_max, args.perturb,
                        args.keep)
        results.append(result)
        detail = f"  {result['detail']}" if result.get("detail") else ""
        print(f"  {result['test']:<46} {result['verdict']:<12} "
              f"{result.get('cycles_matched', 0):>8} cycles  "
              f"{result.get('cosim_matched', 0):>7} insns{detail}", flush=True)

    header["finished"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    document = {**header, "results": results}
    (run / "results.json").write_text(json.dumps(document, indent=2),
                                      encoding="utf-8")

    matched = [r for r in results if r["verdict"] == "matched"]
    cycles = sum(r.get("cycles_matched", 0) for r in results)
    insns = sum(r.get("cosim_matched", 0) for r in results)
    print(f"\n{len(matched)} of {len(results)} entries matched: "
          f"{cycles:,} cycles and {insns:,} instructions of the baseline's own "
          f"runs")
    print(f"written to {run / 'results.json'}")
    return 0 if len(matched) == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
