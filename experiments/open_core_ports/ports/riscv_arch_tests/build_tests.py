#!/usr/bin/env python3
"""Build the applicable riscv-arch-test programs for Ibex Simple System.

Upstream builds these through `act`, which resolves a UDB configuration with a
Ruby toolchain, then compiles each test twice: once to capture a golden
signature from a reference model, and once with that signature linked in so the
program self-checks. This port needs neither. It runs one binary under two
harnesses and requires them to agree, so a single -DSIGNATURE build is enough,
and target/rvtest_config.h is written by hand instead of generated. See
target/rvmodel_macros.h for why that is the question being asked.

Which tests apply is decided by each test's own REQUIRED_EXTENSIONS header
rather than by a list of directories kept in step by hand, so a test added
upstream is picked up or skipped on its own terms.

    python3 build_tests.py                     # the `small` configuration
    python3 build_tests.py --config bmfull     # bitmanip and PMP as well
    python3 build_tests.py --list              # what applies, and what does not
    python3 build_tests.py --filter I-         # a subset

Standard library only, matching fetch.py and bin2vmem.py.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SUITE = ROOT / "deps" / "riscv_arch_test"
TOOLCHAIN = ROOT / "deps" / "riscv_gcc15" / "bin" / "riscv-none-elf-"
TARGET = HERE / "target"
BUILD = HERE / "build"

sys.path.insert(0, str(ROOT))
from bin2vmem import convert as bin_to_vmem  # noqa: E402

# examples/simple_system/rtl/ibex_simple_system.sv: boot_addr_i is 0x00100000
# and the RAM is instantiated with .Depth(1024*1024/4).
RAM_BASE = 0x0010_0000
RAM_BYTES = 1024 * 1024

# What every Ibex configuration implements, against which each test's declared
# REQUIRED_EXTENSIONS is checked.
#
# I, M, C and U come straight from MISA_VALUE in rtl/ibex_cs_registers.sv. The
# rest are extensions MISA has no bit for:
#
#   Sm           Machine mode, which Ibex always implements.
#   Zicsr        The machine CSRs are implemented.
#   Zifencei     fence.i flushes the pipeline.
#   Zca          The compressed subset C is composed of; MISA bit 2 is set.
#   Zmmul        Implied by M.
#   Zihintntl    The NTL hints encode as `add x0, x0, rN`, which is a plain I
#                instruction on a core that does not recognise the hint.
#   Zihintpause  `pause` encodes as `fence w, 0`, which Ibex already accepts.
#
# The last two are included on the argument that their encodings are legal
# rather than on Ibex claiming support, so they are the ones most likely to be
# wrong. run_suite.py reports per-test results, so a wrong guess here shows up
# as a failing test rather than a silently skipped one.
#
# Absent from every configuration because no Ibex build implements them: Zicntr
# (no `time` CSR), Zihpm (MHPMCounterNum=0 in both configurations here), Zimop
# and Zcmop, Zicbom/Zicbop/Zicboz, Zicond, the crypto extensions, the atomics,
# supervisor mode and everything floating point.
COMMON_EXTENSIONS = frozenset({
    "I", "M", "C", "U", "Sm",
    "Zicsr", "Zifencei", "Zca", "Zmmul", "Zihintntl", "Zihintpause",
})


@dataclass(frozen=True)
class Config:
    """One Ibex configuration, and everything that depends on which one it is.

    Both are configurations Ibex itself ships in ibex_configs.yaml, elaborated
    from the same RTL. Running the suite against both is worth more than running
    it twice against one: the second is a materially different design -- PMP
    checkers, an extra pipeline stage, a single-cycle multiplier -- so it
    exercises cpptb's code generation as well as the tests.
    """

    name: str
    ibex_config: str          # the name in Ibex's ibex_configs.yaml
    project: str              # the cpptb.toml to build
    cpptb_binary: str         # relative to the open_core_ports root
    upstream_binary: str      # relative to the open_core_ports root
    extensions: frozenset[str]
    pmp_entries: int
    # 0 means no restriction on NAPOT region size, so NA4 is available.
    pmp_granularity: int = 0
    # 'harness' compares the two harnesses against each other;
    # 'spike' checks every instruction against a reference model.
    reference: str = "harness"
    # Extra -D flags for every test compiled for this configuration.
    defines: tuple[str, ...] = ()
    # Test-level params the suite declares beyond extensions. Ibex handles
    # misaligned accesses in hardware in every configuration.
    features: frozenset[str] = frozenset({"MISALIGNED_LDST"})


_UPSTREAM = "deps/ibex/{build}/lowrisc_ibex_ibex_simple_system_0/sim-verilator/Vibex_simple_system"
_CPPTB = "work/{name}/cpptb/{name}/obj/Vdpi_{name}"

CONFIGS = {
    "small": Config(
        name="small",
        ibex_config="small",
        project="cpptb.toml",
        cpptb_binary=_CPPTB.format(name="riscv_arch_tests"),
        upstream_binary=_UPSTREAM.format(build="build"),
        extensions=COMMON_EXTENSIONS,
        pmp_entries=0,
    ),
    # RV32BFull adds Zba, Zbb, Zbc and Zbs; RV32ZcaZcbZcmp adds Zcb; PMPEnable
    # with 16 regions brings in tests/priv/pmp. The suite refuses to run with
    # fewer than 8 usable PMP entries, which is why this is the 16-region
    # configuration and not the 4-region default.
    "bmfull": Config(
        name="bmfull",
        ibex_config="maxperf-pmp-bmfull",
        project="cpptb-bmfull.toml",
        cpptb_binary=_CPPTB.format(name="riscv_arch_tests_bmfull"),
        upstream_binary=_UPSTREAM.format(build="build-bmfull"),
        extensions=COMMON_EXTENSIONS | {"Zba", "Zbb", "Zbc", "Zbs", "Zcb"},
        pmp_entries=16,
    ),
    # The `small` core again, but elaborated with Ibex's co-simulation checker
    # bound in, so Spike runs in lockstep and every retired instruction and data
    # memory access is compared against it. Same tests, a different and much
    # stronger question: not "do the two harnesses agree" but "is the core
    # right". Needs Spike, which fetch.py and build_spike.py provide.
    "cosim": Config(
        name="cosim",
        ibex_config="small",
        project="cpptb.toml",
        cpptb_binary=_CPPTB.format(name="riscv_arch_tests_cosim"),
        upstream_binary=_UPSTREAM.format(build="build-cosim"),
        extensions=COMMON_EXTENSIONS,
        pmp_entries=0,
        # Spike is the reference; there is no second harness to agree with.
        reference="spike",
        # See target/rvtest_config.h: this Spike has no menvcfgh.
        defines=("-DIBEX_NO_U_MODE",),
    ),
}
DEFAULT_CONFIG = "small"

# Tests whose declared extensions Ibex implements but which still cannot be
# built for it. Keeping them out here, with the reason, is the difference
# between a suite that reports 96 of 96 and one that reports 96 of 102 with six
# compile errors nobody reads.
UNBUILDABLE = {
    "Zicsr": (
        "the generated tests need a CSR they may freely clobber, and choose it "
        "with a fixed ladder: fflags if F, vxsat if V, mepc if U-mode is "
        "absent, nothing if Zicntr, else #error. Ibex has U-mode but no F, no "
        "V and no `time` CSR, so it reaches the #error. Defining "
        "ZICNTR_SUPPORTED would compile, but that branch is `li x11, 0` and "
        "tests no CSR at all, which would be six passing tests that exercise "
        "nothing. Excluded instead."
    ),
}

# Roughly one real test image (I-add-00 is 169,552 bytes). The padded
# baseline carries this much data so its load costs what a real one does.
NULL_TEST_PAD = 165_000

CONFIG_BLOCK = re.compile(
    r"START_TEST_CONFIG\s*#+(?P<body>.*?)#+\s*END_TEST_CONFIG", re.S)
REQUIRED_RE = re.compile(r"REQUIRED_EXTENSIONS:\s*(\[.*?\])")
MARCH_RE = re.compile(r"MARCH:\s*(\S+)")
PARAMS_RE = re.compile(r"^#\s*params:\s*$(?P<body>.*?)(?=^#\s*MARCH:)",
                       re.S | re.M)
PARAM_RE = re.compile(r"^#\s+(?P<key>\w+):\s*(?P<value>.+?)\s*$", re.M)


def parse_extensions(text: str) -> set[str]:
    """The REQUIRED_EXTENSIONS list, which is not quite a Python literal.

    Most tests write `['I', 'Zca']`, but the Sv ones write `[I, Sv57]` with bare
    words, so ast.literal_eval raises on 137 of them. Splitting by hand accepts
    both. Some also carry a NORUN marker, which upstream uses for tests that are
    not meant to be run at all; it is kept in the set so the caller sees it.
    """
    return {token.strip().strip("'\"")
            for token in text.strip("[]").split(",") if token.strip()}


def parse_params(body: str) -> dict[str, str]:
    """The `params:` block a test declares, if it has one.

    Across the whole suite these take only a handful of shapes: MXLEN is 32 or
    64, NUM_PMP_ENTRIES is always '>0', and PMP_TOR_SUPPORTED,
    PMP_NAPOT_SUPPORTED, PMP_NA4_SUPPORTED and MISALIGNED_LDST are always
    'true'. unmet_params below rejects anything outside that, so a new shape
    fails loudly rather than being read as satisfied.
    """
    block = PARAMS_RE.search(body)
    if not block:
        return {}
    return {m.group("key"): m.group("value").strip("'\"")
            for m in PARAM_RE.finditer(block.group("body"))}


def unmet_params(params: dict[str, str], config: Config) -> list[str]:
    """Which of a test's declared params this configuration does not satisfy."""
    unmet = []
    for key, value in params.items():
        if key == "MXLEN":
            if value != "32":
                unmet.append(f"MXLEN {value}, Ibex is 32")
        elif key == "NUM_PMP_ENTRIES":
            if value != ">0":
                unmet.append(f"unrecognised NUM_PMP_ENTRIES constraint {value!r}")
            elif config.pmp_entries == 0:
                unmet.append("needs PMP, this configuration has none")
        elif key in ("PMP_TOR_SUPPORTED", "PMP_NAPOT_SUPPORTED",
                     "PMP_NA4_SUPPORTED"):
            # rtl/ibex_pmp.sv implements all three modes, and PMPGranularity=0
            # places no lower bound on region size, so NA4 is real. They still
            # need PMP to be enabled at all.
            if value != "true":
                unmet.append(f"unrecognised {key} constraint {value!r}")
            elif config.pmp_entries == 0:
                unmet.append("needs PMP, this configuration has none")
        elif key in config.features:
            if value != "true":
                unmet.append(f"unrecognised {key} constraint {value!r}")
        else:
            unmet.append(f"unhandled param {key}: {value!r}")
    return unmet


class BuildError(RuntimeError):
    pass


class UnsupportedByConfig(BuildError):
    """A test that applies on paper but cannot be built for this configuration.

    Distinct from BuildError so the summary can separate "the suite does not
    support this core" from "the port is broken". Only raised for failures
    matched against a known signature; anything else stays a build failure.
    """


# Compile failures that are a limitation of the suite rather than of this port.
# Each entry is (signature in the compiler output, predicate on the config,
# explanation). Classifying on the observed failure rather than predicting it
# from the source keeps working tests from being excluded by a rule that is too
# broad: 22 of the applicable PMP tests reference the arithmetic below and only
# 8 of them actually break on it.
KNOWN_COMPILE_LIMITS = [
    (
        "for REPT",
        lambda config: config.pmp_granularity < 2,
        "the PMP tests size their NAPOT padding as "
        "`(1 << (UDB_PMP_GRANULARITY - 3)) - 1`, which underflows to -1 for a "
        "core with granularity below 3 and reaches .rept as 2^64-1. The tests "
        "special-case granularity 2 but not 0 or 1. Ibex's PMPGranularity is 0, "
        "meaning no restriction on region size, which is the most capable "
        "setting and the one that supports NA4; raising it to work around this "
        "would change the design under test to suit the test."
    ),
]


def toolchain(tool: str) -> Path:
    path = Path(f"{TOOLCHAIN}{tool}")
    if not path.is_file():
        raise BuildError(
            f"missing {path}\n"
            f"run: python3 {ROOT / 'fetch.py'} riscv_gcc15")
    return path


def discover(config: Config) -> tuple[list[dict], list[dict]]:
    """Split the suite into what this configuration can run and what it cannot.

    Both tests/rv32i and tests/priv are scanned. tests/priv is mostly supervisor
    and Sv material that Ibex will never run, but its PMP subset applies as soon
    as PMPEnable is set, and that subset is large enough to be worth the walk.
    """
    roots = [SUITE / "tests" / "rv32i", SUITE / "tests" / "priv"]
    for root in roots:
        if not root.is_dir():
            raise BuildError(
                f"missing {root}\n"
                f"run: python3 {ROOT / 'fetch.py'} riscv_arch_test")

    # A program that halts immediately, so the suite carries its own measurement
    # of what a run costs before any simulation happens. See target/null_test.S.
    #
    # The padded variant carries an image the size of a real test's, so the
    # difference between the two is the cost of loading it. NULL_TEST_PAD is
    # measured from a real test rather than guessed, so the two stay comparable
    # if the suite's signature sizing changes.
    applicable = [
        {"name": "null", "group": "baseline", "march": "rv32i_zicsr",
         "path": str(TARGET / "null_test.S"), "required": [], "defines": []},
        {"name": "null-loaded", "group": "baseline", "march": "rv32i_zicsr",
         "path": str(TARGET / "null_test.S"), "required": [],
         "defines": [f"-DNULL_TEST_PAD={NULL_TEST_PAD}"]},
    ]
    skipped = []
    for source in sorted(s for root in roots for s in root.rglob("*.S")):
        text = source.read_text(encoding="utf-8", errors="replace")
        block = CONFIG_BLOCK.search(text)
        if not block:
            skipped.append({"name": source.stem, "path": str(source),
                            "reason": "no START_TEST_CONFIG block"})
            continue

        required_match = REQUIRED_RE.search(block.group("body"))
        march_match = MARCH_RE.search(block.group("body"))
        if not (required_match and march_match):
            skipped.append({"name": source.stem, "path": str(source),
                            "reason": "config block lacks "
                                      "REQUIRED_EXTENSIONS or MARCH"})
            continue

        required = parse_extensions(required_match.group(1))
        params = parse_params(block.group("body"))
        entry = {
            "name": source.stem,
            "group": source.parent.name,
            "path": str(source),
            "march": march_match.group(1),
            "required": sorted(required),
        }

        missing = required - config.extensions - {"NORUN"}
        unmet = unmet_params(params, config)
        if "NORUN" in required:
            entry["reason"] = "marked NORUN by the suite"
            skipped.append(entry)
        elif missing:
            entry["reason"] = ("this configuration does not implement "
                               + ", ".join(sorted(missing)))
            skipped.append(entry)
        elif unmet:
            entry["reason"] = "; ".join(unmet)
            skipped.append(entry)
        elif entry["group"] in UNBUILDABLE:
            entry["reason"] = UNBUILDABLE[entry["group"]]
            skipped.append(entry)
        else:
            applicable.append(entry)
    return applicable, skipped


def compile_one(test: dict, config: Config) -> dict:
    """Compile one test to an ELF and a VMEM, returning its manifest entry."""
    out = BUILD / config.name / test["group"]
    out.mkdir(parents=True, exist_ok=True)
    elf = out / f"{test['name']}.elf"
    vmem = out / f"{test['name']}.vmem"

    # The same flags act would use for the -DSIGNATURE pass, from
    # framework/src/act/build_plan.py: _compiler_cmd plus the signature build.
    # -O0 is upstream's choice and is kept: these are hand-written assembly
    # tests whose instruction sequences are the thing under test, so letting the
    # compiler rearrange the little C-level glue would only add variance.
    command = [
        str(toolchain("gcc")),
        f"-I{TARGET}",
        f"-T{TARGET / 'link.ld'}",
        "-O0", "-g", "-mcmodel=medany", "-nostdlib",
        f"-I{SUITE / 'tests' / 'env'}",
        "-Wl,--no-warn-rwx-segments",
        f"-march={test['march']}",
        "-mabi=ilp32",
        "-DSIGNATURE",
        # The suite has two build modes and neither is "run on the DUT and
        # report what the signature came out as". -DSIGNATURE makes RVTEST_SIGUPD
        # store results, which is what we want, but tests/env/riscv_arch_test.h
        # then pulls in sail_macros.h:
        #
        #     #ifndef RVTEST_SELFCHECK
        #       #include "sail_macros.h"
        #     #endif
        #
        # and that header #undefs the target's halt, IO and data-section macros
        # and redefines them for Sail: HTIF tohost instead of the Simple System
        # control register, and a CLINT at 0x02000000. On Ibex the first store
        # to that address traps, which is exactly what the vector table caught
        # while this was being brought up.
        #
        # -DRVTEST_SELFCHECK would suppress it but also switches SIGUPD from
        # storing results to comparing them against a golden signature that
        # would have to come from a reference model, which is the dependency
        # this port is avoiding.
        #
        # So the header's own include guard is set instead, which makes the
        # include a no-op and leaves target/rvmodel_macros.h in place. It relies
        # on the guard's name rather than on a supported switch, because there
        # is no supported switch; if upstream renames it the tests go back to
        # storing to Sail's addresses and every one of them reports EARLY-TRAP,
        # which is loud rather than silent.
        "-D_SAIL_MACROS_H",
        f"-DIBEX_PMP_ENTRIES={config.pmp_entries}",
        *config.defines,
        "-DTEST_FLEN=0",
        f'-DTEST_FILE="{Path(test["path"]).name}"',
        *test.get("defines", []),
        "-o", str(elf),
        test["path"],
        str(TARGET / "vectors.S"),
    ]
    completed = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=False)
    if completed.returncode != 0:
        for signature, applies, reason in KNOWN_COMPILE_LIMITS:
            if signature in completed.stdout and applies(config):
                raise UnsupportedByConfig(reason)
        raise BuildError(f"{test['name']}: compile failed\n{completed.stdout.strip()}")

    raw = out / f"{test['name']}.bin"
    subprocess.run([str(toolchain("objcopy")), "-O", "binary",
                    "--gap-fill", "0", str(elf), str(raw)], check=True)
    data = raw.read_bytes()
    raw.unlink()

    symbols = read_symbols(elf, ("rvtest_sig_begin", "rvtest_sig_end",
                                 "rvtest_entry_point", "rvtest_vector_table"))

    # The vector table is the lowest thing in the image and sits at the base of
    # RAM, so the binary objcopy produces starts exactly there and loads at
    # offset zero. Both harnesses depend on that: the upstream ELF loader places
    # segments relative to the file's lowest address rather than at their
    # absolute ones, so an image that did not begin at the RAM base would be
    # loaded shifted. Checking it here makes that a build error instead of a
    # core that traps at reset and spins.
    load_base = symbols["rvtest_vector_table"]
    if load_base != RAM_BASE:
        raise BuildError(
            f"{test['name']}: image starts at {load_base:#x}, "
            f"not at the RAM base {RAM_BASE:#x}")

    end = load_base + len(data)
    if end > RAM_BASE + RAM_BYTES:
        raise BuildError(
            f"{test['name']}: image ends at {end:#x}, past the "
            f"{RAM_BYTES // 1024}kB RAM at {RAM_BASE:#x}")

    vmem.write_text(bin_to_vmem(data, offset=load_base - RAM_BASE),
                    encoding="utf-8")

    return {
        "name": test["name"],
        "group": test["group"],
        "march": test["march"],
        "elf": str(elf.relative_to(HERE)),
        "vmem": str(vmem.relative_to(HERE)),
        "load_addr": load_base,
        "bytes": len(data),
        # Where the signature lives, so a mismatch can be diffed word by word
        # through the cpptb memory backdoor rather than only observed.
        "sig_begin": symbols["rvtest_sig_begin"],
        "sig_end": symbols["rvtest_sig_end"],
    }


def read_symbols(elf: Path, wanted: tuple[str, ...]) -> dict[str, int]:
    completed = subprocess.run([str(toolchain("nm")), str(elf)], text=True,
                               stdout=subprocess.PIPE, check=True)
    found = {}
    for line in completed.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] in wanted:
            found[parts[2]] = int(parts[0], 16)
    missing = set(wanted) - set(found)
    if missing:
        raise BuildError(f"{elf.name}: no symbol {', '.join(sorted(missing))}")
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--list", action="store_true",
                        help="show what applies without building")
    parser.add_argument("--filter", default="",
                        help="only tests whose name contains this")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--clean", action="store_true",
                        help="remove this configuration's build tree first")
    parser.add_argument("--config", default=DEFAULT_CONFIG, choices=sorted(CONFIGS),
                        help=f"Ibex configuration to build for "
                             f"(default {DEFAULT_CONFIG})")
    args = parser.parse_args(argv)
    config = CONFIGS[args.config]
    out = BUILD / config.name

    try:
        applicable, skipped = discover(config)
    except BuildError as error:
        print(f"build_tests: {error}", file=sys.stderr)
        return 1

    if args.filter:
        applicable = [t for t in applicable if args.filter in t["name"]]

    by_group: dict[str, int] = {}
    for test in applicable:
        by_group[test["group"]] = by_group.get(test["group"], 0) + 1

    print(f"config {config.name} (Ibex {config.ibex_config}): "
          f"{len(applicable)} applicable, {len(skipped)} not applicable")
    for group, count in sorted(by_group.items()):
        print(f"  {group:<16} {count}")

    if args.list:
        reasons: dict[str, int] = {}
        for test in skipped:
            reasons[test["reason"]] = reasons.get(test["reason"], 0) + 1
        print("\nskipped:")
        for reason, count in sorted(reasons.items(), key=lambda kv: -kv[1]):
            print(f"  {count:>4}  {reason}")
        return 0

    if args.clean and out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True, exist_ok=True)

    built, failures = [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(compile_one, test, config): test
                   for test in applicable}
        for future in concurrent.futures.as_completed(futures):
            test = futures[future]
            try:
                built.append(future.result())
            except UnsupportedByConfig as error:
                skipped.append({**test, "reason": str(error)})
            except (BuildError, subprocess.CalledProcessError) as error:
                failures.append((test["name"], str(error)))

    built.sort(key=lambda entry: (entry["group"], entry["name"]))
    if len(built) + len(failures) != len(applicable):
        dropped = len(applicable) - len(built) - len(failures)
        print(f"\n{dropped} test(s) applied on paper but the suite cannot build "
              f"them for this configuration; --list explains each")
    manifest = out / "manifest.json"
    manifest.write_text(json.dumps({
        "config": config.name,
        "ibex_config": config.ibex_config,
        "cpptb_binary": config.cpptb_binary,
        "upstream_binary": config.upstream_binary,
        "reference": config.reference,
        "ram_base": RAM_BASE,
        "ram_bytes": RAM_BYTES,
        "pmp_entries": config.pmp_entries,
        "extensions": sorted(config.extensions),
        "tests": built,
        "not_applicable": skipped,
    }, indent=2), encoding="utf-8")

    print(f"\nbuilt {len(built)} test(s) into {out.relative_to(HERE)}")
    print(f"wrote {manifest.relative_to(HERE)}")
    for name, error in failures:
        print(f"  FAILED {name}: {error.splitlines()[0]}", file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
