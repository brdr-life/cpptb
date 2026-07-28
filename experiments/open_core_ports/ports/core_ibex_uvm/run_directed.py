#!/usr/bin/env python3
"""Build and run the core_ibex directed tests.

The test list is upstream's own `directed_tests/directed_testlist.yaml`: 944
entries over three `- config:` groups. Unlike the riscv-dv testlist there is no
generator involved -- each entry names a hand-written C or assembly source, and
upstream's `scripts/compile_test.py` turns it into a binary with two commands:

    $RISCV_GCC <gcc_opts> -I<includes> -T<ld_script> -o test.o <test_srcs>
    $RISCV_OBJCOPY -O binary test.o test.bin

This does the same thing, with the same working directory upstream uses
(`dv/uvm/core_ibex`, which is what the relative `-I` paths inside `gcc_opts`
are written against), and then runs the result under the entry's `rtl_test`.

    python3 build_tb.py --config opentitan
    python3 run_directed.py --config opentitan --group riscv-tests

`+bin` is a flat binary, not an ELF: the testbench loads it byte by byte from
`BOOT_ADDR`, and an ELF leaves memory at zero. See README.md.

Two things about the results are worth stating before reading them.

First, every one of the three configs in the directed testlist carries
`rtl_params: {PMPEnable: 1}`, so no entry in the file is applicable to a build
of the `small` configuration. `--config small` reports all 944 as inapplicable
rather than running them, which is what upstream's `filter_tests_by_config`
does with the same data.

Second, the three groups check themselves to three different depths, and only
one of them reaches the harness on its own.

  riscv-tests (93)       signal TEST_PASS or TEST_FAIL, so a pass is the
                         program's own verdict.
  epmp-tests (744)       compute a verdict and then throw it away: syscalls.c's
                         `tohost_exit(code)` sends TEST_PASS whatever `code`
                         is. This reads the code back out of the execution
                         trace, so those do get a verdict here; upstream's
                         harness cannot fail one of them.
  riscv-arch-tests (107) signal TEST_PASS unconditionally from `RVMODEL_HALT`
                         and leave the checking to a signature comparison
                         against a reference that this flow, like upstream's,
                         does not do.

For the last of those a pass means "ran to completion with the RVFI cosim
against Spike staying quiet", which is a real check -- every retired
instruction is compared against the ISS -- but it is not the test's own.

Standard library only, matching the other tools here.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import fnmatch
import json
import os
import re
import shlex
import subprocess
import sys
import threading
import time
from pathlib import Path

import build_tb

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
IBEX = ROOT / "deps" / "ibex"
CORE_IBEX = IBEX / "dv" / "uvm" / "core_ibex"
TESTLIST = CORE_IBEX / "directed_tests" / "directed_testlist.yaml"
BUILD = HERE / "build"
TOOLCHAIN = ROOT / "deps" / "riscv_gcc15" / "bin"
GCC = TOOLCHAIN / "riscv-none-elf-gcc"
OBJCOPY = TOOLCHAIN / "riscv-none-elf-objcopy"
SPIKE_LIB = ROOT / "deps" / "spike_cosim" / "install" / "lib"
TOOLS_LIB = ROOT / "deps" / ".tools" / "root" / "usr" / "lib" / "x86_64-linux-gnu"
TOOLS_BIN = ROOT / "deps" / ".tools" / "root" / "usr" / "bin"

SIGNATURE_ADDR = "8ffffffc"

# The keys the schema in scripts/directed_test_schema.py defines. Anything else
# in the file is a change upstream that this reader has not been taught about,
# and is an error rather than something to skip.
CONFIG_KEYS = {"config", "rtl_test", "rtl_params", "timeout_s",
               "gcc_opts", "ld_script", "includes"}
TEST_KEYS = CONFIG_KEYS | {"test", "desc", "test_srcs", "iterations"}
# The three the schema resolves relative to the testlist file itself.
PATH_KEYS = ("ld_script", "includes", "test_srcs")


class DirectedError(Exception):
    """Something about the test list or the toolchain does not hold."""


OVERLAY_LOCK = threading.Lock()


# ---------------------------------------------------------------------------
# Divergences from upstream's compile, each recorded against the test that
# carries it so a green result cannot imply more than it should.
# ---------------------------------------------------------------------------

# Exact-text substitutions into a config's `gcc_opts`, by config name. Each one
# must match exactly once or the run stops: an upstream edit to these options
# has to be looked at rather than silently ignored, which is the same rule
# build_tb.py applies to the SystemVerilog overlays.
GCC_OPTS_PATCHES: dict[str, list[tuple[str, str, str]]] = {
    "epmp-tests": [
        (
            "-march=rv32imc",
            "-march=rv32imc_zicsr_zifencei",
            "gcc 15 assembles no CSR or fence.i instruction unless Zicsr and "
            "Zifencei are named; they were part of the base ISA when this "
            "option was written. Same instruction set, spelled for a newer "
            "assembler.",
        ),
    ],
}

# The riscv-tests and riscv-arch-tests configs name no -march at all and take
# whatever the toolchain defaults to. lowRISC's default is rv32imc (rv32imcb
# for the bitmanip toolchain); this toolchain defaults to
# rv32imac_zmmul_zaamo_zalrsc_zca, which has the A extension Ibex does not
# implement. So an -march is supplied, derived from the Ibex configuration
# being built rather than fixed, and recorded here.
MARCH_NOTE = ("this config names no -march, so one derived from the Ibex "
              "configuration is supplied in place of the toolchain default, "
              "which here would include the A extension Ibex has not got")

# Extra sources appended to a config's compile. See the file for why.
EXTRA_SOURCES: dict[str, list[tuple[Path, str]]] = {
    "epmp-tests": [
        (HERE / "shims" / "emutls_malloc.c",
         "syscalls.c uses __thread, this toolchain emulates TLS, and libgcc's "
         "emulation calls malloc under -nostdlib; see shims/emutls_malloc.c"),
    ],
}

# What a group needs from the RTL beyond the `rtl_params` the testlist states.
# The testlist records only PMPEnable, so a bitmanip test on a build without
# bitmanip would show up as a failed assembly rather than as inapplicable.
# Keyed on a fragment of the entry's test_srcs path.
EXTRA_REQUIREMENTS: list[tuple[str, str, str]] = [
    ("riscv-arch-tests/riscv-test-suite/rv32i_m/B/src",
     "RV32B",
     "the B-extension arch tests assemble to bitmanip instructions, which a "
     "build with RV32BNone cannot execute"),
]

# Exact-text substitutions into a linker script, by config name. The patched
# copy goes under build/, never into deps/, and a replacement that stops
# matching fails the run rather than quietly doing nothing -- the same rule
# build_tb.py applies to the SystemVerilog overlays.
#
# `mseccfg_test.ld` is stale with respect to the ePMP sources beside it. Every
# generated test says
#
#     #define TEST_MEM_START 0x80200000
#     #define TEST_MEM_END   0x80240000
#
# and programs its PMP regions from those constants, but the linker script
# places TEST_MEM at 0x0020_0000 and M_MEM at 0x0010_0000. The flat binary
# starts at the lowest section, so loading it at BOOT_ADDR puts _start at
# 0x8000_0080 -- right -- and TEST_MEM at 0x8010_0000, a megabyte below where
# the program's own PMP entries say it is. lowRISC regenerated the C for Ibex's
# memory map and left the script on Spike's.
#
# The effect is not subtle and it is not a cosim mismatch, because Spike is
# given the same binary and agrees with the DUT about every instruction. It is
# the program's own check failing: of a 13-entry sample, the four failures were
# all in the two families that execute from or load out of TEST_MEM, and all
# four pass once the script is corrected.
#
# Rebasing the script rather than patching the 744 generated C files keeps the
# stimulus exactly as upstream generated it. `--stock-ld` runs the script as
# vendored, for anyone who wants to see the failures.
LD_SCRIPT_PATCHES: dict[str, list[tuple[str, str]]] = {
    "epmp-tests": [
        ("M_MEM (AX)  : ORIGIN = 0x100000, LENGTH = 1M",
         "M_MEM (AX)  : ORIGIN = 0x80000000, LENGTH = 1M"),
        ("RESERVED  : ORIGIN = 0x000000, LENGTH = 1M",
         "RESERVED  : ORIGIN = 0x7ff00000, LENGTH = 1M"),
        ("TEST_MEM (AX)  : ORIGIN = 0x200000, LENGTH = 256K",
         "TEST_MEM (AX)  : ORIGIN = 0x80200000, LENGTH = 256K"),
        ("U_MEM (AX)  : ORIGIN = 0x240000, LENGTH = 64K",
         "U_MEM (AX)  : ORIGIN = 0x80240000, LENGTH = 64K"),
        ("    . = 0x100000;\n    __global_pointer$ = 0x140000;",
         "    . = 0x80000000;\n    __global_pointer$ = 0x80140000;"),
        ("    _end = 0x180000;",
         "    _end = 0x80180000;"),
    ],
}

LD_SCRIPT_NOTE = (
    "linker script rebased to Ibex's memory map: the vendored mseccfg_test.ld "
    "puts TEST_MEM at 0x0020_0000 while the ePMP sources beside it program "
    "their PMP regions at 0x8020_0000. Pass --stock-ld for upstream's")


# ---------------------------------------------------------------------------
# Reading the test list
# ---------------------------------------------------------------------------

def parse_testlist(path: Path) -> tuple[list[dict], list[dict]]:
    """(configs, tests) from the directed testlist.

    The file is a flat YAML list of `- config:` and `- test:` mappings, with
    two shapes that need care: `gcc_opts` is a folded plain scalar continued
    over several deeper-indented lines, and `rtl_params` is a nested mapping.
    A block key with no value on its own line is the nested mapping; anything
    else continues a scalar. Written out rather than taken from PyYAML because
    every tool here is standard library only.
    """
    if not path.is_file():
        raise DirectedError(f"no directed testlist at {path}")

    blocks: list[dict] = []
    block: dict | None = None
    key: str | None = None

    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.rstrip()
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        indent = len(line) - len(line.lstrip())

        if stripped.startswith("- "):
            block = {}
            blocks.append(block)
            stripped = stripped[2:]
            indent = 2
        if block is None:
            raise DirectedError(f"{path.name}:{number}: content before the "
                                f"first list item")

        if indent <= 2:
            name, separator, value = stripped.partition(":")
            if not separator:
                raise DirectedError(f"{path.name}:{number}: no key in {stripped!r}")
            key = name.strip()
            value = value.strip()
            # `>` opens a folded block; an empty value opens a nested mapping.
            block[key] = "" if value == ">" else ({} if value == "" else value)
        else:
            if key is None:
                raise DirectedError(f"{path.name}:{number}: continuation with "
                                    f"no key")
            if isinstance(block[key], dict):
                name, separator, value = stripped.partition(":")
                if not separator:
                    raise DirectedError(f"{path.name}:{number}: no key in "
                                        f"{stripped!r}")
                block[key][name.strip()] = value.strip()
            else:
                block[key] = f"{block[key]} {stripped}".strip()

    configs = [b for b in blocks if "test" not in b]
    tests = [b for b in blocks if "test" in b]

    for entry in configs:
        unknown = set(entry) - CONFIG_KEYS
        if unknown:
            raise DirectedError(f"{path.name}: config {entry.get('config')!r} "
                                f"has keys this reader does not know: "
                                f"{sorted(unknown)}")
    for entry in tests:
        unknown = set(entry) - TEST_KEYS
        if unknown:
            raise DirectedError(f"{path.name}: test {entry.get('test')!r} has "
                                f"keys this reader does not know: "
                                f"{sorted(unknown)}")
    if not configs or not tests:
        raise DirectedError(f"{path.name}: read {len(configs)} configs and "
                            f"{len(tests)} tests")
    return configs, tests


def merge(configs: list[dict], tests: list[dict]) -> list[dict]:
    """Join each test onto its config, test keys winning, as the schema does."""
    by_name = {}
    for entry in configs:
        if "config" not in entry:
            raise DirectedError("a config block has no `config` key")
        by_name[entry["config"]] = entry

    merged = []
    for test in tests:
        name = test.get("config")
        if name not in by_name:
            raise DirectedError(f"test {test['test']!r} names the config "
                                f"{name!r}, which the file does not define")
        entry = {**by_name[name], **test}
        for path_key in PATH_KEYS:
            resolved = (TESTLIST.parent / entry[path_key]).resolve()
            if not resolved.exists():
                raise DirectedError(f"test {entry['test']!r}: {path_key} "
                                    f"{entry[path_key]} does not exist "
                                    f"({resolved})")
            entry[path_key] = resolved
        merged.append(entry)

    names = [entry["test"] for entry in merged]
    if len(set(names)) != len(names):
        duplicates = sorted({n for n in names if names.count(n) > 1})
        raise DirectedError(f"duplicate test names in the testlist: {duplicates}")
    return merged


# ---------------------------------------------------------------------------
# The Ibex configuration
# ---------------------------------------------------------------------------

def isa_march(defines: dict[str, str]) -> str:
    """The -march for a build, following ibex_cmd.get_isas_for_config().

    That function is what upstream uses to pick the toolchain ISA for a
    configuration. Zicsr and Zifencei are named explicitly because gcc 15 no
    longer folds them into the base ISA; the bitmanip subsets are the standard
    ones from upstream's own mapping, the `XZb*` entries in it being draft
    extensions no released gcc has.
    """
    bitmanip = {
        "ibex_pkg::RV32BNone": [],
        "ibex_pkg::RV32BBalanced": ["zba", "zbb", "zbs"],
        "ibex_pkg::RV32BOTEarlGrey": ["zba", "zbb", "zbc", "zbs"],
        "ibex_pkg::RV32BFull": ["zba", "zbb", "zbc", "zbs"],
    }
    rv32b = defines.get("IBEX_CFG_RV32B")
    if rv32b not in bitmanip:
        raise DirectedError(f"unknown RV32B value {rv32b!r} in the "
                            f"configuration")
    multiplier = defines.get("IBEX_CFG_RV32M") != "ibex_pkg::RV32MNone"
    base = "rv32{}{}c".format("e" if defines.get("RV32E") == "1" else "i",
                              "m" if multiplier else "")
    return "_".join([base, "zicsr", "zifencei", *bitmanip[rv32b]])


def applicability(entry: dict, parameters: dict[str, str],
                  defines: dict[str, str]) -> str | None:
    """Why this entry does not apply to the built configuration, or None.

    The same test upstream's `ibex_cmd.filter_tests_by_config` does, plus the
    requirements in EXTRA_REQUIREMENTS that the testlist does not state.
    """
    for name, wanted in (entry.get("rtl_params") or {}).items():
        built = parameters.get(name)
        if built is None:
            return f"the build does not set {name}"
        if str(built) != str(wanted):
            return f"needs {name}={wanted}, the build has {name}={built}"

    source = entry["test_srcs"].as_posix()
    for fragment, requirement, reason in EXTRA_REQUIREMENTS:
        if fragment not in source:
            continue
        if requirement == "RV32B":
            if defines.get("IBEX_CFG_RV32B") == "ibex_pkg::RV32BNone":
                return f"needs bitmanip: {reason}"
        else:
            raise DirectedError(f"unhandled extra requirement {requirement!r}")
    return None


# ---------------------------------------------------------------------------
# Building one test
# ---------------------------------------------------------------------------

def compile_options(entry: dict, march: str) -> tuple[list[str], list[str]]:
    """(gcc options, notes) for one entry, with every divergence recorded."""
    options = entry["gcc_opts"]
    notes: list[str] = []

    for old, new, reason in GCC_OPTS_PATCHES.get(entry["config"], []):
        if options.count(old) != 1:
            raise DirectedError(
                f"config {entry['config']!r}: expected exactly one {old!r} in "
                f"gcc_opts to replace with {new!r}, found "
                f"{options.count(old)}. Upstream's options have changed and "
                f"this substitution needs looking at.")
        options = options.replace(old, new)
        notes.append(f"gcc_opts: {old} -> {new} ({reason})")

    tokens = shlex.split(options)
    if not any(token.startswith("-march=") for token in tokens):
        tokens.insert(0, f"-march={march}")
        notes.append(f"-march={march} added: {MARCH_NOTE}")

    for source, reason in EXTRA_SOURCES.get(entry["config"], []):
        if not source.is_file():
            raise DirectedError(f"missing extra source {source}")
        tokens.append(str(source))
        notes.append(f"{source.name} compiled in: {reason}")

    return tokens, notes


def linker_script(entry: dict, stock: bool) -> tuple[Path, list[str]]:
    """The linker script for one entry, patched under build/ where it needs it."""
    script = entry["ld_script"]
    patches = LD_SCRIPT_PATCHES.get(entry["config"], [])
    if stock or not patches:
        return script, []

    overlay = BUILD / "directed_overlay" / script.name
    text = script.read_text(encoding="utf-8")

    for old, new in patches:
        if text.count(old) != 1:
            raise DirectedError(
                f"{script.name}: expected exactly one\n  {old!r}\nto replace "
                f"with\n  {new!r}\nfound {text.count(old)}. Upstream's script "
                f"has changed and this substitution needs looking at.")
        text = text.replace(old, new)
    overlay.parent.mkdir(parents=True, exist_ok=True)
    # Written every time rather than only when absent, so an edit to the
    # substitutions above cannot leave a stale copy in place. Under the lock
    # because entries are built several at a time and they share the file.
    with OVERLAY_LOCK:
        overlay.write_text(text, encoding="utf-8")
    return overlay, [LD_SCRIPT_NOTE]


def build_one(entry: dict, march: str, directory: Path, stock_ld: bool) -> dict:
    """Compile one entry to a flat binary, as scripts/compile_test.py does."""
    directory.mkdir(parents=True, exist_ok=True)
    objectfile = directory / "test.o"
    binary = directory / "test.bin"
    log = directory / "compile.log"

    try:
        options, notes = compile_options(entry, march)
        script, script_notes = linker_script(entry, stock_ld)
        notes += script_notes
    except DirectedError as error:
        return {"built": False, "notes": [], "detail": str(error)}

    commands = [
        [str(GCC), *options,
         f"-I{entry['includes']}", f"-T{script}",
         "-o", str(objectfile), str(entry["test_srcs"])],
        [str(OBJCOPY), "-O", "binary", str(objectfile), str(binary)],
    ]

    output = ""
    for command in commands:
        output += shlex.join(command) + "\n"
        # cwd is dv/uvm/core_ibex, which is what the relative -I paths inside
        # gcc_opts are written against; upstream runs from the same place.
        completed = subprocess.run(command, cwd=CORE_IBEX, text=True,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, check=False)
        output += completed.stdout
        if completed.returncode != 0:
            log.write_text(output, encoding="utf-8")
            first = next((line for line in completed.stdout.splitlines()
                          if ": error:" in line or "Error:" in line),
                         completed.stdout.splitlines()[-1]
                         if completed.stdout.strip() else "")
            return {"built": False, "notes": notes, "detail": first.strip()[:120]}
    log.write_text(output, encoding="utf-8")

    if binary.stat().st_size == 0:
        return {"built": False, "notes": notes,
                "detail": "objcopy produced an empty binary"}
    return {"built": True, "notes": notes, "binary": binary,
            "bytes": binary.stat().st_size}


# ---------------------------------------------------------------------------
# Running one test
# ---------------------------------------------------------------------------

OUTCOMES = [
    (re.compile(r"--- RISC-V UVM TEST PASSED ---"), "passed"),
    (re.compile(r"Cosim mismatch (.+)"), "cosim mismatch"),
    # The program's own verdict, which is what riscv-tests and the epmp tests
    # report. The arch tests never send it; see the module docstring.
    (re.compile(r"Test failed due to RISCV-DV handshake"), "self-check failed"),
    (re.compile(r"TEST TIMEOUT!!"), "cycle timeout"),
    (re.compile(r"double_fault detector"), "double faults"),
    (re.compile(r"Randomization failed"), "randomize failed"),
    (re.compile(r"--- RISC-V UVM TEST FAILED ---"), "failed"),
]


def environment() -> dict[str, str]:
    env = dict(os.environ)
    # z3 for Verilator's constraint solver, and Spike's shared libraries.
    # Without z3 every randomize() fails and the run is over in a tenth of a
    # second, so run_one checks the wall time it took as well as its output.
    env["PATH"] = f"{TOOLS_BIN}:{env.get('PATH', '')}"
    env["LD_LIBRARY_PATH"] = ":".join(
        [str(TOOLS_LIB), str(SPIKE_LIB), env.get("LD_LIBRARY_PATH", "")])
    return env


def run_one(testbench: Path, entry: dict, binary: Path, directory: Path,
            cycles: int, seconds: int, extra: list[str]) -> dict:
    command = [str(testbench), f"+UVM_TESTNAME={entry['rtl_test']}",
               f"+bin={binary}", f"+signature_addr={SIGNATURE_ADDR}",
               f"+timeout_in_cycles={cycles}", *extra]
    started = time.monotonic()
    try:
        completed = subprocess.run(command, cwd=directory, env=environment(),
                                   text=True, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, timeout=seconds)
        output, timed_out = completed.stdout, False
    except subprocess.TimeoutExpired as expired:
        output = expired.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        timed_out = True
    elapsed = time.monotonic() - started

    (directory / "sim.log").write_text(output, encoding="utf-8")

    if timed_out:
        return {"outcome": "wall-clock timeout", "detail": "", "seconds": elapsed}
    # A run that ends in a fraction of a second did not simulate anything: it
    # is what a missing z3 looks like, and it has been read as a result here
    # before. Report it as a broken environment rather than as an outcome.
    if elapsed < 1.0 and "RISC-V UVM TEST PASSED" not in output:
        return {"outcome": "no simulation",
                "detail": f"exited after {elapsed:.2f}s; z3 on PATH?",
                "seconds": elapsed}
    for pattern, outcome in OUTCOMES:
        found = pattern.search(output)
        if found:
            detail = found.group(1).strip()[:70] if found.groups() else ""
            return {"outcome": outcome, "detail": detail, "seconds": elapsed}
    return {"outcome": "no verdict", "detail": "", "seconds": elapsed}


# The ePMP programs end in syscalls.c's `tohost_exit(code)`, which writes two
# words to the signature address:
#
#   sw (code << 8) | CORE_STATUS
#   sw (TEST_PASS << 8) | TEST_RESULT
#
# The second is a constant. `exit(ret)` from `checkTestResult`, `main()`
# returning non-zero, and `handle_trap` all reach it, so every ePMP program
# tells the testbench it passed no matter what it found -- upstream's harness
# has no way to fail one of these 744 tests. The result is still there in the
# first word, and TRACE_EXECUTION records the store, so it is read back out of
# the tracer log. `handle_trap` is the one caller with a fixed code.
EPMP_TRAP_CODE = 1337
EPMP_STATUS_STORE = re.compile(
    r"PA:0x8ffffff8\s+store:0x([0-9a-f]{8})", re.IGNORECASE)


def epmp_exit_code(trace: Path) -> int | None:
    """The code an ePMP program passed to exit(), from the execution trace."""
    if not trace.is_file():
        return None
    for line in trace.read_text(encoding="utf-8", errors="replace").splitlines():
        found = EPMP_STATUS_STORE.search(line)
        if not found:
            continue
        word = int(found.group(1), 16)
        if word & 0xFF == 0x00:  # CORE_STATUS, so the payload is the code
            return word >> 8
    return None


def handle(entry: dict, testbench: Path, march: str, cycles: int,
           wall_seconds: int, extra: list[str], build_only: bool,
           stock_ld: bool) -> dict:
    directory = BUILD / "directed" / entry["test"]
    result = {"test": entry["test"], "group": entry["config"],
              "rtl_test": entry["rtl_test"]}

    build = build_one(entry, march, directory, stock_ld)
    result["notes"] = build["notes"]
    if not build["built"]:
        result["outcome"] = "build failed"
        result["detail"] = build["detail"]
        return result
    result["bytes"] = build["bytes"]
    if build_only:
        result["outcome"] = "built"
        result["detail"] = ""
        return result

    seconds = wall_seconds or int(entry.get("timeout_s", 300))
    result.update(run_one(testbench, entry, build["binary"], directory,
                          cycles, seconds, extra))

    if entry["config"] == "epmp-tests" and result["outcome"] == "passed":
        code = epmp_exit_code(directory / "trace_core_00000000.log")
        result["exit_code"] = code
        if code is None:
            result["outcome"] = "no verdict"
            result["detail"] = "no CORE_STATUS write found in the trace"
        elif code != 0:
            result["outcome"] = "self-check failed"
            result["detail"] = (f"the program exited {code}"
                                if code != EPMP_TRAP_CODE else
                                f"the program took an unhandled trap "
                                f"(handle_trap exits {EPMP_TRAP_CODE})")
    return result


# ---------------------------------------------------------------------------
# Selection and reporting
# ---------------------------------------------------------------------------

def select(entries: list[dict], args) -> list[dict]:
    chosen = entries
    if args.group:
        chosen = [e for e in chosen if e["config"] in args.group]
    if args.only:
        chosen = [e for e in chosen if e["test"] in args.only]
    if args.pattern:
        chosen = [e for e in chosen
                  if any(fnmatch.fnmatch(e["test"], p) for p in args.pattern)]
    if args.stride > 1:
        chosen = chosen[args.offset::args.stride]
    if args.limit:
        chosen = chosen[:args.limit]
    return chosen


def report(results: list[dict], inapplicable: list[dict], config: str,
           total: int) -> int:
    output = BUILD / f"directed_results_{config}.json"
    output.write_text(json.dumps(
        {"config": config, "results": results, "inapplicable": inapplicable},
        indent=2), encoding="utf-8")

    tally: dict[str, int] = {}
    for result in results:
        tally[result["outcome"]] = tally.get(result["outcome"], 0) + 1

    print(f"\n{config}: {len(results)} run of {total} entries")
    for name, count in sorted(tally.items(), key=lambda item: -item[1]):
        print(f"  {count:>5}  {name}")
    if inapplicable:
        reasons: dict[str, int] = {}
        for entry in inapplicable:
            reasons[entry["reason"]] = reasons.get(entry["reason"], 0) + 1
        print(f"  {len(inapplicable):>5}  inapplicable to this configuration")
        for reason, count in sorted(reasons.items(), key=lambda i: -i[1]):
            print(f"          {count} {reason}")

    # Everything that ran, ran a compile that differs from upstream's in some
    # recorded way, or ran a program whose layout is known not to match its
    # source. Neither is silent.
    noted = [r for r in results if r.get("notes")]
    if noted:
        kinds: dict[str, int] = {}
        for result in noted:
            for note in result["notes"]:
                kinds[note] = kinds.get(note, 0) + 1
        print(f"\n{len(noted)} of {len(results)} were built or run in a way "
              f"that differs from upstream:")
        for note, count in sorted(kinds.items(), key=lambda item: -item[1]):
            print(f"  {count:>5}  {note}")

    print(f"\nwritten to {output.relative_to(HERE)}")
    return 0 if tally.get("passed", 0) == len(results) and results else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", default="small",
                        help="the Ibex configuration to run against; the "
                             "binary is build/obj_<config>/core_ibex_tb")
    parser.add_argument("--group", action="append", default=[],
                        help="only entries from this testlist config group "
                             "(riscv-tests, riscv-arch-tests, epmp-tests)")
    parser.add_argument("--only", action="append", default=[],
                        help="only these entries, by name")
    parser.add_argument("--pattern", action="append", default=[],
                        help="only entries whose name matches this glob")
    parser.add_argument("--limit", type=int, default=0,
                        help="stop after this many entries")
    parser.add_argument("--stride", type=int, default=1,
                        help="take every Nth entry, for a spread sample")
    parser.add_argument("--offset", type=int, default=0,
                        help="where --stride starts")
    parser.add_argument("--jobs", type=int, default=4,
                        help="entries in flight at once; capped at 4 because "
                             "each simulation spawns its own z3")
    # Upstream never overrides `timeout_in_cycles`, so its budget is the
    # 100,000,000 cycles core_ibex_base_test defaults to. That is hours here.
    # This is set instead to about what the wall-clock budget below can reach,
    # so a test that ends on it has genuinely run out of time rather than out
    # of an arbitrary number: at 200,000 cycles ten of the pmp_mseccfg entries
    # reported a timeout and all ten pass at 5,000,000, the first of them
    # finishing at 236,835.
    parser.add_argument("--timeout-cycles", type=int, default=5000000)
    parser.add_argument("--timeout-seconds", type=int, default=0,
                        help="wall-clock budget per test, overriding the "
                             "entry's own timeout_s")
    parser.add_argument("--stock-ld", action="store_true",
                        help="link with the vendored linker scripts unpatched; "
                             "see LD_SCRIPT_PATCHES for what that costs")
    parser.add_argument("--build-only", action="store_true",
                        help="compile, do not simulate")
    parser.add_argument("--list", action="store_true",
                        help="print the selection and stop")
    parser.add_argument("extra", nargs="*", help="further plusargs")
    args = parser.parse_args(argv)

    try:
        entries = merge(*parse_testlist(TESTLIST))
        parameters, defines = build_tb.config_parameters(args.config)
        march = isa_march(defines)
    except (DirectedError, build_tb.BuildError) as error:
        print(f"run_directed: {error}", file=sys.stderr)
        return 1

    chosen = select(entries, args)
    if not chosen:
        print("run_directed: nothing selected", file=sys.stderr)
        return 1

    runnable, inapplicable = [], []
    for entry in chosen:
        reason = applicability(entry, parameters, defines)
        if reason:
            inapplicable.append({"test": entry["test"],
                                 "group": entry["config"], "reason": reason})
        else:
            runnable.append(entry)

    if args.list:
        groups: dict[str, int] = {}
        for entry in chosen:
            groups[entry["config"]] = groups.get(entry["config"], 0) + 1
        print(f"{len(entries)} directed entries, {len(chosen)} selected, "
              f"{len(runnable)} applicable to --config {args.config}")
        for name, count in sorted(groups.items()):
            print(f"  {count:>5}  {name}")
        for entry in chosen:
            print(f"  {entry['test']:<58} {entry['config']}")
        return 0

    testbench = BUILD / f"obj_{args.config}" / "core_ibex_tb"
    if not args.build_only and not testbench.is_file():
        print(f"run_directed: no testbench at {testbench}\n"
              f"run: python3 {HERE / 'build_tb.py'} --config {args.config}",
              file=sys.stderr)
        return 1
    for tool in (GCC, OBJCOPY):
        if not tool.is_file():
            print(f"run_directed: no {tool.name} at {tool}", file=sys.stderr)
            return 1

    jobs = max(1, min(args.jobs, 4))
    results: list[dict] = []
    print(f"run_directed: {len(runnable)} entries on --config {args.config}, "
          f"{len(inapplicable)} inapplicable, {jobs} at a time")
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {pool.submit(handle, entry, testbench, march,
                               args.timeout_cycles, args.timeout_seconds,
                               args.extra, args.build_only,
                               args.stock_ld): entry
                   for entry in runnable}
        for future in concurrent.futures.as_completed(futures):
            result = future.result()
            results.append(result)
            detail = f"  {result['detail']}" if result.get("detail") else ""
            print(f"  {result['test']:<58} {result['outcome']}{detail}",
                  flush=True)

    results.sort(key=lambda r: (r["group"], r["test"]))
    return report(results, inapplicable, args.config, len(chosen))


if __name__ == "__main__":
    raise SystemExit(main())
