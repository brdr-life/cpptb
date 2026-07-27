#!/usr/bin/env python3
"""Generate random RISC-V programs with riscv-dv's pyflow.

riscv-dv is vendored inside Ibex at vendor/google_riscv-dv. Its usual flow needs
a SystemVerilog simulator with constrained-random support to run the generator,
which is the part of UVM that does not yet work on Verilator. `pyflow` is a
pure-Python reimplementation of the generator with a pyvsc constraint solver, so
it needs no simulator at all.

    python3 generate.py --count 10 --instructions 400

Getting it to run took three things that are not obvious, all encoded below:

  - **Python 3.11.** pygen_src/isa/riscv_instr.py does `from imp import reload`,
    and `imp` was removed in Python 3.12. uv provisions 3.11 rather than this
    repository changing its own interpreter or the fetched tree being patched.
  - **`--num_of_sub_program 0`.** Any other value reaches `gen_callstack`, which
    fails on current pyvsc with `'riscv_asm_program_gen' object has no attribute
    'callstack_gen'`. The vendored generator expects an older pyvsc. Sub-programs
    are call-stack stress rather than instruction coverage, so dropping them
    costs less than pinning an old solver would.
  - **PYTHONPATH at `pygen/`,** not at the riscv-dv root, because the package is
    `pygen_src`.

Standard library only, matching the other tools here; the generator's own
dependencies are supplied by uv at run time and not installed into this project.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RISCV_DV = ROOT / "deps" / "ibex" / "vendor" / "google_riscv-dv"
BUILD = HERE / "build"

# The generator's dependencies, resolved by uv into a throwaway environment.
# pyvsc is the constraint solver; the rest are what pygen_src imports.
DEPENDENCIES = ["pyvsc", "bitstring==3.1.9", "PyYAML", "tabulate", "pandas"]

# See the module docstring. Both are workarounds, not preferences.
PYTHON = "3.11"
SUB_PROGRAMS = 0

# Ibex hardwires mtvec.MODE to vectored and requires BASE to be 256-byte
# aligned:
#
#     // mtvec.MODE set to vectored
#     // mtvec.BASE must be 256-byte aligned
#
# The generator defaults to a 4-byte alignment, so Ibex masks the low bits of
# whatever it writes and traps land part-way through the program rather than at
# its handler. The symptom is a run that executes correctly for a million
# instructions and then wanders into unwritten memory. 2**8 is 256.
TVEC_ALIGNMENT = 8


def generate(count: int, instructions: int, seed: int, target: str,
             out: Path, interrupts: bool = False,
             signature_addr: str | None = None,
             debug_section: bool = False,
             pygen: Path | None = None,
             name: str = "gen",
             options: list[str] | None = None,
             gen_test: str = "riscv_instr_base_test") -> int:
    # A caller may supply a patched copy of the generator; ports/core_ibex_uvm
    # needs one because pyflow's signature-handshake path does not run as
    # shipped. Everything else uses the fetched tree.
    pygen = pygen or (RISCV_DV / "pygen")
    # A testlist entry names the generator test it wants. `--gen_test` matters
    # as much as the script does: riscv_instr_base_test.py runs its test at
    # import, guarded by `if cfg.argv.gen_test == "riscv_instr_base_test"`, and
    # riscv_rand_instr_test.py imports it. Without the flag both tests run,
    # nested inside each other's multiprocessing pool, and the second one hangs.
    entry = pygen / f"pygen_src/test/{gen_test}.py"
    if not entry.is_file():
        print(f"generate: no {gen_test}.py under {pygen}\n"
              f"if riscv-dv is missing, run: python3 {ROOT / 'fetch.py'} ibex",
              file=sys.stderr)
        return 1
    if shutil.which("uv") is None:
        print("generate: needs uv to provision Python "
              f"{PYTHON}; see the module docstring", file=sys.stderr)
        return 1

    out.mkdir(parents=True, exist_ok=True)
    command = ["uv", "run", "--python", PYTHON, "--no-project"]
    for dependency in DEPENDENCIES:
        command += ["--with", dependency]
    command += [
        "python", str(entry),
        "--num_of_tests", str(count),
        "--instr_cnt", str(instructions),
        "--num_of_sub_program", str(SUB_PROGRAMS),
        "--tvec_alignment", str(TVEC_ALIGNMENT),
        "--target", target,
        "--asm_file_name", name,
        "--gen_test", gen_test,
        "--seed", str(seed),
    ]
    if interrupts:
        # Without this the program installs no interrupt handler, so forcing an
        # interrupt pin traps into code that cannot service it.
        command += ["--enable_interrupt", "1"]
    if debug_section:
        # Ibex's own riscv_dv_extension always emits jumps to `debug_rom` and
        # `debug_exception` at the base of the image, so the section holding
        # those labels has to exist or the program does not link. An "empty"
        # debug ROM is just a dret, which is what upstream generates unless a
        # debug test asks for more.
        command += ["--gen_debug_section", "1"]
    if signature_addr:
        # The handshake core_ibex's UVM testbench watches for. Simple System
        # has nothing that reads it, so this stays off unless a caller asks:
        # ports/core_ibex_uvm does, with the address from that Makefile.
        command += ["--require_signature_addr", "1",
                    "--signature_addr", signature_addr]
    # A caller with generator options of its own -- ports/core_ibex_uvm builds
    # these from a testlist entry's `gen_opts` -- appends them last, so that a
    # flag it names wins over the defaults above: argparse keeps the last
    # occurrence of a repeated option.
    command += list(options or [])

    completed = subprocess.run(
        command, cwd=out, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, check=False,
        env={**_environment(), "PYTHONPATH": str(pygen)})
    if completed.returncode != 0:
        tail = "\n".join(
            line for line in completed.stdout.splitlines()
            if "INFO Registering" not in line)[-2000:]
        print(f"generate: pyflow failed:\n{tail}", file=sys.stderr)
        return 1

    produced = sorted(out.glob(f"{name}_*.S"))
    # A caller outside this port passes an output directory of its own, so the
    # path is only shortened when it happens to sit under this one.
    shown = out.relative_to(HERE) if out.is_relative_to(HERE) else out
    print(f"generated {len(produced)} program(s) into {shown}")
    for path in produced:
        print(f"  {path.name}  {sum(1 for _ in path.open()):>6} lines")
    return 0 if produced else 1


def _environment() -> dict[str, str]:
    import os
    return dict(os.environ)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--count", type=int, default=5,
                        help="programs to generate")
    parser.add_argument("--instructions", type=int, default=400,
                        help="instructions per program")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--target", default="rv32imc",
                        help="riscv-dv target; Ibex's small config is rv32imc")
    parser.add_argument("--out", type=Path, default=BUILD / "programs")
    parser.add_argument("--interrupts", action="store_true",
                        help="generate programs with interrupt handlers, for "
                             "the asynchronous stimulus in the testbench")
    args = parser.parse_args(argv)
    return generate(args.count, args.instructions, args.seed, args.target,
                    args.out, args.interrupts)


if __name__ == "__main__":
    raise SystemExit(main())
