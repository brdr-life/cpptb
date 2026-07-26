#!/usr/bin/env python3
"""Build riscv-dv programs for the core_ibex UVM testbench.

`ports/riscv_dv` already generates random programs with riscv-dv's pyflow and
links them for Simple System, whose RAM starts at 0x0010_0000. core_ibex maps
its memory at 0x8000_0000 and talks to the program through a signature address
rather than an HTIF `tohost` write, so the same generated assembly needs a
different link and different generator options.

    python3 build_programs.py --count 3 --instructions 400

Generation is `ports/riscv_dv/generate.py` -- the same pyflow invocation, with
the signature handshake turned on -- and linking uses riscv-dv's own
`scripts/link.ld`, which already targets 0x8000_0000. No target adaptation of
our own: unlike Simple System, core_ibex is the platform riscv-dv's generator
was written for.

Standard library only, matching the other tools here.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RISCV_DV = ROOT / "deps" / "ibex" / "vendor" / "google_riscv-dv"
TOOLCHAIN = ROOT / "deps" / "riscv_gcc15" / "bin" / "riscv-none-elf-"
LINKER = RISCV_DV / "scripts" / "link.ld"
BUILD = HERE / "build"
PROGRAMS = BUILD / "programs"

sys.path.insert(0, str(ROOT / "ports" / "riscv_dv"))

# dv/uvm/core_ibex/Makefile: SIGNATURE_ADDR := 8ffffffc. The testbench watches
# for writes there and at four bytes below it, so the generator and the run
# have to agree; run_test.py passes the same value to both.
SIGNATURE_ADDR = "8ffffffc"

# The same directive rewrite as ports/riscv_dv/build_programs.py, repeated
# rather than imported: both files are called build_programs, so the import
# resolves to whichever one is running.
TVEC_ALIGN = re.compile(r"^\.align\s+\d+\s*$\n(?=mtvec_handler:)", re.M)


class BuildError(RuntimeError):
    pass


def toolchain(tool: str) -> Path:
    path = Path(f"{TOOLCHAIN}{tool}")
    if not path.is_file():
        raise BuildError(f"missing {path}\n"
                         f"run: python3 {ROOT / 'fetch.py'} riscv_gcc15")
    return path


# pyflow's signature-handshake path does not run as shipped:
#
#     elif signature_type == test_result_t.TEST_RESULT:
#     AttributeError: TEST_RESULT
#
# `test_result_t` holds TEST_PASS and TEST_FAIL; TEST_RESULT is a member of
# `signature_type_t`, which every other arm of the same `if` chain uses. It is a
# one-word typo on a path nothing exercises, because the SystemVerilog
# generator is what upstream runs and pyflow is the reimplementation.
#
# The generator is Python, so this patches a copy under build/ and points
# PYTHONPATH at that, leaving deps/ untouched, and fails loudly if the line
# moves -- the same discipline as the SystemVerilog overlays in build_tb.py.
PYGEN_PATCHES = [
    (
        "riscv_asm_program_gen.py",
        "            elif signature_type == test_result_t.TEST_RESULT:\n",
        "            elif signature_type == signature_type_t.TEST_RESULT:\n",
    ),
    # The second one on the same path: `("a string")` is a string, not a
    # one-element tuple, so list.extend walks it character by character and the
    # program comes out with one letter per line.
    #
    #     l
    #     i
    #     x
    #     2
    #
    (
        "riscv_asm_program_gen.py",
        '            instr.extend(("li x{}, {}".format('
        'cfg.gpr[1], hex(cfg.signature_addr))))\n',
        '            instr.extend(["li x{}, {}".format('
        'cfg.gpr[1], hex(cfg.signature_addr))])\n',
    ),
    # And the third: `core_is_initialized` appends the whole instruction list
    # as one element of the output stream, so the program gets a line reading
    # `['li x22, 0x8ffffffc', 'li x27, 0x0', ...]`. Every other producer in the
    # file extends.
    (
        "riscv_asm_program_gen.py",
        "                self.format_section(instr)\n"
        "                self.instr_stream.append(instr)\n",
        "                self.format_section(instr)\n"
        "                self.instr_stream.extend(instr)\n",
    ),
    # Not a bug: the piece of Ibex's own riscv-dv customisation that a program
    # cannot run without. `ibex_asm_program_gen.sv` overrides
    # gen_program_header to put the two debug-ROM jumps at 0x0 and 0x8 and to
    # align `_start` to 0x80, because Ibex takes its reset vector at
    # boot_addr + 0x80. pyflow is a separate implementation and reads none of
    # that, so `_start` lands at 0x8000_0000 and the core fetches zeros from
    # 0x8000_0080. This transcribes those five directives.
    #
    # Upstream jumps to `debug_rom` and `debug_exception`, which its own
    # generator emits. pyflow cannot: `gen_debug_rom` is `# TODO / pass`, and
    # the rv32imc target sets support_debug_mode = 0. So the two entry points
    # are self-loops here. They keep the addresses Spike is built against
    # (DEBUG_ROM_ENTRY and DEBUG_ROM_TVEC) occupied, and nothing fetches them
    # without debug stimulus; if something does, the core visibly spins rather
    # than running whatever happened to be there.
    #
    # ports/riscv_dv/README.md notes that pyflow generates generic RV32IMC
    # programs rather than Ibex-tuned ones. This closes the part of that gap
    # that stops a program running at all; the directed instruction library and
    # the debug ROM are still not reproduced.
    (
        "riscv_asm_program_gen.py",
        '        self.instr_stream.extend((".include \\"user_define.h\\"",'
        ' ".globl _start", ".section .text"))\n',
        '        self.instr_stream.extend((".include \\"user_define.h\\"",'
        ' ".globl _start", ".section .text"))\n'
        '        self.instr_stream.extend((".option norvc",\n'
        '                                  "debug_rom: j debug_rom",\n'
        '                                  ".align 3",\n'
        '                                  "debug_exception: j debug_exception",\n'
        '                                  ".align 7",\n'
        '                                  ".option rvc"))\n',
    ),
]


def patched_pygen() -> Path:
    import shutil

    source = RISCV_DV / "pygen"
    target = BUILD / "pygen"
    if not source.is_dir():
        raise BuildError(f"no riscv-dv at {RISCV_DV}\n"
                         f"run: python3 {ROOT / 'fetch.py'} ibex")
    if target.is_dir():
        shutil.rmtree(target)
    shutil.copytree(source, target)

    for name, old, new in PYGEN_PATCHES:
        path = target / "pygen_src" / name
        text = path.read_text(encoding="utf-8")
        if text.count(old) != 1:
            raise BuildError(
                f"{name}: a line this build patches is no longer present "
                f"exactly once; riscv-dv has changed and PYGEN_PATCHES needs "
                f"revisiting")
        path.write_text(text.replace(old, new), encoding="utf-8")
    return target


def generate(count: int, instructions: int, seed: int, target: str) -> int:
    from generate import generate as pyflow_generate  # noqa: E402

    return pyflow_generate(count, instructions, seed, target, PROGRAMS,
                           interrupts=False,
                           signature_addr=SIGNATURE_ADDR,
                           debug_section=True,
                           pygen=patched_pygen())


def align_trap_handler(source: Path) -> Path:
    """Force the trap handler onto a 256-byte boundary.

    The same fix, and the same reason, as ports/riscv_dv/build_programs.py:
    Ibex masks mtvec.BASE to 256 bytes and `--tvec_alignment` is only a soft
    constraint, so the generator honours it or not by luck. Rewriting the
    directive is reliable where the flag is not.
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
    source = align_trap_handler(source)

    command = [
        str(toolchain("gcc")),
        f"-I{RISCV_DV}/user_extension",
        f"-T{LINKER}",
        "-O0", "-g", "-static", "-nostdlib", "-nostartfiles",
        "-Wl,--no-warn-rwx-segments",
        f"-march={march}", "-mabi=ilp32",
        "-o", str(elf),
        str(source),
    ]
    completed = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, check=False)
    if completed.returncode != 0:
        raise BuildError(f"{source.name}: link failed\n{completed.stdout.strip()}")

    # `+bin=` is a flat binary, not an ELF: core_ibex_base_test loads it with
    # $fread byte by byte from `BOOT_ADDR, and hands the same file to the cosim
    # agent. Passing an ELF leaves memory at zero, and the run dies a long way
    # downstream with the core executing 0x00000000 at 0x8000_0000 and the
    # double-fault detector hitting its threshold.
    raw = elf.with_suffix(".bin")
    subprocess.run([str(toolchain("objcopy")), "-O", "binary", "--gap-fill", "0",
                    str(elf), str(raw)], check=True)
    return {"name": elf.stem,
            "elf": str(elf.relative_to(HERE)),
            "bin": str(raw.relative_to(HERE)),
            "bytes": raw.stat().st_size}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--count", type=int, default=3)
    parser.add_argument("--instructions", type=int, default=400)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--target", default="rv32imc")
    parser.add_argument("--march", default="rv32imc_zicsr_zifencei")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--skip-generate", action="store_true",
                        help="link what is already in build/programs")
    args = parser.parse_args(argv)

    if not args.skip_generate:
        status = generate(args.count, args.instructions, args.seed, args.target)
        if status:
            return status

    sources = sorted(PROGRAMS.glob("gen_*.S"))
    if not sources:
        print(f"build_programs: nothing in {PROGRAMS}", file=sys.stderr)
        return 1

    built, failures = [], []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(build_one, s, args.march): s for s in sources}
        for future in concurrent.futures.as_completed(futures):
            try:
                built.append(future.result())
            except (BuildError, subprocess.CalledProcessError) as error:
                failures.append((futures[future].name, str(error)))

    built.sort(key=lambda entry: entry["name"])
    if built:
        manifest = BUILD / "manifest.json"
        manifest.write_text(json.dumps(
            {"signature_addr": SIGNATURE_ADDR, "march": args.march,
             "programs": built}, indent=2), encoding="utf-8")
        print(f"built {len(built)} program(s); wrote "
              f"{manifest.relative_to(HERE)}")
    for name, error in failures:
        print(f"  FAILED {name}: {error.splitlines()[0]}", file=sys.stderr)
        if len(failures) == 1:
            print("\n".join(error.splitlines()[1:6]), file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
