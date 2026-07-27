#!/usr/bin/env python3
"""Build riscv-dv programs for the core_ibex UVM testbench.

`ports/riscv_dv` already generates random programs with riscv-dv's pyflow and
links them for Simple System, whose RAM starts at 0x0010_0000. core_ibex maps
its memory at 0x8000_0000 and talks to the program through a signature address
rather than an HTIF `tohost` write, so the same generated assembly needs a
different link and different generator options.

    python3 build_programs.py --count 3 --instructions 400
    python3 build_programs.py --test riscv_arithmetic_basic_test
    python3 build_programs.py --all-tests

Generation is `ports/riscv_dv/generate.py` -- the same pyflow invocation, with
the signature handshake turned on -- and linking uses riscv-dv's own
`scripts/link.ld`, which already targets 0x8000_0000. No target adaptation of
our own: unlike Simple System, core_ibex is the platform riscv-dv's generator
was written for.

`--test` and `--all-tests` build the program a named entry in upstream's
`riscv_dv_extension/testlist.yaml` asks for, translating that entry's
`gen_opts` into pyflow flags. Upstream runs the SystemVerilog generator with
Ibex's own SystemVerilog extension; pyflow is a separate implementation and
reads neither, so some options have no equivalent. Those are not dropped
silently: every one is recorded in `build/manifest.json` against the test it
came from, and `run_tests.py` reports which tests are running a program that
does not match their intent.

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
import threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RISCV_DV = ROOT / "deps" / "ibex" / "vendor" / "google_riscv-dv"
PYGEN_SRC = RISCV_DV / "pygen" / "pygen_src"
TESTLIST = ROOT / "deps/ibex/dv/uvm/core_ibex/riscv_dv_extension/testlist.yaml"
TOOLCHAIN = ROOT / "deps" / "riscv_gcc15" / "bin" / "riscv-none-elf-"
LINKER = RISCV_DV / "scripts" / "link.ld"
BUILD = HERE / "build"
PROGRAMS = BUILD / "programs"
MANIFEST = BUILD / "manifest.json"

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
    # The second piece of Ibex's extension a program cannot finish without.
    # `ibex_asm_program_gen.sv` empties `gen_test_done` and puts its own
    # `gen_test_end` at the `test_done:` label: a write of
    # `(TEST_PASS << 8) | TEST_RESULT` to `signature_addr - 0x4`, which is the
    # one thing `core_ibex_base_test::wait_for_test_done` waits for.
    #
    # pyflow generates riscv-dv's stock ending instead -- `li gp, 1; ecall`,
    # and an ecall handler that jumps to `write_tohost` -- which is the HTIF
    # handshake Spike and Simple System use and core_ibex has nothing that
    # reads. The core reaches the end of the program and spins in
    # `write_tohost` forever:
    #
    #     800003fc  auipc  x30,0x2
    #     80000400  sw     x3,-1020(x30)   PA:0x80002000
    #     80000404  c.j    800003fc
    #
    # and the run ends on `TEST TIMEOUT!!` having executed the whole program
    # correctly. Every test in the sweep ended this way.
    (
        "riscv_asm_program_gen.py",
        "        if cfg.bare_program_mode:\n"
        '            self.instr_stream.append(pkg_ins.indent + "j write_tohost")\n'
        "        else:\n"
        '            self.instr_stream.append(pkg_ins.indent + "ecall")\n',
        "        if cfg.bare_program_mode:\n"
        '            self.instr_stream.append(pkg_ins.indent + "j write_tohost")\n'
        "        else:\n"
        "            if cfg.require_signature_addr:\n"
        "                self.instr_stream.extend(\n"
        '                    ("{}li x{}, {}".format(pkg_ins.indent, cfg.gpr[1],\n'
        "                                          hex(cfg.signature_addr - 4)),\n"
        '                     "{}li x{}, {}".format(pkg_ins.indent, cfg.gpr[0],\n'
        "                                          int(test_result_t.TEST_PASS)),\n"
        '                     "{}slli x{}, x{}, 8".format(pkg_ins.indent,\n'
        "                                                cfg.gpr[0], cfg.gpr[0]),\n"
        '                     "{}addi x{}, x{}, {}".format(\n'
        "                         pkg_ins.indent, cfg.gpr[0], cfg.gpr[0],\n"
        "                         hex(int(signature_type_t.TEST_RESULT))),\n"
        '                     "{}sw x{}, 0(x{})".format(pkg_ins.indent,\n'
        "                                              cfg.gpr[0], cfg.gpr[1])))\n"
        '            self.instr_stream.append(pkg_ins.indent + "ecall")\n',
    ),
    # Forty of the fifty-seven testlist entries name `riscv_rand_instr_test` as
    # their generator test, and pyflow has that test: the base program plus
    # three directed instruction streams. It also hardcodes two settings over
    # the top of the command line, one of which is the sub-program count that
    # `gen_callstack` cannot survive on current pyvsc, and the other of which
    # would make every entry's `+instr_cnt` a no-op.
    (
        "test/riscv_rand_instr_test.py",
        "        cfg.instr_cnt = 10000\n"
        "        cfg.num_of_sub_program = 5\n",
        "        cfg.instr_cnt = cfg.argv.instr_cnt\n"
        "        cfg.num_of_sub_program = 0\n",
    ),
    # `randomize_avail_regs` is `pass  # TODO` in pyflow, with the constraint
    # the SystemVerilog uses left in the file as a comment. The effect is not
    # that streams get a worse choice of registers: it is that they get an
    # unusable one. `avail_regs` is then a random ten-tuple of `riscv_reg_t`
    # with nothing excluded, so it can consist entirely of x0 and the reserved
    # registers, and `randomize_gpr` -- which asks for `rd inside avail_regs`
    # and `rd != reserved_rd[i]` in the same call -- has nothing to solve for:
    #
    #     vsc.model.solve_failure.SolveFailure: solve failure
    #       riscv_load_store_instr_lib.py, in post_randomize
    #       add_mixed_instr -> randomize_instr -> randomize_gpr
    #
    # It is per-stream-instance luck, so it shows up as an instruction-count
    # limit: with directed streams mixed in, generation failed above about
    # 2,000 instructions and always failed at 10,000. This is the commented-out
    # constraint written out in Python -- ten unique registers, none reserved,
    # the first in x8-x15 so the compressed formats have something to use --
    # rather than in pyvsc, which is what the TODO says did not work.
    (
        "riscv_instr_stream.py",
        "    def randomize_avail_regs(self):\n"
        "        pass\n",
        "    def randomize_avail_regs(self):\n"
        "        if self.avail_regs.size > 0:\n"
        "            excluded = set([riscv_reg_t.ZERO])\n"
        "            excluded.update(list(cfg.reserved_regs))\n"
        "            excluded.update(list(self.reserved_rd))\n"
        "            pool = [reg for reg in riscv_reg_t if reg not in excluded]\n"
        "            head = [reg for reg in pool\n"
        "                    if riscv_reg_t.S0 <= reg <= riscv_reg_t.A5]\n"
        "            size = min(len(self.avail_regs), len(pool))\n"
        "            picked = random.sample(pool, size)\n"
        "            if head and picked[0] not in head:\n"
        "                first = random.choice(head)\n"
        "                picked = [first] + [reg for reg in picked\n"
        "                                    if reg != first][:size - 1]\n"
        "            for i in range(size):\n"
        "                self.avail_regs[i] = picked[i]\n",
    ),
    # The same shape of gap one file over, and this one produces a program that
    # does not assemble. `riscv_load_store_base_instr_stream` has its `rs1_c`
    # constraint commented out -- "TODO Getting pyvsc error --> rs1 has not been
    # build yet" -- so the base register of a load/store stream is drawn with
    # x0 and the reserved registers still in the running:
    #
    #     Error: illegal operands `la zero,region_1+4062'
    #
    # x0 is the visible half. The other half is that the stream can pick one of
    # cfg.reserved_regs, which is where the signature handshake keeps its
    # address, and then write to it. Redrawn here after randomization rather
    # than constrained during it, for the reason the TODO gives.
    (
        "riscv_load_store_instr_lib.py",
        "    def post_randomize(self):\n"
        "        self.randomize_offset()\n",
        "    def post_randomize(self):\n"
        "        excluded = set([riscv_reg_t.ZERO])\n"
        "        excluded.update(list(cfg.reserved_regs))\n"
        "        excluded.update(list(self.reserved_rd))\n"
        "        if self.rs1_reg in excluded:\n"
        "            self.rs1_reg = random.choice(\n"
        "                [reg for reg in riscv_reg_t if reg not in excluded])\n"
        "        self.randomize_offset()\n",
    ),
    # `build_basic_instruction_list` adds the optional instructions to the pool
    # by name and gets the type wrong every time: the pool holds
    # `riscv_instr_name_t` members, and these lines push strings and, twice, a
    # whole list as one element. A test asking for one of them dies in
    # `get_rand_instr`:
    #
    #     instr_h = copy.deepcopy(cls.instr_template[name])
    #     KeyError: 'WFI'
    #
    # That is `+no_wfi=0`, `+no_ebreak=0` and `+no_dret=0`, eight testlist
    # entries between them. The CSR line in the same run of code is left exactly
    # as it is; see NEVER_GENERATED.
    (
        "isa/riscv_instr.py",
        "        if cfg.no_ebreak == 0:\n"
        '            cls.basic_instr.append("EBREAK")\n'
        "            for _ in rcs.supported_isa:\n"
        "                if(riscv_instr_group_t.RV32C in rcs.supported_isa and\n"
        "                   not(cfg.disable_compressed_instr)):\n"
        '                    cls.basic_instr.append("C_EBREAK")\n'
        "                    break\n"
        "        if cfg.no_dret == 0:\n"
        '            cls.basic_instr.append("DRET")\n'
        "        if cfg.no_fence == 0:\n"
        '            cls.basic_instr.append(cls.instr_category["SYNCH"])\n',
        "        if cfg.no_ebreak == 0:\n"
        "            cls.basic_instr.append(riscv_instr_name_t.EBREAK)\n"
        "            for _ in rcs.supported_isa:\n"
        "                if(riscv_instr_group_t.RV32C in rcs.supported_isa and\n"
        "                   not(cfg.disable_compressed_instr)):\n"
        "                    cls.basic_instr.append(riscv_instr_name_t.C_EBREAK)\n"
        "                    break\n"
        "        if cfg.no_dret == 0:\n"
        "            cls.basic_instr.append(riscv_instr_name_t.DRET)\n"
        "        if cfg.no_fence == 0:\n"
        '            cls.basic_instr.extend(cls.instr_category["SYNCH"])\n',
    ),
    (
        "isa/riscv_instr.py",
        "        if cfg.no_wfi == 0:\n"
        '            cls.basic_instr.append("WFI")\n',
        "        if cfg.no_wfi == 0:\n"
        "            cls.basic_instr.append(riscv_instr_name_t.WFI)\n",
    ),
    # The bug behind most of the generation failures, and the one that took
    # pyvsc's own diagnostics to see:
    #
    #     Problem Set: 2 constraints
    #       if ((instr_name == 242)) { (rd == 2); }
    #       (rd != reserved_rd.reserved_rd[0]);
    #
    # 242 is C_ADDI16SP, whose rd is architecturally SP, and reserved_rd[0] is
    # SP because the load/store stream around it took SP as its base register.
    # `randomize_instr` has the guard for exactly this, and excludes the four
    # SP-forcing compressed instructions when SP is reserved -- but
    # `get_rand_instr` never applies `exclude_instr`. It builds
    # `disallowed_instr`, tests whether the list is empty, and then, in the
    # branch for when it is not, picks a name at random from the pool it was
    # about to filter. So the exclusion has no effect and the solver is handed
    # a contradiction, which is a coin flip per stream and made generation fail
    # about half the time above a couple of thousand instructions.
    (
        "isa/riscv_instr.py",
        "                name = random.choice(cls.instr_names)\n"
        "                if len(include_instr) > 0:\n"
        "                    name = random.choice(include_instr)\n"
        "                if len(allowed_instr) > 0:\n"
        "                    name = random.choice(allowed_instr)\n",
        "                candidates = list(cls.instr_names)\n"
        "                if len(include_instr) > 0:\n"
        "                    candidates = list(include_instr)\n"
        "                if len(allowed_instr) > 0:\n"
        "                    candidates = list(allowed_instr)\n"
        "                candidates = [c for c in candidates\n"
        "                              if c not in disallowed_instr]\n"
        "                name = random.choice(candidates)\n",
    ),
    # `gen_load_store_instr` picks the instructions legal for each address it
    # generated, and builds the list with `extend` on one list created before
    # the loop instead of a fresh list per address. The SystemVerilog assigns
    # `allowed_instr = {LB, LBU, SB};` at the top of its foreach. So a
    # compressed form allowed by one offset stays allowed for every offset
    # after it, and the program does not assemble:
    #
    #     Error: illegal operands `c.lwsp a5,25(sp)'
    #     Error: illegal operands `c.lw a2,-116(a0)'
    #
    # C.LWSP takes a multiple of four in 0-252 and C.LW a multiple of four in
    # 0-124; both were added to the pool for an earlier address that satisfied
    # `offset in range(128) and offset % 4 == 0`.
    (
        "riscv_load_store_instr_lib.py",
        "        for i in range(len(self.addr)):\n"
        "            # Assign the allowed load/store instructions based on "
        "address alignment\n",
        "        for i in range(len(self.addr)):\n"
        "            allowed_instr = []\n"
        "            # Assign the allowed load/store instructions based on "
        "address alignment\n",
    ),
    # `get_load_store_instr` and `get_instr` take a shallow copy of the
    # instruction template. `get_rand_instr` takes a deep one, with a comment
    # saying why: these are pyvsc `randobj`s, so a shallow copy shares the
    # field objects with the template and every instance of one instruction
    # name ends up with the last `rs1` any of them was given. What that looks
    # like in a program is a load/store stream whose base register changes
    # part-way through:
    #
    #     la    s5, region_1+3568   #start riscv_load_store_rand_instr_stream_0
    #     ...
    #     lhu   s3, -132 (a1)       #end riscv_load_store_rand_instr_stream_0
    #
    # `a1` was never initialised as a pointer -- it still holds the constant
    # the init section put there -- so the program reads a wild address, the
    # testbench's memory agent answers it, Spike takes a load access fault, and
    # the cosim scoreboard reports a trap the DUT did not report.
    (
        "isa/riscv_instr.py",
        "        name = load_store_instr[cls.idx]\n"
        "        instr_h = copy.copy(cls.instr_template[name])\n",
        "        name = load_store_instr[cls.idx]\n"
        "        instr_h = copy.deepcopy(cls.instr_template[name])\n",
    ),
    (
        "isa/riscv_instr.py",
        "            sys.exit(1)\n"
        "        instr_h = copy.copy(cls.instr_template[name])\n",
        "            sys.exit(1)\n"
        "        instr_h = copy.deepcopy(cls.instr_template[name])\n",
    ),
    # And the reason the exclusion would still not have worked: the same
    # string-for-enum mistake as the basic instruction list. `exclude_instr`
    # is matched against `instr_names`, which holds `riscv_instr_name_t`
    # members.
    (
        "riscv_instr_stream.py",
        "            exclude_instr.append(riscv_instr_name_t.C_ADDI4SPN.name)\n"
        "            exclude_instr.append(riscv_instr_name_t.C_ADDI16SP.name)\n"
        "            exclude_instr.append(riscv_instr_name_t.C_LWSP.name)\n"
        "            exclude_instr.append(riscv_instr_name_t.C_LDSP.name)\n",
        "            exclude_instr.append(riscv_instr_name_t.C_ADDI4SPN)\n"
        "            exclude_instr.append(riscv_instr_name_t.C_ADDI16SP)\n"
        "            exclude_instr.append(riscv_instr_name_t.C_LWSP)\n"
        "            exclude_instr.append(riscv_instr_name_t.C_LDSP)\n",
    ),
]


_PYGEN: Path | None = None
_PYGEN_LOCK = threading.Lock()


def patched_pygen() -> Path:
    """The patched copy of the generator, made once per run.

    Cached because `--all-tests` runs pyflow from several threads at a time and
    they all point PYTHONPATH at the same copy; rebuilding it under them would
    delete the tree a running generator is importing from.
    """
    global _PYGEN
    with _PYGEN_LOCK:
        if _PYGEN is None:
            _PYGEN = copy_and_patch_pygen()
    return _PYGEN


def copy_and_patch_pygen() -> Path:
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


def generate(count: int, instructions: int, seed: int, target: str,
             name: str = "gen", options: list[str] | None = None,
             gen_test: str = "riscv_instr_base_test") -> int:
    from generate import generate as pyflow_generate  # noqa: E402

    return pyflow_generate(count, instructions, seed, target, PROGRAMS,
                           interrupts=False,
                           signature_addr=SIGNATURE_ADDR,
                           debug_section=True,
                           pygen=patched_pygen(),
                           name=name, options=options, gen_test=gen_test)


# ----------------------------------------------------------------------------
# Upstream's testlist, and what pyflow can do with it
# ----------------------------------------------------------------------------

def testlist() -> list[dict]:
    """`riscv_dv_extension/testlist.yaml` as a list of entries.

    Enough YAML for this one file rather than a dependency: an entry starts at
    column 0 with `- test:`, its keys sit two columns in, and a `>` folded
    block runs until the indentation comes back. A nested mapping
    (`rtl_params`, `compare_opts`) is kept as its raw text, which is all
    anything here wants from it.
    """
    if not TESTLIST.is_file():
        raise BuildError(f"no testlist at {TESTLIST}\n"
                         f"run: python3 {ROOT / 'fetch.py'} ibex")
    entries: list[dict] = []
    entry: dict | None = None
    key, key_indent = None, 0
    for raw in TESTLIST.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        indent = len(raw) - len(raw.lstrip())
        if indent == 0 and line.startswith("- "):
            entry = {}
            entries.append(entry)
            line, indent, key = line[2:], 2, None
        elif entry is None:
            continue
        elif key is not None and indent > key_indent:
            entry[key] = f"{entry[key]} {line}".strip()
            continue
        name, separator, value = line.partition(":")
        if not separator:
            continue
        key, key_indent = name.strip(), indent
        entry[key] = value.strip().lstrip(">").strip()
    return entries


def gen_opts(entry: dict) -> list[tuple[str, str]]:
    """The `+name=value` pairs of one entry's `gen_opts`, in order."""
    return [(token[1:].split("=", 1)[0], token.split("=", 1)[1])
            for token in entry.get("gen_opts", "").split()
            if token.startswith("+") and "=" in token]


def pyflow_arguments() -> set[str]:
    """What pyflow's generator accepts on its command line.

    Read out of `riscv_instr_gen_config.py` rather than listed here, because
    the answer is that file's own argparse and nothing else. An option it does
    not name is a hard error: pyflow finishes with `parse_args`, not
    `parse_known_args`, so passing one aborts generation rather than being
    ignored.
    """
    text = (PYGEN_SRC / "riscv_instr_gen_config.py").read_text(encoding="utf-8")
    names = set(re.findall(r"add_argument\('--(\w+)'", text))
    if not names:
        raise BuildError("riscv_instr_gen_config.py: no argparse options found; "
                         "riscv-dv has changed shape")
    # The directed-instruction slots are added in a loop, so the pattern above
    # does not see them: `for i in range(self.max_directed_instr_stream_seq)`.
    names |= {f"directed_instr_{i}" for i in range(MAX_DIRECTED_STREAMS)}
    return names


def pyflow_streams() -> set[str]:
    """The directed instruction streams pyflow can build.

    `riscv_utils.factory` is a literal dict of eleven names and exits with
    `Cannot Create object of ...` for anything else, so a `+directed_instr_N`
    naming one of Ibex's own streams has to be dropped rather than passed.
    """
    text = (PYGEN_SRC / "riscv_utils.py").read_text(encoding="utf-8")
    body = text.partition("def factory(")[2].partition("}")[0]
    names = set(re.findall(r'"(\w+)":', body))
    if not names:
        raise BuildError("riscv_utils.py: no stream factory found; "
                         "riscv-dv has changed shape")
    return names


# riscv_instr_gen_config.py: self.max_directed_instr_stream_seq = 20.
MAX_DIRECTED_STREAMS = 20

# Two options pyflow accepts, reads, and then cannot randomize. Both go through
# `riscv_illegal_instr`, whose constraint set pyvsc fails to solve often enough
# that a handful of instructions is all it takes: measured here,
# `+illegal_instr_ratio=5` and `+hint_instr_ratio=5` each end in a SolveFailure
# at 2,000 instructions, and `+illegal_instr_ratio=25` fails at 400. There is
# no smaller ratio worth falling back to, so they are dropped and recorded.
# Two instruction classes pyflow does not put in a program whatever it is
# asked. Turning either off is honoured trivially; turning one on is not, so
# only the `=0` form is recorded.
#
#   no_fence      `create_instr_list` skips FENCE, FENCE_I and SFENCE_VMA
#                 before the categories are filled, so instr_category["SYNCH"]
#                 is empty and there is nothing for `+no_fence=0` to add.
#   no_csr_instr  `build_basic_instruction_list` guards the CSR instructions
#                 with `cfg.init_privileged_mode == "MACHINE_MODE"`, an enum
#                 compared against a string, which is never true. Correcting
#                 that comparison was tried: the CSR instructions then reach
#                 the solver and every program fails to generate, so the
#                 comparison is left as it is and this is recorded instead.
NEVER_GENERATED = {
    "no_fence": "pyflow generates no fence instructions in either case",
    "no_csr_instr": "pyflow generates no CSR instructions in either case",
}

SOLVER_FAILS = {
    "illegal_instr_ratio":
        "pyflow's riscv_illegal_instr randomization fails on current pyvsc",
    "hint_instr_ratio":
        "same path as illegal_instr_ratio, and the same pyvsc failure",
}

# How many seeds to try before giving up on an entry. What is left of the
# solve failures after the patches above is luck per directed stream instance,
# so a failed generation is worth retrying rather than reporting; a program that
# takes three attempts is the same program as one that takes one.
GENERATION_ATTEMPTS = 3

# Options pyflow's argparse accepts and pyflow's generator never reads. They
# are not errors and not effects either, so a test asking for one gets a
# program that quietly does not have it. `check_inert` re-derives this at build
# time and fails if any of them acquires a reader.
INERT = {
    "gen_debug_section": "pyflow's gen_debug_rom is `# TODO / pass`, and no "
                         "pyflow target sets support_debug_mode",
    "num_debug_sub_program": "there is no generated debug ROM to put them in",
    "set_dcsr_ebreak": "debug ROM only",
    "enable_debug_single_step": "debug ROM only",
    "enable_dummy_csr_write": "parsed into the config and never read",
    "enable_misaligned_instr": "parsed into the config and never read",
}


def check_inert() -> None:
    read = set()
    for path in sorted(PYGEN_SRC.rglob("*.py")):
        if path.name == "riscv_instr_gen_config.py":
            continue
        read |= set(re.findall(r"\bcfg\.(\w+)",
                               path.read_text(encoding="utf-8")))
    acquired = sorted(name for name in INERT if name in read)
    if acquired:
        raise BuildError(
            f"pyflow now reads {', '.join(acquired)}; INERT is out of date")


def reason(name: str) -> str:
    """Why a `gen_opts` option cannot be passed to pyflow."""
    if name.startswith("pmp_") or name in ("enable_write_pmp_csr", "mseccfg",
                                           "suppress_pmp_setup",
                                           "disable_pmp_exception_handler"):
        return "PMP, which no pyflow target supports (support_pmp = 0)"
    if name.startswith("enable_z"):
        return ("a bitmanip subset; pyflow has only enable_b_extension and "
                "enable_bitmanip_groups")
    if name.startswith("uvm_set_type_override"):
        return "a UVM factory override, which a Python generator has no notion of"
    if name in ("toggle_dit", "toggle_dummy_instr", "gen_all_csrs_by_default",
                "add_csr_write"):
        return "Ibex's own riscv_dv_extension, which is SystemVerilog"
    return "no pyflow equivalent"


def translate(entry: dict) -> tuple[list[str], list[str]]:
    """pyflow flags for one entry, and the options it cannot honour.

    The second half is the point. Dropping an option changes what the program
    tests, so each one comes back as a line naming the option and the reason,
    which ends up in the manifest and in `run_tests.py`'s report.
    """
    accepted, streams = pyflow_arguments(), pyflow_streams()
    options: list[str] = []
    notes: list[str] = []
    for name, value in gen_opts(entry):
        opt = f"+{name}={value}"
        if name in ("require_signature_addr", "signature_addr"):
            # Set for every program built here; see SIGNATURE_ADDR.
            continue
        if name in SOLVER_FAILS:
            notes.append(f"{opt}: {SOLVER_FAILS[name]}")
            continue
        if name in NEVER_GENERATED and value == "0":
            notes.append(f"{opt}: {NEVER_GENERATED[name]}")
            continue
        if name == "num_of_sub_program" and value != "0":
            notes.append(f"{opt}: forced to 0, pyflow's gen_callstack fails on "
                         f"current pyvsc")
            continue
        if name.startswith("directed_instr_"):
            stream = value.split(",")[0]
            if stream not in streams:
                notes.append(f"{opt}: {stream} is not in pyflow's stream factory")
                continue
        if name not in accepted:
            notes.append(f"{opt}: {reason(name)}")
            continue
        if name in INERT:
            notes.append(f"{opt}: accepted by pyflow and never read -- "
                         f"{INERT[name]}")
            continue
        if name == "enable_bitmanip_groups":
            # nargs='*', and pyflow spells the groups in upper case.
            options.append(f"--{name}")
            options += [group.strip().upper() for group in value.split(",")]
            continue
        options += [f"--{name}", value]
    return options, notes


def march_for(entry: dict, default: str) -> str:
    """The `-march` to link with, from the entry's `gcc_opts` if it has one."""
    found = re.search(r"-march=(rv32\w+)", entry.get("gcc_opts", ""))
    if not found:
        return default
    # gcc 15 wants zicsr and zifencei named; riscv-dv's generated code uses
    # both whatever the entry asked for.
    return f"{found.group(1)}_zicsr_zifencei"


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


def build_test(entry: dict, args) -> dict:
    """Generate and link the program one testlist entry asks for."""
    name = entry["test"]
    gen_test = entry.get("gen_test", "")
    options, notes = translate(entry)
    record = {"test": name,
              "rtl_test": entry.get("rtl_test", ""),
              "gen_test": gen_test,
              "gen_opts": entry.get("gen_opts", "").split(),
              "sim_opts": entry.get("sim_opts", "").split(),
              "timeout_s": entry.get("timeout_s", ""),
              "march": march_for(entry, args.march),
              "unsupported": notes}
    if not gen_test:
        # riscv_csr_test is generated by riscv-dv's gen_csr_test.py from the
        # target's CSR description, not by the instruction generator at all.
        record["error"] = "entry has no gen_test; not a generator program"
        return record
    if not (patched_pygen() / f"pygen_src/test/{gen_test}.py").is_file():
        # pyflow ships riscv_instr_base_test and riscv_rand_instr_test and no
        # other generator test. Falling back to the base one keeps the entry's
        # gen_opts, and loses whatever its own test class does on top.
        notes.append(f"gen_test: {gen_test} has no pyflow implementation; "
                     f"generated with riscv_instr_base_test")
        gen_test = "riscv_instr_base_test"
        record["gen_test"] = gen_test
    elif gen_test == "riscv_rand_instr_test":
        # pyflow's copy of that test has four of the seven directed streams
        # the SystemVerilog one mixes in commented out: riscv_loop_instr,
        # riscv_hazard_instr_stream, riscv_multi_page_load_store_instr_stream
        # and riscv_mem_region_stress_test. Three of the four have no pyflow
        # implementation at all.
        notes.append("gen_test: riscv_rand_instr_test mixes three of "
                     "upstream's seven directed streams; pyflow comments out "
                     "the other four")

    if not args.skip_generate:
        for attempt in range(args.attempts):
            seed = args.seed + attempt
            if generate(1, args.instructions, seed, args.target, name=name,
                        options=options, gen_test=gen_test) == 0:
                record["seed"] = seed
                break
        else:
            record["error"] = f"generation failed on {args.attempts} seed(s)"
            return record

    sources = sorted(PROGRAMS.glob(f"{name}_*.S"))
    if not sources:
        record["error"] = f"no {name}_*.S in {PROGRAMS}"
        return record
    try:
        record.update(build_one(sources[0], record["march"]))
    except (BuildError, subprocess.CalledProcessError) as error:
        record["error"] = str(error).splitlines()[0]
    return record


def write_manifest(march: str, programs: list[dict] | None = None,
                   tests: list[dict] | None = None) -> None:
    """Update build/manifest.json, keeping what this run did not rebuild.

    A run naming one test should not throw away the other fifty-six, so the
    file is merged rather than rewritten.
    """
    data = {"signature_addr": SIGNATURE_ADDR, "march": march,
            "programs": [], "tests": []}
    if MANIFEST.is_file():
        data.update(json.loads(MANIFEST.read_text(encoding="utf-8")))
    data["march"] = march
    if programs is not None:
        data["programs"] = programs
    if tests:
        kept = [t for t in data.get("tests", [])
                if t["test"] not in {new["test"] for new in tests}]
        data["tests"] = sorted(kept + tests, key=lambda t: t["test"])
    MANIFEST.write_text(json.dumps(data, indent=2), encoding="utf-8")
    print(f"wrote {MANIFEST.relative_to(HERE)}")


def build_tests(args, wanted: list[str]) -> int:
    entries = [e for e in testlist() if e.get("test")]
    known = {e["test"] for e in entries}
    missing = [name for name in wanted if name not in known]
    if missing:
        print(f"build_programs: not in the testlist: {', '.join(missing)}",
              file=sys.stderr)
        return 1
    if wanted:
        entries = [e for e in entries if e["test"] in wanted]

    check_inert()
    patched_pygen()
    # One pyflow process per entry, several at a time: a 10,000-instruction
    # program takes about eighty seconds and the whole testlist is an hour
    # serially. Half the cores, because each entry is a separate Python
    # interpreter with the constraint solver loaded. Their logging interleaves;
    # the per-entry lines below are the summary to read.
    jobs = max(1, min(args.jobs, (os.cpu_count() or 4) // 2))
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        records = list(pool.map(lambda e: build_test(e, args), entries))

    records.sort(key=lambda record: record["test"])
    write_manifest(args.march, tests=records)
    failed = 0
    for record in records:
        if "error" in record:
            failed += 1
            print(f"  FAILED {record['test']:<42} {record['error']}",
                  file=sys.stderr)
            continue
        notes = record["unsupported"]
        detail = f"{record['bytes']:>8} bytes"
        if notes:
            detail += f", {len(notes)} gen_opt(s) not honoured"
        print(f"  {record['test']:<42} {detail}")
    for record in records:
        for note in record["unsupported"]:
            print(f"    {record['test']}: {note}")
    print(f"\nbuilt {len(records) - failed} of {len(records)} test program(s)")
    return 1 if failed else 0


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
    parser.add_argument("--test", action="append", default=[],
                        help="build the program a testlist entry asks for; "
                             "repeatable")
    parser.add_argument("--all-tests", action="store_true",
                        help="build a program for every testlist entry")
    parser.add_argument("--attempts", type=int, default=GENERATION_ATTEMPTS,
                        help="seeds to try per entry before giving up")
    parser.add_argument("--list-tests", action="store_true",
                        help="print the testlist entries and stop")
    args = parser.parse_args(argv)

    if args.list_tests:
        for entry in testlist():
            options, notes = translate(entry)
            print(f"  {entry['test']:<42} {entry.get('rtl_test', '')}"
                  f"{'  (' + str(len(notes)) + ' gen_opts dropped)' if notes else ''}")
        return 0
    if args.test or args.all_tests:
        return build_tests(args, [] if args.all_tests else args.test)

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
        write_manifest(args.march, programs=built)
        print(f"built {len(built)} program(s)")
    for name, error in failures:
        print(f"  FAILED {name}: {error.splitlines()[0]}", file=sys.stderr)
        if len(failures) == 1:
            print("\n".join(error.splitlines()[1:6]), file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
