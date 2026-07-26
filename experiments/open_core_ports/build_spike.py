#!/usr/bin/env python3
"""Build the Spike reference model used for co-simulation.

Ibex's co-simulation flow needs Spike from lowRISC's ibex_cosim branch, which
adds the cosim API that dv/cosim calls. Upstream's instructions install it into
/opt/spike-cosim with sudo; this installs it under deps/ instead, so nothing
outside this directory is touched and deps/ stays disposable.

    python3 fetch.py spike_cosim     # first
    python3 build_spike.py           # then this, about 5 minutes

Two configure flags are not optional:

  --enable-commitlog    the checker reads Spike's commit log to learn what it
                        did; without this there is nothing to compare against
  --enable-misaligned   Ibex handles misaligned accesses in hardware, so the
                        reference has to as well, or every misaligned access is
                        reported as a mismatch

Standard library only, matching the other tools here.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE / "deps" / "spike_cosim"
BUILD = SOURCE / "build"
PREFIX = SOURCE / "install"

CONFIGURE_FLAGS = ["--enable-commitlog", "--enable-misaligned"]


def environment() -> dict[str, str]:
    """Inherit the caller's environment, plus the no-root prefix if present.

    Spike's configure needs dtc on PATH and its link needs libfdt. Where those
    came from local_deps.py rather than the system, they are not on the default
    search paths.
    """
    env = dict(os.environ)
    sys.path.insert(0, str(HERE))
    try:
        import local_deps
    except ImportError:
        return env
    for name, value in local_deps.environment().items():
        env[name] = f"{value}:{env[name]}" if env.get(name) else value
    return env


def run(command: list[str], cwd: Path, env: dict[str, str], label: str) -> None:
    print(f"spike: {label}")
    completed = subprocess.run(command, cwd=cwd, env=env, text=True,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=False)
    if completed.returncode != 0:
        tail = "\n".join(completed.stdout.strip().splitlines()[-25:])
        raise SystemExit(f"spike: {label} failed:\n{tail}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--clean", action="store_true",
                        help="discard the build directory and start over")
    args = parser.parse_args(argv)

    if not (SOURCE / "configure").is_file():
        print(f"build_spike: no Spike source at {SOURCE}\n"
              f"run: python3 {HERE / 'fetch.py'} spike_cosim", file=sys.stderr)
        return 1

    if args.clean:
        shutil.rmtree(BUILD, ignore_errors=True)
        shutil.rmtree(PREFIX, ignore_errors=True)
    BUILD.mkdir(parents=True, exist_ok=True)

    env = environment()
    if shutil.which("dtc", path=env.get("PATH")) is None:
        print("build_spike: Spike's configure needs the device-tree compiler\n"
              "run: python3 local_deps.py   (or install device-tree-compiler)",
              file=sys.stderr)
        return 1

    if not (BUILD / "config.status").is_file():
        run([str(SOURCE / "configure"), *CONFIGURE_FLAGS, f"--prefix={PREFIX}"],
            BUILD, env, "configure")
    run(["make", f"-j{args.jobs}", "install"], BUILD, env, "build and install")

    pkgconfig = PREFIX / "lib/pkgconfig"
    if not (pkgconfig / "riscv-riscv.pc").is_file():
        print(f"build_spike: install produced no pkg-config files in "
              f"{pkgconfig}", file=sys.stderr)
        return 1

    print(f"\nspike: installed into {PREFIX.relative_to(HERE)}")
    print("configure.py finds it through pkg-config; nothing to export.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
