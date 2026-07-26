#!/usr/bin/env python3
"""Generate the cpptb project for a non-default Ibex configuration.

`cpptb build --project` takes a directory and reads the cpptb.toml in it, so
each configuration needs a project of its own. The two differ in about a dozen
lines out of 240 -- the rest is a list of 141 RTL sources -- and committing that
list twice would guarantee the copies drift.

So cpptb.toml is the single description of the design, and this applies the
configuration delta from build_tests.py's CONFIGS table to produce the others.
The generated project is a sibling directory rather than a nested one so every
`../../deps/...` path in the source list stays correct without rewriting; only
the testbench path has to move.

    python3 configure.py --config bmfull
    python3 configure.py --config bmfull --show    # print the delta only

Generated projects are build output and are gitignored. Regenerate after editing
cpptb.toml.

Standard library only, matching the other tools here.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from build_tests import CONFIGS, DEFAULT_CONFIG

SPIKE_PREFIX = Path(__file__).resolve().parents[2] / "deps/spike_cosim/install"
SPIKE_PACKAGES = ["riscv-riscv", "riscv-disasm", "riscv-fdt"]

HERE = Path(__file__).resolve().parent
BASE = HERE / "cpptb.toml"

IBEX = Path(__file__).resolve().parents[2] / "deps/ibex"

# Which of Ibex's parameters reach the design as `define and which as -G. The
# split is declared by the fusesoc core file -- vlogdefine against vlogparam in
# lowrisc_ibex_ibex_simple_system_0.eda.yml -- and is not a choice made here.
# Getting it wrong does not fail the build: the parameter is silently ignored
# and the design elaborates with the default.
AS_DEFINE = {"RV32M", "RV32B", "RV32ZC", "RegFile"}


def ibex_config_opts(name: str) -> dict[str, str]:
    """The parameters Ibex's own tooling emits for a named configuration.

    `util/ibex_config.py <name> fusesoc_opts` is what Ibex's CI uses, so asking
    it is the difference between matching upstream and transcribing upstream.
    ibex_configs.yaml changing is then a change in behaviour rather than a
    silent divergence.
    """
    import os
    import subprocess

    env = dict(os.environ)
    pylibs = IBEX.parent / ".tools/pylibs"
    if pylibs.is_dir():
        env["PYTHONPATH"] = f"{pylibs}:{env.get('PYTHONPATH', '')}"
    result = subprocess.run(
        [sys.executable, "util/ibex_config.py", name, "fusesoc_opts"],
        cwd=IBEX, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False)
    if result.returncode != 0:
        raise SystemExit(f"configure: ibex_config.py {name} failed:\n"
                         f"{result.stderr.strip()}")
    opts = {}
    for token in result.stdout.split():
        key, _, value = token.lstrip("-").partition("=")
        if key and value:
            opts[key] = value
    if not opts:
        raise SystemExit(f"configure: ibex_config.py {name} emitted nothing")
    return opts


def apply_ibex_config(text: str, name: str) -> str:
    """Rewrite the defines and parameters for a named Ibex configuration."""
    for key, value in ibex_config_opts(name).items():
        if key in AS_DEFINE:
            pattern = rf'"{key}=[^"]*"'
            replacement = f'"{key}={value}"'
        else:
            pattern = rf'^{key} = "[^"]*"$'
            replacement = f'{key} = "{value}"'
        text, count = re.subn(pattern, replacement, text, flags=re.M)
        if count != 1:
            raise SystemExit(
                f"configure: {key} matched {count} times in cpptb.toml, "
                f"expected exactly 1; the base project and ibex_configs.yaml "
                f"have diverged")
    return text

# Extra SystemVerilog, extra C++ and extra Verilator arguments for
# configurations that need them. The co-simulation build is the only one that
# does. Paths are relative to open_core_ports, matching the base project.
EXTRAS: dict[str, dict] = {
    "cosim": {
        # Reused from Ibex verbatim. The checker binds itself into
        # ibex_simple_system and calls the cosim DPI on every retired
        # instruction and every data memory access; cosim_dpi.svh carries the
        # import declarations and is compiled, not included.
        # cosim_dpi.svh is copied in as .sv rather than referenced in place.
        # cpptb's source list accepts .sv and .v only, on the reasonable ground
        # that .svh is by convention an include file; upstream's fusesoc core
        # nonetheless lists it as systemVerilogSource and compiles it, because
        # it holds the DPI import declarations the checker needs at file scope.
        # Copying keeps the fetched tree untouched.
        "copy_sources": [
            ("../../deps/ibex/dv/cosim/cosim_dpi.svh", "cosim_dpi_imports.sv"),
        ],
        "sources": [
            "../../deps/ibex/dv/verilator/simple_system_cosim/ibex_simple_system_cosim_checker.sv",
            # Not upstream's bind file: slang rejects its implicit parameter
            # shorthand. See cosim_bind.sv.
            "../riscv_arch_tests/cosim_bind.sv",
        ],
        # cosim_dpi.cc and spike_cosim.cc are upstream's; cosim_glue.cc is this
        # port's replacement for simple_system_cosim.cc. Verilator compiles and
        # links whatever .cc files appear on its command line.
        "cpp_sources": [
            "../../deps/ibex/dv/cosim/cosim_dpi.cc",
            "../../deps/ibex/dv/cosim/spike_cosim.cc",
            "../riscv_arch_tests/cosim_glue.cc",
        ],
    },
}

# Applied after the configuration parameters. Only the watchdog needs it: the
# base is sized for architectural tests of a few hundred thousand cycles, and a
# co-simulation project must also be able to run CoreMark, which is upstream's
# own co-simulation workload at 40.7 million cycles.
COSIM_DELTAS = [(r"^timeout_cycles = 20000000$", "timeout_cycles = 100000000")]

HEADER = """# Generated by configure.py from ../riscv_arch_tests/cpptb.toml. Do not edit.
#
# The `{ibex_config}` configuration from Ibex's ibex_configs.yaml, elaborated
# from the same 141 sources at the same pinned commit as the base project. Only
# the configuration differs, which is the point: it is a materially different
# core built from identical RTL, so running the suite against both exercises
# cpptb's code generation as well as the tests.
#
# What the delta buys, in tests that become applicable:
#
#   RV32B=RV32BFull          Zba, Zbb, Zbc, Zbs                     +32
#   RV32ZC=RV32ZcaZcbZcmp    Zcb, ZcbM, ZcbZbb                      +11
#   PMPEnable, 16 regions    tests/priv/pmp                         +60
#   RV32M=RV32MSingleCycle   nothing new; part of the configuration
#   BranchTargetALU=1        nothing new; an extra pipeline stage to get
#   WritebackStage=1         wrong, which is worth exercising
#
# 16 PMP regions rather than Ibex's 4-region default because the suite refuses
# to run with fewer than 8 usable entries. See target/rvtest_config.h.
"""


def spike_flags() -> tuple[str, str]:
    """Compiler and linker flags for the Spike install, from its own pkg-config.

    Asking pkg-config rather than hard-coding -lriscv and friends matters
    because the set is not obvious -- it pulls in softfloat, disasm, fdt and an
    rpath -- and because it changes between Spike versions.
    """
    import os
    import subprocess

    env = dict(os.environ)
    env["PKG_CONFIG_PATH"] = str(SPIKE_PREFIX / "lib/pkgconfig")
    out = []
    for kind in ("--cflags", "--libs"):
        result = subprocess.run(["pkg-config", kind, *SPIKE_PACKAGES],
                                text=True, env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, check=False)
        if result.returncode != 0:
            raise SystemExit(
                f"configure: pkg-config {kind} failed for Spike:\n"
                f"{result.stderr.strip()}\n"
                f"build it with: python3 {SPIKE_PREFIX.parents[1]}/build_spike.py")
        out.append(result.stdout.strip())
    return out[0], out[1]


def apply_extras(text: str, name: str, config) -> str:
    """Add the co-simulation sources and the Spike build flags."""
    extras = EXTRAS.get("cosim") if config.cosim else None
    if not extras:
        return text

    added = "".join(f'  "{path}",\n'
                    for path in [name for _, name in extras.get("copy_sources", ())]
                    + extras["sources"])
    marker = '  "../../deps/ibex/examples/simple_system/rtl/ibex_simple_system.sv",\n'
    if marker not in text:
        raise SystemExit("configure: cannot find the design top in the source "
                         "list; the checker has to be compiled after it")
    text = text.replace(marker, marker + added, 1)

    cflags, libs = spike_flags()
    # spike_cosim.cc finds its own header beside it, but cosim_glue.cc is in
    # this port, so the cosim directory has to be on the include path. Absolute
    # because the compile runs in the Verilator object directory.
    cosim_include = (Path(__file__).resolve().parents[2] / "deps/ibex/dv/cosim")
    cflags = f"{cflags} -I{cosim_include}"
    # The ISA string has to match what the design was elaborated with, or Spike
    # and Ibex disagree about which instructions exist. Upstream derives it from
    # the Verilated model with GetIsaString(); this port has no such pointer, so
    # it is passed in and cosim_glue.cc refuses to compile without it.
    isa = "rv32imc"
    extra_args = [
        "-CFLAGS", f"{cflags} -DCPPTB_COSIM -DCPPTB_COSIM_ISA={isa}",
        "-LDFLAGS", libs,
        *extras["cpp_sources"],
    ]
    rendered = "".join(f'  "{arg}",\n' for arg in extra_args)
    text = text.replace("verilator_args = [\n", "verilator_args = [\n" + rendered, 1)
    return text


COSIM_HEADER = """# Generated by configure.py from ../riscv_arch_tests/cpptb.toml. Do not edit.
#
# The same `small` core as the base project, with Ibex's own co-simulation
# checker bound in. Spike runs in lockstep and every retired instruction and
# every data memory access is compared against it.
#
# This answers the question the plain port cannot. There, both harnesses drive
# the same RTL and agreeing only proves they drove it the same way; neither
# knows what the right answer is. Here the reference does.
#
# Everything SystemVerilog is reused from Ibex unmodified. What is not reusable
# is simple_system_cosim.cc, a subclass of the hand-written harness cpptb
# replaces; ../riscv_arch_tests/cosim_glue.cc takes its place.
"""


def generate(name: str, *, show: bool = False) -> int:
    config = CONFIGS[name]
    if name == DEFAULT_CONFIG:
        print(f"configure: {name} is the base project, nothing to generate",
              file=sys.stderr)
        return 1
    if not BASE.is_file():
        print(f"configure: missing {BASE}", file=sys.stderr)
        return 1

    text = BASE.read_text(encoding="utf-8")
    # Parameters always come from Ibex's own tooling, never from a table here,
    # so a configuration added upstream needs no transcription and one that
    # changes cannot silently diverge.
    text = apply_ibex_config(text, config.ibex_config)
    if config.cosim:
        for pattern, replacement in COSIM_DELTAS:
            text, count = re.subn(pattern, replacement, text, flags=re.M)
            if count != 1:
                print(f"configure: {pattern!r} matched {count} times, "
                      f"expected 1", file=sys.stderr)
                return 1

    # The generated project sits beside the base one, so the testbench is one
    # directory across. Everything else is already relative to open_core_ports.
    text = text.replace('sources = ["testbench.cpp"]',
                        f'sources = ["../{HERE.name}/testbench.cpp"]')
    target = config.cpptb_binary.split("/")[1]
    text = text.replace('directory = "../../work/riscv_arch_tests"',
                        f'directory = "../../work/{target}"')
    text = text.replace('name = "riscv_arch_tests"\ntarget = "riscv_arch_tests"',
                        f'name = "{target}"\ntarget = "{target}"')

    text = apply_extras(text, name, config)

    text = re.sub(r"\A# Derived from ports/ibex_simple_system/cpptb\.toml.*?\n\n",
                  (COSIM_HEADER if config.cosim
                   else HEADER).format(ibex_config=config.ibex_config) + "\n",
                  text, flags=re.S)

    if show:
        print("\n".join(line for line in text.splitlines()
                        if re.match(r"^(RV32|PMP|Branch|Writeback|directory|"
                                    r"name|target|sources = \[\"\.\.)", line)
                        or "ibex_pkg::" in line))
        return 0

    out = HERE.parent / target
    out.mkdir(parents=True, exist_ok=True)
    for source, copied in ((EXTRAS["cosim"]["copy_sources"] if config.cosim
                            else ())):
        (out / copied).write_text(
            (HERE.parent.parent / source.replace("../../", "")).read_text(
                encoding="utf-8"),
            encoding="utf-8")
    (out / "cpptb.toml").write_text(text, encoding="utf-8")
    print(f"wrote {(out / 'cpptb.toml').relative_to(HERE.parent)}")
    print(f"build it with:\n"
          f"  uv run cpptb build --project "
          f"experiments/open_core_ports/ports/{target}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--config", default="bmfull",
                        choices=sorted(c for c in CONFIGS if c != DEFAULT_CONFIG),
                        help="which entry of build_tests.CONFIGS to generate")
    parser.add_argument("--all", action="store_true",
                        help="generate every configuration that needs one")
    parser.add_argument("--show", action="store_true",
                        help="print the changed lines without writing")
    args = parser.parse_args(argv)
    if args.all:
        names = [c for c in sorted(CONFIGS) if c != DEFAULT_CONFIG]
        return max(generate(n, show=args.show) for n in names)
    return generate(args.config, show=args.show)


if __name__ == "__main__":
    raise SystemExit(main())
