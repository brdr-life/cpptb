#!/usr/bin/env python3
"""Install the system packages these ports need, without root.

Three of the upstream flows here link against system libraries. Ibex's Verilator
harness needs libelf and lz4 to load an ELF at run time, and Spike needs the
device-tree compiler to configure and libfdt to link. On a machine where you can
run `sudo apt install`, do that instead and skip this entirely.

Where you cannot, `apt-get download` still works as an ordinary user: it fetches
the .deb without installing it, and `dpkg -x` unpacks one into any directory. So
this assembles a private prefix under deps/.tools/root and prints the
environment that points the builds at it. Nothing outside deps/ is touched, and
deps/ is disposable.

    python3 local_deps.py            # fetch and unpack
    python3 local_deps.py --env      # print the environment to export
    eval "$(python3 local_deps.py --env)"

Linux and dpkg only. On macOS the same libraries come from Homebrew:

    brew install dtc libelf lz4

Standard library only, matching fetch.py.
"""

from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE / "deps" / ".tools"
CACHE = TOOLS / "pkg"
PREFIX = TOOLS / "root"

# Why each is here, so a future reader can tell whether it is still needed.
#
# Each entry is the candidate names in preference order, because Ubuntu's 64-bit
# time_t transition renamed several of these: libelf1 became libelf1t64 in 24.04
# and does not exist under the old name. Resolving against apt rather than
# pinning one spelling keeps this working across releases.
PACKAGES = {
    "Spike's configure aborts without dtc": ["device-tree-compiler"],
    "Spike links against libfdt": ["libfdt1"],
    "Ibex's verilator_memutil loads firmware from an ELF":
        ["libelf-dev"],
    "the shared library for the above": ["libelf1t64", "libelf1"],
    "libelf on Ubuntu is built against lz4": ["liblz4-dev"],
    "the lz4 shared library": ["liblz4-1"],
    "libelf is built against zlib": ["zlib1g-dev"],
}


def resolve(candidates: list[str]) -> str | None:
    """The first candidate apt actually offers on this release."""
    for name in candidates:
        completed = subprocess.run(["apt-cache", "policy", name], text=True,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.DEVNULL, check=False)
        for line in completed.stdout.splitlines():
            if line.strip().startswith("Candidate:"):
                value = line.split(":", 1)[1].strip()
                if value and value != "(none)":
                    return name
    return None


def environment() -> dict[str, str]:
    """Where the unpacked packages ended up, in the form builds expect."""
    include = PREFIX / "usr/include"
    lib = PREFIX / "usr/lib/x86_64-linux-gnu"
    return {
        "PATH": f"{PREFIX / 'usr/bin'}",
        "C_INCLUDE_PATH": f"{include}",
        "CPLUS_INCLUDE_PATH": f"{include}",
        "LIBRARY_PATH": f"{lib}",
        "LD_LIBRARY_PATH": f"{lib}",
        "PKG_CONFIG_PATH": f"{lib / 'pkgconfig'}",
    }


def print_env() -> None:
    """Print shell that prepends our prefix rather than replacing anything."""
    for name, value in environment().items():
        print(f'export {name}="{value}${{{name}:+:${{{name}}}}}"')


def install() -> int:
    if platform.system() != "Linux" or shutil.which("dpkg-deb") is None:
        print("local_deps: needs Linux with dpkg; on macOS run\n"
              "  brew install dtc libelf lz4", file=sys.stderr)
        return 1

    CACHE.mkdir(parents=True, exist_ok=True)
    PREFIX.mkdir(parents=True, exist_ok=True)

    wanted, missing = [], []
    for reason, candidates in PACKAGES.items():
        name = resolve(candidates)
        if name is None:
            missing.append(f"{' or '.join(candidates)} ({reason})")
        else:
            wanted.append(name)
    if missing:
        print("local_deps: apt offers none of these:", file=sys.stderr)
        for entry in missing:
            print(f"  {entry}", file=sys.stderr)
        return 1

    completed = subprocess.run(
        ["apt-get", "download", *wanted], cwd=CACHE, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    if completed.returncode != 0:
        print(f"local_deps: apt-get download failed:\n{completed.stdout}",
              file=sys.stderr)
        return 1

    debs = sorted(CACHE.glob("*.deb"))
    if not debs:
        print("local_deps: apt-get download produced no .deb files",
              file=sys.stderr)
        return 1
    for deb in debs:
        subprocess.run(["dpkg", "-x", str(deb), str(PREFIX)], check=True)

    print(f"unpacked {len(debs)} package(s) into "
          f"{PREFIX.relative_to(HERE)}\n")
    print("point builds at it with:\n")
    print_env()
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--env", action="store_true",
                        help="print the environment and do nothing else")
    args = parser.parse_args(argv)
    if args.env:
        print_env()
        return 0
    return install()


if __name__ == "__main__":
    raise SystemExit(main())
