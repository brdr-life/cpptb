#!/usr/bin/env python3
"""Run Ibex's own co-simulation tests, under both harnesses, across configurations.

This is the flow Ibex's CI runs, reproduced. `.github/actions/ibex-rtl-ci-steps`
builds `lowrisc:ibex:ibex_simple_system_cosim` for each of six configurations and
runs four programs through it:

    ./ci/run-cosim-test.sh --skip-pass-check CoreMark  .../coremark.elf
    ./ci/run-cosim-test.sh --skip-pass-check pmp_smoke .../pmp_smoke_test.elf
    ./ci/run-cosim-test.sh dit_test                    .../dit_test.elf
    ./ci/run-cosim-test.sh dummy_instr_test            .../dummy_instr_test.elf

Every program runs under both harnesses, as everywhere else here, and the pass
criteria are upstream's, taken from that script rather than invented:

  - the process must exit zero
  - `FAILURE` must not appear in the log
  - `PASS` must appear, except where CI passes --skip-pass-check

A program that fails must fail on both harnesses. One side failing where the
other does not is a defect in this port, and is reported separately from a
program that fails on both, which is a property of the core.

    python3 run_cosim_programs.py
    python3 run_cosim_programs.py --config cosim-opentitan
    python3 run_cosim_programs.py --program dit_test --verbose

Standard library only, matching the other tools here.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from build_tests import CONFIGS, RAM_BASE, toolchain

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
IBEX = ROOT / "deps" / "ibex"
BUILD = HERE / "build" / "cosim-programs"

TRACER_OFF = "+ibex_tracer_enable=0"
LOG_NAME = "ibex_simple_system.log"

# CoreMark needs 40.7 million cycles; the others are far shorter. The cpptb
# testbench takes this from the environment, and the upstream harness has no
# cycle limit at all, so only run_cosim_programs' own timeout bounds it there.
CYCLE_LIMIT = 80_000_000
RUN_TIMEOUT_S = 600


@dataclass(frozen=True)
class Program:
    name: str
    elf: Path
    # CI passes --skip-pass-check for CoreMark and pmp_smoke, so neither is
    # expected to print PASS. Copied from the workflow, not chosen here.
    skip_pass_check: bool
    make: str  # the directory to build it in, and any flags CI passes
    # The parameter a configuration must set for CI to run this program at all.
    # Without this the security tests are run on cores that do not implement
    # the features they test, and fail -- correctly, but pointlessly, and not
    # as upstream runs them.
    requires: str | None = None


PROGRAMS = [
    # Built with SUPPRESS_PCOUNT_DUMP=1 because Spike's performance counters do
    # not match Ibex's; upstream's cosim README and CI both require it.
    Program("CoreMark", IBEX / "examples/sw/benchmarks/coremark/coremark.elf",
            True, "examples/sw/benchmarks/coremark SUPPRESS_PCOUNT_DUMP=1"),
    Program("pmp_smoke",
            IBEX / "examples/sw/simple_system/pmp_smoke_test/pmp_smoke_test.elf",
            True, "examples/sw/simple_system/pmp_smoke_test",
            requires="PMPEnable"),
    Program("dit_test",
            IBEX / "examples/sw/simple_system/dit_test/dit_test.elf",
            False, "examples/sw/simple_system/dit_test",
            requires="SecureIbex"),
    Program("dummy_instr_test",
            IBEX / "examples/sw/simple_system/dummy_instr_test/dummy_instr_test.elf",
            False, "examples/sw/simple_system/dummy_instr_test",
            requires="SecureIbex"),
]

def config_has(ibex_config: str, field: str) -> bool:
    """Whether a configuration sets a parameter, asked the way CI asks.

    CI gates these programs on exactly this, so running them where it does not
    means running the security tests against cores that do not implement the
    security features:

        if ./util/ibex_config.py $CFG query_fields SecureIbex | grep -q 'SecureIbex=1'; then
    """
    environment = dict(os.environ)
    pylibs = ROOT / "deps/.tools/pylibs"
    if pylibs.is_dir():
        environment["PYTHONPATH"] = f"{pylibs}:{environment.get('PYTHONPATH', '')}"
    result = subprocess.run(
        [sys.executable, "util/ibex_config.py", ibex_config, "query_fields",
         field],
        cwd=IBEX, env=environment, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, check=False)
    return f"{field}=1" in result.stdout


MISMATCH_RE = re.compile(r"Co-simulation mismatch at time\s*(\S+)")
MATCHED_RE = re.compile(r"matched (\d+) instructions")


def vmem_for(program: Program) -> Path:
    """The cpptb harness loads a VMEM; upstream loads the ELF directly.

    Converted here rather than committed, because these ELFs are build output of
    the fetched tree. objcopy emits from the image's lowest address, which the
    Simple System linker script puts at the base of RAM.
    """
    BUILD.mkdir(parents=True, exist_ok=True)
    vmem = BUILD / f"{program.elf.stem}.vmem"
    raw = BUILD / f"{program.elf.stem}.bin"
    subprocess.run([str(toolchain("objcopy")), "-O", "binary", "--gap-fill", "0",
                    str(program.elf), str(raw)], check=True)
    sys.path.insert(0, str(ROOT))
    from bin2vmem import convert
    vmem.write_text(convert(raw.read_bytes(), offset=0), encoding="utf-8")
    raw.unlink()
    return vmem


def run_once(mode: str, program: Program, binary: Path, vmem: Path,
             workdir: Path) -> dict:
    log = workdir / LOG_NAME
    log.unlink(missing_ok=True)

    environment = None
    if mode == "cpptb":
        command = [str(binary), TRACER_OFF]
        environment = dict(os.environ)
        environment.update({"ACT_NAME": program.name,
                            "ACT_FIRMWARE": str(vmem),
                            "ACT_CYCLE_LIMIT": str(CYCLE_LIMIT)})
    else:
        command = [str(binary), TRACER_OFF, f"--meminit=ram,{program.elf}"]

    started = time.perf_counter()
    try:
        completed = subprocess.run(command, cwd=workdir, text=True,
                                   env=environment, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, check=False,
                                   timeout=RUN_TIMEOUT_S)
        returncode, stdout = completed.returncode, completed.stdout
    except subprocess.TimeoutExpired:
        returncode, stdout = -1, ""
    wall_ms = (time.perf_counter() - started) * 1000.0

    text = log.read_text(encoding="utf-8", errors="replace") if log.is_file() else ""
    matched = MATCHED_RE.search(stdout)
    return {"mode": mode, "wall_ms": wall_ms, "returncode": returncode,
            "failure": "FAILURE" in text, "pass": "PASS" in text,
            "mismatch": bool(MISMATCH_RE.search(stdout)),
            "matched": int(matched.group(1)) if matched else None}


def verdict(program: Program, sample: dict) -> list[str]:
    """Upstream's own criteria, from ci/run-cosim-test.sh."""
    problems = []
    if sample["returncode"] != 0:
        problems.append(f"exit {sample['returncode']}")
    if sample["failure"]:
        problems.append("FAILURE in log")
    if not program.skip_pass_check and not sample["pass"]:
        problems.append("no PASS in log")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    cosim = sorted(n for n, c in CONFIGS.items() if c.cosim)
    parser.add_argument("--config", action="append", choices=cosim,
                        help="repeatable; default is every configuration")
    parser.add_argument("--program", action="append",
                        choices=[p.name for p in PROGRAMS])
    parser.add_argument("--json", type=Path)
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--build-software", action="store_true",
                        help="build the programs first, as CI does")
    args = parser.parse_args(argv)

    configs = args.config or cosim
    programs = [p for p in PROGRAMS
                if not args.program or p.name in args.program]

    if args.build_software:
        for program in programs:
            subprocess.run(["make", "-C", *program.make.split()], cwd=IBEX,
                           check=True, stdout=subprocess.DEVNULL)

    missing = [p.name for p in programs if not p.elf.is_file()]
    if missing:
        print(f"run_cosim_programs: not built: {', '.join(missing)}\n"
              f"run with --build-software, or see ibex-coverage.md",
              file=sys.stderr)
        return 1

    vmems = {p.name: vmem_for(p) for p in programs}
    work = BUILD / "run"
    shutil.rmtree(work, ignore_errors=True)
    dirs = {m: work / m for m in ("upstream", "cpptb")}
    for path in dirs.values():
        path.mkdir(parents=True)

    print(f"{len(configs)} configuration(s) x {len(programs)} program(s), "
          f"both harnesses\n")
    results, skipped, gated = [], [], []
    for name in configs:
        config = CONFIGS[name]
        binaries = {"upstream": ROOT / config.upstream_binary,
                    "cpptb": ROOT / config.cpptb_binary}
        absent = [k for k, v in binaries.items() if not v.is_file()]
        if absent:
            skipped.append((name, ", ".join(absent)))
            continue

        for program in programs:
            if program.requires and not config_has(config.ibex_config,
                                                   program.requires):
                # CI skips it here too, and says so.
                gated.append((name, program.name, program.requires))
                continue
            samples = {m: run_once(m, program, binaries[m], vmems[program.name],
                                   dirs[m])
                       for m in ("upstream", "cpptb")}
            up, cp = verdict(program, samples["upstream"]), verdict(program, samples["cpptb"])
            # Same split as run_suite.py: a difference between the harnesses is
            # this port's problem, a shared failure is the core's or the test's.
            disagreement = (sorted(up) != sorted(cp)
                            or samples["upstream"]["mismatch"]
                            != samples["cpptb"]["mismatch"])
            record = {"config": name, "ibex_config": config.ibex_config,
                      "program": program.name,
                      "disagreement": disagreement,
                      "shared_failure": bool(up) and not disagreement,
                      "upstream": samples["upstream"], "cpptb": samples["cpptb"]}
            results.append(record)

            status = ("ok" if not (up or cp)
                      else f"both: {'; '.join(up)}" if not disagreement
                      else f"DIFFER upstream[{'; '.join(up) or 'ok'}] "
                           f"cpptb[{'; '.join(cp) or 'ok'}]")
            if args.verbose or up or cp:
                print(f"  {name:<36} {program.name:<17} "
                      f"{samples['upstream']['wall_ms']:8.0f} ms "
                      f"{samples['cpptb']['wall_ms']:8.0f} ms  {status}")

    for name, absent in skipped:
        print(f"  {name:<36} skipped: no {absent} binary")
    if gated and args.verbose:
        for name, program, field in gated:
            print(f"  {name:<36} {program:<17} not run: {field} is 0, as in CI")

    differ = [r for r in results if r["disagreement"]]
    shared = [r for r in results if r["shared_failure"]]
    ok = len(results) - len(differ) - len(shared)
    checked = sum(r["cpptb"]["matched"] or 0 for r in results)

    print(f"\n{ok}/{len(results)} passed on both harnesses")
    if gated:
        print(f"{len(gated)} not run because the configuration lacks the "
              f"feature, which is what CI does")
    if checked:
        print(f"{checked:,} instructions checked against Spike by the cpptb side")
    if shared:
        print(f"{len(shared)} failed on both harnesses (a property of the core "
              f"or the program, not of the port)")
    if differ:
        print(f"{len(differ)} differed between the harnesses -- a port defect",
              file=sys.stderr)

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(
            {"passed": ok, "shared_failures": len(shared),
             "disagreements": len(differ), "instructions_checked": checked,
             "results": results}, indent=2), encoding="utf-8")
        print(f"wrote {args.json}")
    return 1 if differ else 0


if __name__ == "__main__":
    raise SystemExit(main())
