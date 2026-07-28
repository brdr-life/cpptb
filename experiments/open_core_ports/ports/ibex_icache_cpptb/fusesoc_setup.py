#!/usr/bin/env python3
"""Resolve the RTL source list with fusesoc and write cpptb.toml from it.

`ibex_icache_cpptb.core` describes the design half of this port: ibex_icache,
the two primitives tb.sv instantiates beside it, and the wrapper in
ibex_icache_tb_top.sv. fusesoc walks the CAPI=2 graph in `--setup` mode, which
resolves dependencies and writes an EDA description without invoking a
simulator, and this reads the file list back out. Ibex pins fusesoc 2.4.3 in
python-requirements.txt, and ports/ibex_icache_uvm resolves the UVM half the
same way.

    python3 fusesoc_setup.py            # print what fusesoc resolves
    python3 fusesoc_setup.py --write    # regenerate cpptb.toml
    python3 fusesoc_setup.py --check    # fail if cpptb.toml is stale

cpptb.toml is committed rather than generated at build time so that
`cpptb build --project .` works with no wrapper script, which is how every
other cpptb project in this tree works. --check is what keeps it honest: an
upstream change that moves or adds a file makes the check fail rather than
silently building something else.

Two details about what fusesoc hands back, both the same as in
ports/ibex_icache_uvm:

- It copies every source into `<build-root>/src/<core>/`, so the paths in the
  EDA file point at copies. They are mapped back to `deps/ibex` and to this
  directory by basename; all 57 basenames are unique, and an unmappable name
  raises rather than being guessed at.
- It emits `vlt` and `user` file types alongside the SystemVerilog. The `vlt`
  files are Verilator configuration and go on the command line through
  `build.verilator_args`; the `user` entries are Ibex's tool-requirement
  scripts and are not compiled.

Standard library only apart from the yaml the EDA file is written in, matching
the other tools here.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
IBEX = ROOT / "deps" / "ibex"

TOP_CORE = "lowrisc:dv:ibex_icache_cpptb:0.1"
TOP_TARGET = "sim"

# common_sim_cfg.hjson passes this to the file-list generator, so it is what
# dvsim would resolve the virtual prim cores to. Without it fusesoc has no
# provider for lowrisc:prim:ram_1p and the graph does not close.
PRIM_MAPPING = "lowrisc:prim_generic:all:0.1"

# Ibex pins this in python-requirements.txt.
PINNED = "2.4.3"


class SetupError(RuntimeError):
    pass


def _fusesoc() -> str:
    found = shutil.which("fusesoc")
    if found is None:
        raise SetupError(
            "fusesoc is not on PATH; Ibex pins it in python-requirements.txt\n"
            f"install it with: uv tool install 'fusesoc=={PINNED}'\n"
            'or add it with: export PATH="$HOME/.local/bin:$PATH"')
    return found


def version() -> str:
    out = subprocess.run([_fusesoc(), "--version"], text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         check=False)
    return out.stdout.strip()


def run_setup(build_root: Path) -> Path:
    command = [
        _fusesoc(), f"--cores-root={IBEX}", f"--cores-root={HERE}", "run",
        f"--target={TOP_TARGET}", "--tool=verilator", "--setup",
        f"--build-root={build_root}", f"--mapping={PRIM_MAPPING}", TOP_CORE,
    ]
    completed = subprocess.run(command, cwd=IBEX, text=True,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=False)
    if completed.returncode != 0:
        raise SetupError(f"fusesoc setup failed:\n{completed.stdout.strip()}")

    found = sorted(build_root.rglob("*.eda.yml"))
    if len(found) != 1:
        raise SetupError(
            f"expected one .eda.yml under {build_root}, found {len(found)}")
    return found[0]


def _upstream_index() -> dict[str, Path]:
    """Basename -> path, over deps/ibex and this port directory.

    fusesoc build roots under deps/ibex are skipped. A previous fusesoc run
    leaves a complete second copy of the tree in `<build-root>/src/`, and
    mapping a source back to a copy of itself would pin the build to whatever
    that run happened to resolve. Only the checked-out tree counts.
    """
    index: dict[str, Path] = {}
    for path in sorted(IBEX.rglob("*.sv")) + sorted(IBEX.rglob("*.svh")) + \
            sorted(IBEX.rglob("*.v")) + sorted(IBEX.rglob("*.vlt")):
        if not path.is_file():
            continue
        parts = path.relative_to(IBEX).parts
        if parts and parts[0].startswith("build"):
            continue
        index.setdefault(path.name, path)
    for path in sorted(HERE.glob("*.sv")):
        index[path.name] = path
    return index


def read(eda_path: Path) -> dict:
    try:
        import yaml
    except ImportError as error:  # pragma: no cover - environment problem
        raise SetupError(
            "reading fusesoc's output needs PyYAML: uv tool install pyyaml"
        ) from error

    description = yaml.safe_load(eda_path.read_text(encoding="utf-8"))
    base = eda_path.parent
    upstream = _upstream_index()

    sources: list[Path] = []
    incdirs: list[Path] = []
    control: list[Path] = []
    seen_incdir: set[Path] = set()

    for entry in description.get("files", []):
        # `user` entries are Ibex's tool-requirement scripts, not compiled.
        if entry.get("file_type", "") == "user":
            continue
        copied = Path(os.path.normpath(base / entry["name"]))
        origin = upstream.get(copied.name)
        if origin is None:
            raise SetupError(
                f"fusesoc listed {copied.name}, which is under neither {IBEX} "
                f"nor {HERE}; the mapping back needs revisiting")
        kind = entry.get("file_type", "")

        if entry.get("is_include_file"):
            directory = origin.parent
            if directory not in seen_incdir:
                seen_incdir.add(directory)
                incdirs.append(directory)
            continue
        if kind == "vlt":
            control.append(origin)
        elif "systemVerilog" in kind or kind == "verilogSource":
            sources.append(origin)

    toplevel = description.get("toplevel")
    if not toplevel:
        raise SetupError(f"{eda_path.name} names no toplevel")

    return {"sources": sources, "incdirs": incdirs,
            "control": control, "toplevel": toplevel}


def resolve(build_root: Path) -> dict:
    return read(run_setup(build_root))


def _relative(path: Path) -> str:
    try:
        return str(path.relative_to(HERE))
    except ValueError:
        return os.path.relpath(path, HERE)


HEADER = """\
# Generated by fusesoc_setup.py. Do not edit by hand: run
#
#     python3 fusesoc_setup.py --write
#
# The source list, the include directories and the .vlt control files all come
# from fusesoc resolving lowrisc:dv:ibex_icache_cpptb:0.1, which is the core
# file next to this one. ports/ibex_icache_uvm resolves the UVM half of the
# same design the same way, so both harnesses compile the same RTL from the
# same description rather than from two transcriptions of it.
#
# ICacheECC and TweakInfection are localparams of the wrapper rather than
# overrides here, because tb.sv's own defaults are 1 for both and the widths
# derived from them (BusSizeECC, TagSizeECC, LineSizeECC) have to stay
# consistent with the RAM instantiations.
"""

FOOTER = """
[testbench]
sources = ["testbench.cpp"]

[build]
directory = "../../work/ibex_icache_cpptb"
name = "ibex_icache_cpptb"
target = "ibex_icache_cpptb"
optimization = "-O2"

[run]
# The longest of the three tests is about 400,000 cycles at 20ns. This sits
# well above that so a hang is still caught, and testbench.cpp applies a
# tighter limit of its own that says which sequence was running.
timeout_cycles = 20000000
"""

# Verilator flags that are not fusesoc's to supply. The demotions are the ones
# ports/riscv_arch_tests needs for the same RTL.
EXTRA_VERILATOR_ARGS = [
    "-Wno-UNOPTFLAT",
    "-Wno-fatal",
]


def render(resolved: dict) -> str:
    lines = [HEADER, "", "[design]",
             f'top = "{resolved["toplevel"]}"', "sources = ["]
    for path in resolved["sources"]:
        lines.append(f'  "{_relative(path)}",')
    lines.append("]")
    lines.append("include_dirs = [")
    for path in resolved["incdirs"]:
        lines.append(f'  "{_relative(path)}",')
    lines.append("]")
    lines.append("")
    lines.append("[build.__placeholder]")
    text = "\n".join(lines)

    # verilator_args belongs to [build], which the footer opens. Splice the
    # control files in there rather than opening the table twice.
    args = ["verilator_args = ["]
    for flag in EXTRA_VERILATOR_ARGS:
        args.append(f'  "{flag}",')
    for path in resolved["control"]:
        args.append(f'  "{_relative(path)}",')
    args.append("]")
    footer = FOOTER.replace('optimization = "-O2"',
                            'optimization = "-O2"\n' + "\n".join(args))
    return text.replace("\n[build.__placeholder]", "") + footer


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true",
                        help="regenerate cpptb.toml")
    parser.add_argument("--check", action="store_true",
                        help="fail if cpptb.toml differs from the resolution")
    args = parser.parse_args()

    build_root = HERE / "build" / "fusesoc"
    try:
        resolved = resolve(build_root)
    except SetupError as error:
        print(f"fusesoc_setup: {error}", file=sys.stderr)
        return 1

    target = HERE / "cpptb.toml"
    rendered = render(resolved)

    if args.write:
        target.write_text(rendered, encoding="utf-8")
        print(f"wrote {target.name}: {len(resolved['sources'])} sources, "
              f"{len(resolved['incdirs'])} include dirs, "
              f"{len(resolved['control'])} vlt files")
        return 0

    if args.check:
        if not target.exists():
            print("fusesoc_setup: cpptb.toml is missing", file=sys.stderr)
            return 1
        if target.read_text(encoding="utf-8") != rendered:
            print("fusesoc_setup: cpptb.toml does not match what fusesoc "
                  "resolves; run --write", file=sys.stderr)
            return 1
        print("cpptb.toml matches the fusesoc resolution")
        return 0

    print(f"fusesoc {version()} (Ibex pins {PINNED})")
    print(f"toplevel   {resolved['toplevel']}")
    print(f"sources    {len(resolved['sources'])}")
    print(f"incdirs    {len(resolved['incdirs'])}")
    print(f"vlt files  {len(resolved['control'])}")
    for path in resolved["sources"]:
        print(f"  {_relative(path)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
