#!/usr/bin/env python3
"""Resolve the RTL source list with fusesoc and write cpptb.toml from it.

`core_ibex_cpptb.core` describes the design half of this port: ibex_top_tracing
and the wrapper in core_ibex_cpptb_tb_top.sv. fusesoc walks the CAPI=2 graph in
`--setup` mode, which resolves dependencies and writes an EDA description
without invoking a simulator, and this reads the file list back out.

The Ibex parameters do not come from here. They come from `util/ibex_config.py`,
which is the same tool `scripts/compile_tb.py` calls and the same one
`ports/core_ibex_uvm/build_tb.py` asks, so a change to `ibex_configs.yaml`
changes what both harnesses build rather than only one of them. The integer
parameters land in `[design.parameters]` and the enum ones in
`[design.defines]`, which is the split ibex_config.py itself makes: VCS cannot
override an enum parameter on the command line, so upstream passes those as
`+define+IBEX_CFG_*` and the top module reads the macro.

    python3 fusesoc_setup.py                     # print what fusesoc resolves
    python3 fusesoc_setup.py --write             # regenerate cpptb.toml
    python3 fusesoc_setup.py --check             # fail if cpptb.toml is stale
    python3 fusesoc_setup.py --config small      # a different Ibex config

cpptb.toml is committed rather than generated at build time so that
`cpptb build --project .` works with no wrapper script. --check is what keeps it
honest: an upstream change that moves a file or changes a parameter makes the
check fail rather than silently building something else.

Only `opentitan` is committed. Every one of the 944 directed entries carries
`rtl_params: {PMPEnable: 1}`, so none of them is applicable to `small`, and a
`small` build of this port would run nothing. `--config` exists so that can be
demonstrated rather than asserted.

Standard library only apart from the yaml the EDA file is written in, matching
the other tools here.
"""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
IBEX = ROOT / "deps" / "ibex"
SPIKE = ROOT / "deps" / "spike_cosim" / "install"

TOP_CORE = "lowrisc:dv:core_ibex_cpptb:0.1"
TOP_TARGET = "sim"
TOP_MODULE = "core_ibex_cpptb_tb_top"

# common_sim_cfg.hjson passes this to the file-list generator, so it is what
# dvsim would resolve the virtual prim cores to.
PRIM_MAPPING = "lowrisc:prim_generic:all:0.1"

# Ibex pins this in python-requirements.txt.
PINNED = "2.4.3"

# The configuration this port is built for. See the module docstring.
DEFAULT_CONFIG = "opentitan"

# Spike, through pkg-config, exactly as ports/core_ibex_uvm/build_tb.py locates
# it. riscv-fesvr is in upstream's list as a workaround for a CentOS 7 link
# failure and is left out here; nothing in the cosim sources references it.
SPIKE_PACKAGES = ["riscv-riscv", "riscv-disasm", "riscv-fdt"]


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

    fusesoc build roots under deps/ibex are skipped: a previous fusesoc run
    leaves a complete second copy of the tree in `<build-root>/src/`, and
    mapping a source back to a copy of itself would pin the build to whatever
    that run happened to resolve.

    This is the fallback. `_origin` below matches on the path inside the core's
    source directory first, because the basename is not unique: `deps/ibex` has
    a `lint/verilator_waiver.vlt` and a `dv/cs_registers/lint/verilator_waiver.vlt`,
    and picking by name alone silently handed Verilator the wrong one.
    """
    index: dict[str, Path] = {}
    for suffix in ("*.sv", "*.svh", "*.v", "*.vlt"):
        for path in sorted(IBEX.rglob(suffix)):
            if not path.is_file():
                continue
            parts = path.relative_to(IBEX).parts
            if parts and parts[0].startswith("build"):
                continue
            index.setdefault(path.name, path)
    for path in sorted(HERE.glob("*.sv")):
        index[path.name] = path
    return index


def _origin(name: str, index: dict[str, Path]) -> Path:
    """The upstream file a fusesoc `files:` entry was copied from.

    fusesoc copies each core's sources to `src/<vlnv>/<path within the core>`,
    and every Ibex core is rooted at the repository, so stripping the first two
    components gives a path that is valid relative to `deps/ibex`. Only when
    that does not resolve -- which is the case for this port's own wrapper --
    does the basename index get consulted.
    """
    parts = Path(name).parts
    if len(parts) > 2 and parts[0] == "src":
        candidate = IBEX.joinpath(*parts[2:])
        if candidate.is_file():
            return candidate
    origin = index.get(Path(name).name)
    if origin is None:
        raise SetupError(
            f"fusesoc listed {name}, which is under neither {IBEX} nor "
            f"{HERE}; the mapping back needs revisiting")
    return origin


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
        origin = _origin(entry["name"], upstream)
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
    if toplevel != TOP_MODULE:
        raise SetupError(f"{eda_path.name} names toplevel {toplevel!r}, "
                         f"expected {TOP_MODULE!r}")

    return {"sources": sources, "incdirs": incdirs,
            "control": control, "toplevel": toplevel}


def config_parameters(name: str) -> tuple[dict[str, str], dict[str, str]]:
    """Ask Ibex's own tool for a configuration, split into params and defines.

    Identical to ports/core_ibex_uvm/build_tb.py's function of the same name,
    deliberately: the two harnesses have to elaborate the same core or nothing
    measured against each other means anything.
    """
    env = dict(os.environ)
    pylibs = ROOT / "deps" / ".tools" / "pylibs"
    if pylibs.is_dir():
        env["PYTHONPATH"] = f"{pylibs}:{env.get('PYTHONPATH', '')}"
    result = subprocess.run(
        [sys.executable, "util/ibex_config.py",
         "--config_filename", "ibex_configs.yaml", name, "vcs_opts",
         "--ins_hier_path", TOP_MODULE, "--string_define_prefix", "IBEX_CFG_"],
        cwd=IBEX, env=env, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, check=False)
    if result.returncode != 0:
        raise SetupError(f"ibex_config.py {name} failed:\n{result.stderr.strip()}")

    parameters: dict[str, str] = {}
    defines: dict[str, str] = {}
    for token in shlex.split(result.stdout.strip()):
        if token.startswith("-pvalue+"):
            key, _, value = token[len("-pvalue+"):].partition("=")
            parameters[key.split(".")[-1]] = value
        elif token.startswith("+define+"):
            key, _, value = token[len("+define+"):].partition("=")
            defines[key] = value
        else:
            raise SetupError(f"ibex_config.py emitted an unexpected option: {token}")
    if not parameters and not defines:
        raise SetupError(f"ibex_config.py {name} emitted nothing")
    return parameters, defines


def isa_string(defines: dict[str, str], parameters: dict[str, str]) -> str:
    """The Spike ISA string, following core_ibex_base_test::get_isa_string().

    That function reads the elaborated parameters out of the UVM config DB.
    There is no config DB here, so the string is derived from the same two
    dictionaries and written into the build as a define. Getting it wrong makes
    Spike and Ibex disagree about which instructions exist, which surfaces as a
    cosim mismatch a long way from the cause, so it is derived rather than
    written down.
    """
    bitmanip = {
        "ibex_pkg::RV32BNone": "",
        "ibex_pkg::RV32BBalanced": "_Zba_Zbb_Zbs_XZbf_XZbt",
        "ibex_pkg::RV32BOTEarlGrey": "_Zba_Zbb_Zbc_Zbs_XZbf_XZbp_XZbr_XZbt",
        "ibex_pkg::RV32BFull": "_Zba_Zbb_Zbc_Zbs_XZbe_XZbf_XZbp_XZbr_XZbt",
    }
    rv32b = defines.get("IBEX_CFG_RV32B")
    if rv32b not in bitmanip:
        raise SetupError(f"unknown RV32B value {rv32b!r}")
    isa = "rv32" + ("e" if parameters.get("RV32E") == "1" else "i")
    if defines.get("IBEX_CFG_RV32M") != "ibex_pkg::RV32MNone":
        isa += "m"
    return isa + "c" + bitmanip[rv32b]


def pkg_config(*flags: str) -> str:
    env = dict(os.environ)
    env["PKG_CONFIG_PATH"] = str(SPIKE / "lib" / "pkgconfig")
    if not (SPIKE / "lib" / "pkgconfig").is_dir():
        raise SetupError(f"no Spike at {SPIKE}\n"
                         f"run: python3 {ROOT / 'build_spike.py'}")
    result = subprocess.run(["pkg-config", *flags, *SPIKE_PACKAGES], env=env,
                            text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
    if result.returncode != 0:
        raise SetupError(f"pkg-config {' '.join(flags)} failed:\n"
                         f"{result.stderr.strip()}")
    return result.stdout.strip()


def _relative(path: Path) -> str:
    try:
        return str(path.relative_to(HERE))
    except ValueError:
        return os.path.relpath(path, HERE)


HEADER = """\
# Generated by fusesoc_setup.py. Do not edit by hand: run
#
#     python3 fusesoc_setup.py --write --config {config}
#
# The source list, the include directories and the .vlt control files come from
# fusesoc resolving lowrisc:dv:core_ibex_cpptb:0.1, the core file next to this
# one. The parameters and the four IBEX_CFG_* defines come from
# util/ibex_config.py for the `{config}` configuration, which is the same tool
# scripts/compile_tb.py and ports/core_ibex_uvm/build_tb.py call, so both
# harnesses elaborate the same core from the same description.
#
# The Spike flags and the ISA string are here for the same reason. Spike is
# linked into this build directly rather than through DPI: testbench.cpp holds
# a SpikeCosim and calls its C++ interface, where the UVM environment holds a
# chandle and calls the same functions through cosim_dpi.svh.
"""


def render(resolved: dict, config: str, parameters: dict[str, str],
           defines: dict[str, str]) -> str:
    lines = [HEADER.format(config=config), "", "[design]",
             f'top = "{resolved["toplevel"]}"', "defines = ["]
    # RVFI is what ibex_top_tracing fatals without and what declares the rvfi
    # ports; TRACE_EXECUTION is what makes ibex_tracer write
    # trace_core_00000000.log, which the ePMP entries' verdict is read out of.
    # Both are in upstream's ibex_dv_defines.f.
    lines.append('  "RVFI=1",')
    lines.append('  "TRACE_EXECUTION=1",')
    # Verilator defines this itself, so its backend takes the VERILATOR branch
    # of prim_assert.sv. The slang frontend does not, and would otherwise
    # elaborate assertions referring to signals that only exist under
    # INC_ASSERT.
    lines.append('  "VERILATOR=1",')
    for key, value in sorted(defines.items()):
        lines.append(f'  "{key}={value}",')
    lines.append("]")
    lines.append("sources = [")
    for path in resolved["sources"]:
        lines.append(f'  "{_relative(path)}",')
    lines.append("]")
    lines.append("include_dirs = [")
    for path in resolved["incdirs"]:
        lines.append(f'  "{_relative(path)}",')
    lines.append("]")
    lines.append("")
    lines.append("[design.parameters]")
    for key, value in sorted(parameters.items()):
        lines.append(f'{key} = "{value}"')
    lines.append("")
    lines.append("[testbench]")
    lines.append('sources = ["testbench.cpp"]')
    lines.append("")
    lines.append("[build]")
    lines.append(f'directory = "../../work/core_ibex_cpptb"')
    lines.append('name = "core_ibex_cpptb"')
    lines.append('target = "core_ibex_cpptb"')
    lines.append('optimization = "-O2"')
    lines.append("verilator_args = [")
    lines.append('  "-Wno-UNOPTFLAT",')
    lines.append('  "-Wno-fatal",')
    # Spike's headers and libraries, and the ISA string the design was
    # elaborated for. Passed as a bare token and stringified in testbench.cpp:
    # the define travels cpptb.toml -> Verilator -> make -> shell and each hop
    # strips a layer of quoting.
    cflags = pkg_config("--cflags")
    libs = pkg_config("--libs")
    isa = isa_string(defines, parameters)
    lines.append('  "-CFLAGS",')
    lines.append(f'  "{cflags} -I{IBEX}/dv/cosim -DCPPTB_COSIM_ISA={isa}",')
    lines.append('  "-LDFLAGS",')
    lines.append(f'  "{libs}",')
    lines.append(f'  "{_relative(IBEX / "dv" / "cosim" / "spike_cosim.cc")}",')
    for path in resolved["control"]:
        lines.append(f'  "{_relative(path)}",')
    lines.append("]")
    lines.append("")
    lines.append("[run]")
    # run_directed.py gives the testbench its own cycle budget, which is what
    # reports which program hung. This sits above the largest of those so a
    # genuinely stuck simulation is still caught.
    lines.append("timeout_cycles = 200000000")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true",
                        help="regenerate cpptb.toml")
    parser.add_argument("--check", action="store_true",
                        help="fail if cpptb.toml differs from the resolution")
    parser.add_argument("--config", default=DEFAULT_CONFIG,
                        help="Ibex configuration from ibex_configs.yaml")
    args = parser.parse_args()

    build_root = HERE / "build" / "fusesoc"
    try:
        resolved = read(run_setup(build_root))
        parameters, defines = config_parameters(args.config)
        rendered = render(resolved, args.config, parameters, defines)
    except SetupError as error:
        print(f"fusesoc_setup: {error}", file=sys.stderr)
        return 1

    target = HERE / "cpptb.toml"

    if args.write:
        target.write_text(rendered, encoding="utf-8")
        print(f"wrote {target.name}: {len(resolved['sources'])} sources, "
              f"{len(resolved['incdirs'])} include dirs, "
              f"{len(resolved['control'])} vlt files, "
              f"config {args.config}")
        return 0

    if args.check:
        if not target.exists():
            print("fusesoc_setup: cpptb.toml is missing", file=sys.stderr)
            return 1
        if target.read_text(encoding="utf-8") != rendered:
            print("fusesoc_setup: cpptb.toml does not match what fusesoc and "
                  "ibex_config.py resolve; run --write", file=sys.stderr)
            return 1
        print(f"cpptb.toml matches the resolution for --config {args.config}")
        return 0

    print(f"fusesoc {version()} (Ibex pins {PINNED})")
    print(f"toplevel   {resolved['toplevel']}")
    print(f"sources    {len(resolved['sources'])}")
    print(f"incdirs    {len(resolved['incdirs'])}")
    print(f"vlt files  {len(resolved['control'])}")
    print(f"isa        {isa_string(defines, parameters)}")
    for key, value in sorted(parameters.items()):
        print(f"  -G{key}={value}")
    for key, value in sorted(defines.items()):
        print(f"  +define+{key}={value}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
