#!/usr/bin/env python3
"""Ask fusesoc for the icache testbench's source list.

`dv/uvm/icache` is described by FuseSoC CAPI=2 core files, and fusesoc is what
Ibex's own flow uses: `python-requirements.txt` pins `fusesoc == 2.4.3`. This
runs it in `--setup` mode, which resolves the dependency graph and writes an
EDA description without invoking a simulator, and reads the file list back out.

    python3 fusesoc_setup.py            # print what fusesoc resolves

An earlier version of this port walked the core files itself. That worked --
the two lists agreed on all 101 sources, byte for byte -- but reimplementing a
build system is a liability, and the real one installs with

    uv tool install 'fusesoc==2.4.3'

Two details about what fusesoc hands back:

- It copies every source into `<build-root>/src/<core>/`, so the paths in the
  EDA file point at copies rather than at `deps/ibex`. The overlays in
  build_tb.py are keyed on the upstream path, so the copies are mapped back.
  All 101 basenames are unique, which makes that mapping unambiguous; if that
  ever stops being true this raises rather than guessing.
- It emits `vlt` and `user` file types alongside the SystemVerilog. The `vlt`
  files are Verilator configuration and belong on the command line; the `user`
  entries are Ibex's tool-requirement scripts and are not compiled.

Standard library only apart from the yaml the EDA file is written in, matching
the other tools here.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
IBEX = ROOT / "deps" / "ibex"

TOP_CORE = "lowrisc:dv:ibex_icache_sim:0.1"
TOP_TARGET = "sim"

# Ibex pins this in python-requirements.txt. A different version is not
# necessarily wrong, but it is worth saying so rather than silently building
# something the project does not test.
PINNED = "2.4.3"


class SetupError(RuntimeError):
    pass


def _fusesoc() -> str:
    found = shutil.which("fusesoc")
    if found is None:
        raise SetupError(
            "fusesoc is not on PATH; Ibex pins it in python-requirements.txt\n"
            f"install it with: uv tool install 'fusesoc=={PINNED}'")
    return found


def version() -> str:
    out = subprocess.run([_fusesoc(), "--version"], text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         check=False)
    return out.stdout.strip()


def run_setup(build_root: Path) -> Path:
    """Resolve the graph and return the path to the EDA description."""
    command = [
        _fusesoc(), f"--cores-root={IBEX}", "run",
        f"--target={TOP_TARGET}", "--tool=verilator", "--setup",
        f"--build-root={build_root}", TOP_CORE,
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
    """Basename -> path, for everything fusesoc might have copied."""
    index: dict[str, Path] = {}
    for path in IBEX.rglob("*"):
        if path.is_file():
            index.setdefault(path.name, path)
    return index


def read(eda_path: Path) -> dict:
    """Sources, include directories, Verilator config files and toplevel.

    Paths are mapped back to `deps/ibex` so the overlays in build_tb.py, which
    are keyed on the upstream location, keep matching.
    """
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
        copied = Path(os.path.normpath(base / entry["name"]))
        origin = upstream.get(copied.name)
        if origin is None:
            raise SetupError(
                f"fusesoc listed {copied.name}, which is not under {IBEX}; "
                f"the mapping back to upstream paths needs revisiting")
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
        # `user` entries are Ibex's tool-requirement scripts, not compiled.

    toplevel = description.get("toplevel")
    if not toplevel:
        raise SetupError(f"{eda_path.name} names no toplevel")

    return {"sources": sources, "incdirs": incdirs,
            "control": control, "toplevel": toplevel}


def resolve(build_root: Path) -> dict:
    return read(run_setup(build_root))


def main() -> int:
    build_root = HERE / "build" / "fusesoc"
    try:
        print(f"fusesoc {version()} (Ibex pins {PINNED})")
        resolved = resolve(build_root)
    except SetupError as error:
        print(f"fusesoc_setup: {error}", file=sys.stderr)
        return 1
    print(f"toplevel   {resolved['toplevel']}")
    print(f"sources    {len(resolved['sources'])}")
    print(f"incdirs    {len(resolved['incdirs'])}")
    print(f"vlt files  {len(resolved['control'])}")
    for path in resolved["sources"]:
        print(f"  {path.relative_to(IBEX)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
