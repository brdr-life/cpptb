#!/usr/bin/env python3
"""Check that the local toolchain can build and test cpptb.

Runs on a bare checkout with no third-party packages installed, so this module
must stay on the standard library. Every requirement is reported with the
version that was found, the version that is needed, and a platform-appropriate
way to fix it.

    python3 tools/check_environment.py          # required + optional
    python3 tools/check_environment.py --quiet  # only problems
    make doctor

The exit status is non-zero when a required component is missing or too old.
Optional components never change the exit status.
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

# Keep in sync with README.md "Requirements".
MIN_VERILATOR = (5, 46)
MIN_CMAKE = (3, 20)
MIN_PYTHON = (3, 11)
MIN_Z3 = (4, 15, 5)

# Verilator publishes a zero-padded minor field, so "5.46" would not match any
# release name a user could search for.
_SPELLING = {MIN_VERILATOR: "5.046"}

IS_MACOS = platform.system() == "Darwin"
IS_LINUX = platform.system() == "Linux"

OK, MISSING, TOO_OLD, BROKEN = "ok", "missing", "too-old", "broken"
_SYMBOL = {OK: "PASS", MISSING: "MISSING", TOO_OLD: "TOO OLD", BROKEN: "BROKEN"}


@dataclass
class Result:
    name: str
    status: str
    required: bool
    found: str = ""
    needed: str = ""
    hint: str = ""
    notes: list[str] = field(default_factory=list)

    @property
    def failed(self) -> bool:
        return self.status != OK


def _run(command: list[str], env: dict[str, str] | None = None) -> tuple[int, str]:
    try:
        completed = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=60,
            env=env,
        )
    except (OSError, subprocess.SubprocessError) as error:
        return 1, str(error)
    return completed.returncode, completed.stdout.strip()


def _version(text: str) -> tuple[tuple[int, ...], str] | None:
    """Parse the first dotted number, treating each field independently.

    Verilator writes a zero-padded minor field ("5.046"), so the fields must be
    compared as integers rather than as text. The original spelling is kept for
    display so the report matches what the tool and its release notes call it.
    """
    match = re.search(r"(\d+(?:\.\d+)+)", text)
    if match is None:
        return None
    literal = match.group(1)
    return tuple(int(part) for part in literal.split(".")), literal


def _show(version: tuple[int, ...]) -> str:
    """Render a version tuple, preferring the spelling upstream publishes."""
    return _SPELLING.get(version, ".".join(str(part) for part in version))


def _at_least(found: tuple[int, ...], needed: tuple[int, ...]) -> bool:
    width = max(len(found), len(needed))
    return found + (0,) * (width - len(found)) >= needed + (0,) * (width - len(needed))


def _install_hint(brew: str, apt: str) -> str:
    if IS_MACOS:
        return f"brew install {brew}"
    if IS_LINUX:
        return f"sudo apt-get install -y {apt}"
    return f"install {brew}"


def check_tool(
    name: str,
    command: list[str],
    minimum: tuple[int, ...] | None,
    hint: str,
    *,
    required: bool = True,
) -> Result:
    needed = f">= {_show(minimum)}" if minimum else "any"
    if shutil.which(command[0]) is None:
        return Result(name, MISSING, required, needed=needed, hint=hint)
    code, output = _run(command)
    if code != 0:
        return Result(name, BROKEN, required, found=output.splitlines()[0][:60],
                      needed=needed, hint=hint)
    parsed = _version(output)
    if parsed is None:
        return Result(name, OK, required, found=output.splitlines()[0][:40],
                      needed=needed)
    found, literal = parsed
    if minimum and not _at_least(found, minimum):
        return Result(name, TOO_OLD, required, found=literal, needed=needed,
                      hint=hint)
    return Result(name, OK, required, found=literal, needed=needed)


def check_cxx20() -> Result:
    """Compile a real C++20 probe; a compiler's presence says nothing."""
    compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    hint = _install_hint("llvm", "g++")
    if compiler is None:
        return Result("C++20 compiler", MISSING, True, needed="C++20", hint=hint)

    probe = "#include <coroutine>\n#include <concepts>\n" \
            "template <typename T> concept Any = true;\n" \
            "Any auto value() { return 42; }\n" \
            "int main() { return value() == 42 ? 0 : 1; }\n"
    with tempfile.TemporaryDirectory() as work:
        source = Path(work) / "probe.cpp"
        source.write_text(probe, encoding="utf-8")
        code, output = _run(
            [compiler, "-std=c++20", "-fsyntax-only", str(source)]
        )
    _, banner = _run([compiler, "--version"])
    label = banner.splitlines()[0][:48] if banner else compiler
    if code != 0:
        return Result("C++20 compiler", BROKEN, True, found=label,
                      needed="C++20 (coroutines, concepts)",
                      hint=f"{compiler} cannot compile C++20; {hint}",
                      notes=[line for line in output.splitlines()[:3]])
    return Result("C++20 compiler", OK, True, found=label, needed="C++20")


def check_verilator() -> Result:
    result = check_tool(
        "Verilator",
        ["verilator", "--version"],
        MIN_VERILATOR,
        _install_hint("verilator", "verilator")
        + "  # apt ships an older Verilator; build from source if it is < "
        + _show(MIN_VERILATOR),
    )
    if result.status == TOO_OLD:
        result.notes.append(
            "builds pass --no-sched-zero-delay, added in Verilator "
            f"{_show(MIN_VERILATOR)}"
        )
    if result.status == OK and IS_LINUX and shutil.which("verilator"):
        # Verilator sets no -std of its own; cpptb passes -std=c++20 explicitly.
        code, root = _run(["verilator", "--getenv", "VERILATOR_ROOT"])
        if code == 0 and root and not (Path(root) / "include" / "vltstd").is_dir():
            result.status = BROKEN
            result.notes.append(f"VERILATOR_ROOT={root} has no include/vltstd")
    return result


def _pkg_config_env() -> dict[str, str]:
    """Include the z3.pc that `make z3-toolchain` generates.

    The Makefile exports the same directory, so a bare run of this script
    reports what the build will actually resolve.
    """
    env = dict(os.environ)
    generated = Path(__file__).resolve().parents[1] / "build" / "pkgconfig"
    if generated.is_dir():
        existing = env.get("PKG_CONFIG_PATH", "")
        env["PKG_CONFIG_PATH"] = (
            f"{generated}{os.pathsep}{existing}" if existing else str(generated)
        )
    return env


def check_z3() -> Result:
    """Optional: only needed for CPPTB_WITH_Z3 / the z3 constraint backend."""
    # Portable on macOS and Linux, unlike the distribution packages, which are
    # frequently older than MIN_Z3.
    hint = "make z3-toolchain  # installs the portable z3-solver wheel"
    if shutil.which("pkg-config") is None:
        return Result("Z3 (optional)", MISSING, False, needed=f">= {_show(MIN_Z3)}",
                      hint=_install_hint("pkg-config", "pkg-config"))
    env = _pkg_config_env()
    code, output = _run(["pkg-config", "--modversion", "z3"], env=env)
    if code != 0:
        return Result("Z3 (optional)", MISSING, False, needed=f">= {_show(MIN_Z3)}",
                      hint=hint)
    parsed = _version(output)
    if parsed is None or not _at_least(parsed[0], MIN_Z3):
        result = Result("Z3 (optional)", TOO_OLD, False,
                        found=output.strip(), needed=f">= {_show(MIN_Z3)}",
                        hint=hint)
        result.notes.append(
            f"z3::get_full_version() landed in Z3 {_show(MIN_Z3)}; older "
            "releases fail to compile cpptb/z3_random_backend.hpp"
        )
        return result
    return Result("Z3 (optional)", OK, False, found=output.strip(),
                  needed=f">= {_show(MIN_Z3)}")


def collect() -> list[Result]:
    return [
        check_cxx20(),
        check_verilator(),
        check_tool("CMake", ["cmake", "--version"], MIN_CMAKE,
                   _install_hint("cmake", "cmake")),
        check_tool("Python", [sys.executable, "--version"], MIN_PYTHON,
                   _install_hint("python@3.12", "python3")),
        check_tool("uv", ["uv", "--version"], None,
                   "curl -LsSf https://astral.sh/uv/install.sh | sh"),
        check_z3(),
    ]


def report(results: list[Result], quiet: bool) -> int:
    width = max(len(item.name) for item in results)
    problems = [item for item in results if item.failed]

    for item in results:
        if quiet and not item.failed:
            continue
        detail = item.found or "not found"
        if item.needed and item.needed != "any":
            detail = f"{detail} (need {item.needed})"
        print(f"  {_SYMBOL[item.status]:<8} {item.name:<{width}}  {detail}")
        for note in item.notes:
            print(f"           {' ' * width}  - {note}")
        if item.failed and item.hint:
            print(f"           {' ' * width}  -> {item.hint}")

    required = [item for item in problems if item.required]
    optional = [item for item in problems if not item.required]

    print()
    if required:
        names = ", ".join(item.name for item in required)
        print(f"{len(required)} required component(s) need attention: {names}")
    else:
        print("All required components are present.")
    if optional:
        names = ", ".join(item.name for item in optional)
        print(f"{len(optional)} optional component(s) unavailable: {names}")
        print("Optional gaps only disable the features that depend on them.")
    return 1 if required else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--quiet", action="store_true",
                        help="only report components that need attention")
    args = parser.parse_args(argv)

    print(f"cpptb environment check ({platform.system()} {platform.machine()})")
    print()
    return report(collect(), args.quiet)


if __name__ == "__main__":
    raise SystemExit(main())
