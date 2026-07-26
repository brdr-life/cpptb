#!/usr/bin/env python3
"""Assemble riscv-dv generated programs for Ibex Simple System.

Takes the `.S` files generate.py produced and turns each into an ELF and a VMEM
the co-simulation harness can load, with a manifest naming them.

    python3 generate.py --count 10
    python3 build_programs.py

The target adaptation is entirely in target/: an empty user_define.h and
user_init.s for the includes every generated program opens with, a vector table
so the image starts at the base of RAM, and a linker script whose `tohost`
symbol is Simple System's control register. See target/link.ld.

Standard library only, matching the other tools here.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import re
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
TOOLCHAIN = ROOT / "deps" / "riscv_gcc15" / "bin" / "riscv-none-elf-"
TARGET = HERE / "target"
BUILD = HERE / "build"
PROGRAMS = BUILD / "programs"

sys.path.insert(0, str(ROOT))
from bin2vmem import convert as bin_to_vmem  # noqa: E402

RAM_BASE = 0x0010_0000
RAM_BYTES = 1024 * 1024


class BuildError(RuntimeError):
    pass


def toolchain(tool: str) -> Path:
    path = Path(f"{TOOLCHAIN}{tool}")
    if not path.is_file():
        raise BuildError(f"missing {path}\n"
                         f"run: python3 {ROOT / 'fetch.py'} riscv_gcc15")
    return path


# Ibex hardwires mtvec.MODE to vectored and masks BASE to a 256-byte boundary,
# so the generated trap handler has to land on one. riscv-dv emits `.align 2`
# before it, and `--tvec_alignment` is only a soft constraint
# (`vsc.soft(self.tvec_alignment == ...)` in riscv_instr_gen_config.py), so the
# solver is free to ignore it and does: some programs come out 256-aligned by
# luck and others do not.
#
# Rewriting the directive is reliable where the flag is not. It edits a
# generated file under build/, not the fetched tree.
TVEC_ALIGN = re.compile(r"^\.align\s+\d+\s*$\n(?=mtvec_handler:)", re.M)


def align_trap_handler(source: Path) -> Path:
    """Force the trap handler onto a 256-byte boundary.

    Returns the path to assemble, which is a rewritten copy when the directive
    was found. A program with no handler at all is left alone rather than
    silently accepted, since every generated program should have one.
    """
    text = source.read_text(encoding="utf-8")
    if "mtvec_handler:" not in text:
        raise BuildError(f"{source.name}: no mtvec_handler to align")
    patched, count = TVEC_ALIGN.subn(".align 8\n", text)
    if count != 1:
        raise BuildError(
            f"{source.name}: expected one .align before mtvec_handler, "
            f"found {count}; riscv-dv's output has changed shape")
    out = BUILD / "aligned"
    out.mkdir(parents=True, exist_ok=True)
    target = out / source.name
    target.write_text(patched, encoding="utf-8")
    return target


def build_one(source: Path, march: str) -> dict:
    out = BUILD / "elf"
    out.mkdir(parents=True, exist_ok=True)
    elf = out / f"{source.stem}.elf"
    vmem = out / f"{source.stem}.vmem"
    assemble = align_trap_handler(source)

    command = [
        str(toolchain("gcc")),
        # The generated program includes user_define.h and user_init.s by name.
        f"-I{TARGET}",
        f"-T{TARGET / 'link.ld'}",
        "-O0", "-g", "-static", "-nostdlib", "-nostartfiles",
        "-Wl,--no-warn-rwx-segments",
        f"-march={march}", "-mabi=ilp32",
        "-o", str(elf),
        str(assemble),
        str(TARGET / "vectors.S"),
    ]
    completed = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=False)
    if completed.returncode != 0:
        raise BuildError(f"{source.name}: link failed\n{completed.stdout.strip()}")

    raw = out / f"{source.stem}.bin"
    subprocess.run([str(toolchain("objcopy")), "-O", "binary", "--gap-fill", "0",
                    str(elf), str(raw)], check=True)
    data = raw.read_bytes()
    raw.unlink()

    start = read_symbol(elf, "rvdv_vector_table")
    if start != RAM_BASE:
        raise BuildError(
            f"{source.name}: image starts at {start:#x}, not at the RAM base "
            f"{RAM_BASE:#x}; the upstream ELF loader places segments relative "
            f"to the file's lowest address, so it would load shifted")
    if start + len(data) > RAM_BASE + RAM_BYTES:
        raise BuildError(f"{source.name}: image overflows RAM")

    vmem.write_text(bin_to_vmem(data, offset=0), encoding="utf-8")
    return {"name": source.stem, "elf": str(elf.relative_to(HERE)),
            "vmem": str(vmem.relative_to(HERE)), "bytes": len(data)}


def read_symbol(elf: Path, name: str) -> int:
    completed = subprocess.run([str(toolchain("nm")), str(elf)], text=True,
                               stdout=subprocess.PIPE, check=True)
    for line in completed.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise BuildError(f"{elf.name}: no symbol {name}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--march", default="rv32imc_zicsr_zifencei")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--programs", type=Path, default=PROGRAMS)
    args = parser.parse_args(argv)

    sources = sorted(args.programs.glob("gen_*.S"))
    if not sources:
        print(f"build_programs: nothing in {args.programs}\n"
              f"run: python3 {HERE / 'generate.py'}", file=sys.stderr)
        return 1

    built, failures = [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(build_one, s, args.march): s for s in sources}
        for future in concurrent.futures.as_completed(futures):
            source = futures[future]
            try:
                built.append(future.result())
            except (BuildError, subprocess.CalledProcessError) as error:
                failures.append((source.name, str(error)))

    built.sort(key=lambda entry: entry["name"])
    if built:
        manifest = BUILD / "manifest.json"
        manifest.write_text(json.dumps(
            {"ram_base": RAM_BASE, "march": args.march, "programs": built},
            indent=2), encoding="utf-8")
        print(f"built {len(built)} program(s); wrote "
              f"{manifest.relative_to(HERE)}")
    for name, error in failures:
        print(f"  FAILED {name}: {error.splitlines()[0]}", file=sys.stderr)
        if len(failures) == 1:
            print("\n".join(error.splitlines()[1:6]), file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
