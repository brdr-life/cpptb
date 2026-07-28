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
    # `riscv_debug_rom_gen` emits. pyflow cannot: `gen_debug_rom` is
    # `# TODO / pass`, and the rv32imc target sets support_debug_mode = 0.
    #
    # What goes at those two labels is not a guess. `riscv_debug_rom_gen`
    # generates `dret` and nothing else at `debug_rom` when
    # `cfg.gen_debug_section` is 0 --
    #
    #     if (!cfg.gen_debug_section) begin
    #       // If the debug section should not be generated, we just populate it
    #       // with a dret instruction.
    #       debug_main = {dret};
    #
    # -- and `gen_debug_exception_handler` is `str = {"dret"}` in every case,
    # with its own TODO saying so. So `debug_exception` is exactly upstream's
    # here, and `debug_rom` is exactly upstream's for the entries that do not
    # ask for a debug section.
    #
    # For the entries that do ask, this is the floor rather than the article:
    # they get debug entry and an immediate return where upstream would run a
    # generated ROM. That is recorded against those entries. It matters that it
    # is a `dret` and not the self-loop this file used to emit: fourteen entries
    # send the core into the debug ROM, and a self-loop turns every one of them
    # into a cycle timeout caused by this port rather than by anything upstream.
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
        '                                  "debug_rom: dret",\n'
        '                                  ".align 3",\n'
        '                                  "debug_exception: dret",\n'
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
    # three directed instruction streams. It hardcodes two settings over the top
    # of the command line, exactly as the SystemVerilog it reimplements does:
    #
    #     virtual function void randomize_cfg();
    #       cfg.instr_cnt = 10000;
    #       cfg.num_of_sub_program = 5;
    #
    # `cfg.instr_cnt` is left alone here, because that assignment happens after
    # the config has read `+instr_cnt`, so upstream generates 10,000
    # instructions for every one of those entries whatever their `gen_opts` say.
    # Overriding it would make this port generate something upstream does not.
    # `num_of_sub_program` has to go to 0: see SUB_PROGRAM_REASON.
    (
        "test/riscv_rand_instr_test.py",
        "        cfg.instr_cnt = 10000\n"
        "        cfg.num_of_sub_program = 5\n",
        "        cfg.instr_cnt = 10000\n"
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
    # `create_instr_list` drops FENCE, FENCE_I and SFENCE_VMA from the pool
    # unconditionally. The SystemVerilog it reimplements guards the same line
    # with the option that exists to control it:
    #
    #     if (cfg.no_fence && (instr_name inside {FENCE, FENCE_I, SFENCE_VMA}))
    #       continue;
    #
    # so `+no_fence=0` adds nothing to `instr_category["SYNCH"]` and the three
    # entries that ask for fences get a program with none. The line above it is
    # the same mistake with the polarity the other way round: the SystemVerilog
    # reads `!cfg.enable_sfence && instr_name == SFENCE_VMA`, pyflow reads
    # `cfg.enable_sfence and ...`, so restoring the fence guard on its own would
    # start emitting sfence.vma. pyflow also has none of the SystemVerilog's
    # `sfence_c`, which forces `enable_sfence == 0` when the target does not set
    # support_sfence, so that condition is folded in here -- rv32imc has
    # support_sfence = 0 and Ibex has no S mode.
    (
        "isa/riscv_instr.py",
        "            if (cfg.enable_sfence and instr_name == riscv_instr_name_t.SFENCE_VMA):\n"
        "                continue\n"
        "            if instr_name in [riscv_instr_name_t.FENCE, riscv_instr_name_t.FENCE_I,\n"
        "                              riscv_instr_name_t.SFENCE_VMA]:\n"
        "                continue\n",
        "            if (not (cfg.enable_sfence and rcs.support_sfence) and\n"
        "                    instr_name == riscv_instr_name_t.SFENCE_VMA):\n"
        "                continue\n"
        "            if cfg.no_fence and instr_name in [riscv_instr_name_t.FENCE,\n"
        "                                               riscv_instr_name_t.FENCE_I,\n"
        "                                               riscv_instr_name_t.SFENCE_VMA]:\n"
        "                continue\n",
    ),
    # ------------------------------------------------------------------------
    # riscv_illegal_instr: Python's `and`, `or` and `not` are not pyvsc
    # operators
    # ------------------------------------------------------------------------
    #
    # This is one bug with five sites, and it is why `+illegal_instr_ratio` and
    # `+hint_instr_ratio` were recorded as unsupported: every randomization of
    # `riscv_illegal_instr` failed, in every configuration, from the first one.
    #
    # pyvsc builds a constraint by side effect: each expression created inside a
    # `@vsc.constraint` body is appended to the enclosing scope, and an operator
    # such as `&` or `|` consumes its operands and pushes the combination.
    # Python's `and` and `or` are not operators -- they test the left operand
    # for truthiness, which an expression object always has, and return one of
    # the two. So `(a == 1) and (b == 2)` leaves *both* comparisons in the
    # scope as separate conjuncts, `(a == 1) or (b == 2)` does the same where a
    # disjunction was meant, and `not (a == 1)` produces a Python `False` while
    # leaving `a == 1` behind as a constraint of its own.
    #
    # A minimal unsatisfiable core over the sixteen constraint blocks is ten of
    # them, which is what a conjunction of mutually exclusive alternatives looks
    # like from the solver's side. Two examples of what the file means to say
    # and what pyvsc is given:
    #
    #   has_func7_c   means  (opcode==19 && func3 inside {1,5}) || opcode inside {51,59}
    #                 gets   opcode==19 && func3==1 && func3==5 && opcode==51 && opcode==59
    #
    #   legal_rv32_c_slli  means  if (c_msb==0 && c_op==2 && XLEN==32)
    #                      gets   c_msb==0; c_op==2; if (XLEN==32)
    #
    # The second one alone forces every compressed encoding to C.SLLI, so no
    # compressed illegal instruction can be generated even when the solve
    # succeeds. With all five fixed, all seven `illegal_instr_type_e` values
    # appear in a 6,000-instruction program and 60 of 60 randomizations succeed.
    (
        "riscv_illegal_instr.py",
        "        with vsc.if_then((self.opcode == 19) and (self.func3 == 1 or self.func3 == 5) or\n"
        "                         (self.opcode == 51 or self.opcode == 59)):\n",
        "        with vsc.if_then(((self.opcode == 19) &\n"
        "                          ((self.func3 == 1) | (self.func3 == 5))) |\n"
        "                         (self.opcode == 51) | (self.opcode == 59)):\n",
    ),
    (
        "riscv_illegal_instr.py",
        "        with vsc.if_then(self.opcode == 55 or self.opcode == 111 or self.opcode == 23):\n",
        "        with vsc.if_then((self.opcode == 55) | (self.opcode == 111) |\n"
        "                         (self.opcode == 23)):\n",
    ),
    (
        "riscv_illegal_instr.py",
        "        with vsc.if_then((self.c_msb == 0) and (self.c_op == 2) and (self.xlen == 32)):\n",
        "        with vsc.if_then((self.c_msb == 0) & (self.c_op == 2) & (self.xlen == 32)):\n",
    ),
    # hint_instr_c is eight alternatives joined with `or`, so seven of them were
    # being asserted alongside the first rather than instead of it.
    (
        "riscv_illegal_instr.py",
        "            ((self.c_msb == 0) and (self.c_op == 1) and (self.instr_bin[12] +\n"
        "                                                         self.instr_bin[6:2] == 0)) or \\\n"
        "                ((self.c_msb == 2) and (self.c_op == 1) and "
        "(self.instr_bin[11:7] == 0)) or \\\n"
        "                ((self.c_msb == 4) and (self.c_op == 1) and "
        "(self.instr_bin[12:11] == 0) and\n"
        "                 (self.instr_bin[6:2] == 0)) or \\\n"
        "                ((self.c_msb == 4) and (self.c_op == 2) and "
        "(self.instr_bin[11:7] == 0) and\n"
        "                 (self.instr_bin[6:2] != 0)) or \\\n"
        "                ((self.c_msb == 3) and (self.c_op == 1) and "
        "(self.instr_bin[11:7] == 0) and\n"
        "                 (self.instr_bin[12] + self.instr_bin[6:2]) != 0) or \\\n"
        "                ((self.c_msb == 0) and (self.c_op == 2) and "
        "(self.instr_bin[11:7] == 0)) or \\\n"
        "                ((self.c_msb == 0) and (self.c_op == 2) and "
        "(self.instr_bin[11:7] != 0) and\n"
        "                 not(self.instr_bin[12]) and (self.instr_bin[6:2] == 0)) or \\\n"
        "                ((self.c_msb == 4) and (self.c_op == 2) and "
        "(self.instr_bin[11:7] == 0) and\n"
        "                 self.instr_bin[12] and (self.instr_bin[6:2] != 0))\n",
        "            (((self.c_msb == 0) & (self.c_op == 1) &\n"
        "              (self.instr_bin[12] + self.instr_bin[6:2] == 0)) |\n"
        "             ((self.c_msb == 2) & (self.c_op == 1) & (self.instr_bin[11:7] == 0)) |\n"
        "             ((self.c_msb == 4) & (self.c_op == 1) & (self.instr_bin[12:11] == 0) &\n"
        "              (self.instr_bin[6:2] == 0)) |\n"
        "             ((self.c_msb == 4) & (self.c_op == 2) & (self.instr_bin[11:7] == 0) &\n"
        "              (self.instr_bin[6:2] != 0)) |\n"
        "             ((self.c_msb == 3) & (self.c_op == 1) & (self.instr_bin[11:7] == 0) &\n"
        "              (self.instr_bin[12] + self.instr_bin[6:2] != 0)) |\n"
        "             ((self.c_msb == 0) & (self.c_op == 2) & (self.instr_bin[11:7] == 0)) |\n"
        "             ((self.c_msb == 0) & (self.c_op == 2) & (self.instr_bin[11:7] != 0) &\n"
        "              (self.instr_bin[12] == 0) & (self.instr_bin[6:2] == 0)) |\n"
        "             ((self.c_msb == 4) & (self.c_op == 2) & (self.instr_bin[11:7] == 0) &\n"
        "              (self.instr_bin[12] == 1) & (self.instr_bin[6:2] != 0)))\n",
    ),
    # The two `not` sites. `not self.instr_bin[12]` is a Python bool that goes
    # nowhere, and leaves `instr_bin[12]` in the scope as a bare expression --
    # which pyvsc reads as "non-zero", the opposite of what the SystemVerilog
    # `!instr_bin[12]` asks for. Both are inside `reserved_compressed_instr_c`,
    # whose `and` chains are conjunctions in the enclosing `if_then` and so mean
    # what they say by accident.
    (
        "riscv_illegal_instr.py",
        "                 (not self.instr_bin[12]) and (self.instr_bin[6:2] == 0))\n"
        "            with vsc.if_then(self.reserved_c == reserved_c_instr_e.kReservedLui):\n"
        "                ((self.c_msb == 3) and (self.c_op == 1) and\n"
        "                 (not self.instr_bin[12]) and (self.instr_bin[6:2] == 0))\n",
        "                 (self.instr_bin[12] == 0) and (self.instr_bin[6:2] == 0))\n"
        "            with vsc.if_then(self.reserved_c == reserved_c_instr_e.kReservedLui):\n"
        "                ((self.c_msb == 3) and (self.c_op == 1) and\n"
        "                 (self.instr_bin[12] == 0) and (self.instr_bin[6:2] == 0))\n",
    ),
    # And the encoding those randomizations produce. The SystemVerilog formats
    # the bits as hex digits -- `%8h` and `%4h` -- and the caller writes
    # `.4byte 0x%s`. pyflow's `get_bin_str` returns `hex(instr_bin)` for the
    # 32-bit case, which happens to assemble, and the bare integer for the
    # 16-bit case, which assembles as a decimal number: `.2byte 24705` where
    # `.2byte 0x6081` was meant. Every HINT and every compressed illegal
    # instruction would be the wrong encoding.
    (
        "riscv_illegal_instr.py",
        "        if self.compressed == 1:\n"
        "            local_instr_bin = self.instr_bin & 0xffff\n"
        "        else:\n"
        "            local_instr_bin = hex(self.instr_bin)\n",
        "        if self.compressed == 1:\n"
        '            local_instr_bin = "{:04x}".format(int(self.instr_bin) & 0xffff)\n'
        "        else:\n"
        '            local_instr_bin = "{:08x}".format(int(self.instr_bin))\n',
    ),
    (
        "riscv_instr_sequence.py",
        '                insert_str = "{}.4byte {} # {}".format(pkg_ins.indent,\n',
        '                insert_str = "{}.4byte 0x{} # {}".format(pkg_ins.indent,\n',
    ),
    (
        "riscv_instr_sequence.py",
        '                insert_str = "{}.2byte {} # {}".format(pkg_ins.indent,\n',
        '                insert_str = "{}.2byte 0x{} # {}".format(pkg_ins.indent,\n',
    ),
    # The third piece of the signature handshake, and the one that decides
    # whether any of the twenty directed test classes can start at all.
    #
    # `core_ibex_directed_test::send_stimulus` calls `wait_for_core_setup()`,
    # which is
    #
    #     wait_for_csr_write(CSR_MSTATUS, 10000);
    #     wait_for_csr_write(CSR_MIE, 5000);
    #     check_next_core_status(INITIALIZED, ..., 5000);
    #
    # so the program has to write MSTATUS and MIE to the signature address
    # before it says it is initialized. riscv-dv's
    # `gen_privileged_mode_switch_routine` does that, between entering the
    # target privileged mode and the `mret` that gets there. pyflow's copy is
    #
    #     if cfg.require_signature_addr:
    #         # TODO
    #         pass
    #
    # and the consequence is not subtle: every debug, interrupt, memory-error,
    # dret, umode and invalid-CSR entry -- twenty of the fifty-seven -- dies on
    #
    #     UVM_FATAL Did not receive write to csr 0x300 within 10000 cycle
    #     timeout period
    #
    # ten thousand cycles into the run, having executed nothing of its program.
    # This transcribes the SystemVerilog, including the pop of the `mret` that
    # `enter_privileged_mode` has already appended and re-indenting to match
    # what that function did to the rest of the block.
    (
        "riscv_asm_program_gen.py",
        "            privil_seq.enter_privileged_mode(privil_mode, instr)\n"
        "            if cfg.require_signature_addr:\n"
        "                # TODO\n"
        "                pass\n",
        "            privil_seq.enter_privileged_mode(privil_mode, instr)\n"
        "            if cfg.require_signature_addr:\n"
        "                ret_instr = instr.pop()\n"
        "                csr_handshake = []\n"
        "                if privil_mode == privileged_mode_t.SUPERVISOR_MODE:\n"
        "                    self.gen_signature_handshake(\n"
        "                        csr_handshake, signature_type_t.WRITE_CSR,\n"
        "                        csr=privileged_reg_t.SSTATUS)\n"
        "                    self.gen_signature_handshake(\n"
        "                        csr_handshake, signature_type_t.WRITE_CSR,\n"
        "                        csr=privileged_reg_t.SIE)\n"
        "                elif privil_mode == privileged_mode_t.USER_MODE:\n"
        "                    self.gen_signature_handshake(\n"
        "                        csr_handshake, signature_type_t.WRITE_CSR,\n"
        "                        csr=privileged_reg_t.USTATUS)\n"
        "                    self.gen_signature_handshake(\n"
        "                        csr_handshake, signature_type_t.WRITE_CSR,\n"
        "                        csr=privileged_reg_t.UIE)\n"
        "                self.gen_signature_handshake(\n"
        "                    csr_handshake, signature_type_t.WRITE_CSR,\n"
        "                    csr=privileged_reg_t.MSTATUS)\n"
        "                self.gen_signature_handshake(\n"
        "                    csr_handshake, signature_type_t.WRITE_CSR,\n"
        "                    csr=privileged_reg_t.MIE)\n"
        "                self.format_section(csr_handshake)\n"
        "                instr.extend([pkg_ins.indent + line\n"
        "                              for line in csr_handshake])\n"
        "                instr.append(ret_instr)\n",
    ),
    # A generation error hangs pyflow instead of reporting it. `run` uses
    # `multiprocessing.Pool.map`, and `run_phase` catches `Exception` -- but
    # pyflow's own error path is `logging.critical(...); sys.exit(1)`, in
    # `get_rand_instr`, `get_instr`, `create_instr` and half a dozen other
    # places. `SystemExit` derives from BaseException, not Exception, so it goes
    # uncaught, the pool worker exits, and `Pool.map` waits for a result that
    # will never arrive. The parent sits in a futex and the worker in a pipe
    # read, neither using any CPU, forever.
    #
    # `riscv_loop_test` is one that does this: 37 minutes and one second of CPU
    # before it was killed. With BaseException caught, the same run prints the
    # traceback pyflow meant to print and `run` raises "Test-generation jobs
    # failed" in a few seconds.
    #
    # `build_programs.py` also puts a wall-clock bound on each attempt, because
    # this is the kind of bug that comes back in a different place.
    (
        "test/riscv_instr_base_test.py",
        "        try:\n"
        "            self._run_phase(num)\n"
        "            return 0\n"
        "        except Exception as e:\n"
        "            traceback.print_exc()\n"
        "            return 1\n",
        "        try:\n"
        "            self._run_phase(num)\n"
        "            return 0\n"
        "        except BaseException as e:\n"
        "            traceback.print_exc()\n"
        "            return 1\n",
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
             gen_test: str = "riscv_instr_base_test",
             timeout: int | None = None) -> int:
    from generate import generate as pyflow_generate  # noqa: E402

    return pyflow_generate(count, instructions, seed, target, PROGRAMS,
                           interrupts=False,
                           signature_addr=SIGNATURE_ADDR,
                           debug_section=True,
                           pygen=patched_pygen(),
                           name=name, options=options, gen_test=gen_test,
                           timeout=timeout)


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


# Streams pyflow's factory will build and pyflow cannot generate. Being in the
# factory is not the same as being implemented.
#
#   riscv_loop_instr  `post_randomize` is three-quarters commented out. The
#     SystemVerilog draws each of the three key instructions with
#     `get_rand_instr(.include_instr({ADDI}))` and then constrains it; pyflow
#     draws an unrestricted random instruction -- "Removed include_instr ADDI
#     for now to avoid unrecognized colon" -- so `rd == loop_cnt_reg; rs1 ==
#     ZERO; imm == loop_init_val` is asked of whatever came out, and fails.
#     That failure is `sys.exit(1)` inside a pool worker, which is the hang
#     described in PYGEN_PATCHES. Worse, the constraints on
#     `loop_update_instr` are commented out entirely ("Commenting for now due
#     to key error"), so the loop counter is never updated and any loop pyflow
#     did manage to emit would not terminate. pyflow's own
#     `riscv_rand_instr_test` has this stream commented out of its directed
#     list, which is the same conclusion reached from the other side.
BROKEN_STREAMS = {
    "riscv_loop_instr":
        "in pyflow's factory but not implemented: the loop-counter update "
        "constraints are commented out, so a generated loop never terminates, "
        "and the loop-init randomize fails because the instruction it "
        "constrains is drawn at random instead of being an ADDI",
}


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

# Instruction classes pyflow does not put in a program whatever it is asked.
# Turning one off is honoured trivially; turning one on is not, so only the
# `=0` form is recorded.
#
#   no_csr_instr  Three things stand in the way, not one.
#                 `build_basic_instruction_list` guards the CSR category with
#                 `cfg.init_privileged_mode == "MACHINE_MODE"`, an enum
#                 compared against a string, which is never true. Behind that,
#                 `riscv_instr.csr_c` -- the constraint that keeps the CSR
#                 address inside the implemented set -- is `# TODO / pass`, and
#                 pyflow has no `riscv_csr_instr` class at all, so the address
#                 would be an unconstrained 12-bit value; `create_csr_filter`
#                 fills `include_reg` and `exclude_reg` with strings that
#                 nothing reads. And `convert2asm` formats the result as
#                 `0x{}` around the *decimal* value, so `csrrw x1, 0x800, x2`
#                 would name CSR 0x800 where 0x320 was drawn. Honouring
#                 `+no_csr_instr=0` means porting riscv_csr_instr.sv, not
#                 correcting the comparison.
#   no_ecall      pyflow has no `--no_ecall` and never puts ECALL in the pool.
#                 It also generates riscv-dv's stock ecall handler, which jumps
#                 to `write_tohost` and spins, rather than Ibex's override.
NEVER_GENERATED = {
    "no_csr_instr": "pyflow generates no CSR instructions in either case; "
                    "riscv_instr.csr_c is `# TODO / pass` and there is no "
                    "riscv_csr_instr class to constrain the address",
    "no_ecall": "pyflow has no --no_ecall and never puts ECALL in the pool",
}

# Options that ask for the *absence* of something pyflow never generates. They
# cannot be passed, and dropping them costs nothing: the program pyflow
# produces already has the property the option asks for.
#
# `+suppress_pmp_setup=1` asks riscv-dv to emit a PMP configuration that allows
# everything instead of the randomized one, and `+disable_pmp_exception_handler=1`
# asks Ibex's extension not to install its PMP exception handler. pyflow emits
# neither section, so both requests are already met -- a program with no PMP
# entries configured is unrestricted in M mode, which is where these two entries
# run.
VACUOUS = {
    "suppress_pmp_setup":
        "pyflow emits no PMP setup at all, which is what this asks for",
    "disable_pmp_exception_handler":
        "pyflow emits no PMP exception handler, which is what this asks for",
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

# Options pyflow does read, on a code path it never takes. `check_unreachable`
# checks that the path is still dead rather than trusting this list.
#
#   enable_ebreak_in_debug_rom  read in `riscv_instr_stream.randomize_instr`
#                               under `if is_in_debug`, which only the debug
#                               ROM sets, and `gen_debug_rom` is a stub.
#   enable_access_invalid_csr_level  read in `riscv_instr.create_csr_filter`,
#                               which fills `include_reg` -- a list nothing
#                               reads -- and CSR instructions are not generated
#                               at all. See NEVER_GENERATED["no_csr_instr"].
UNREACHABLE = {
    "enable_ebreak_in_debug_rom":
        "read only inside the debug ROM, which pyflow's gen_debug_rom does not "
        "generate",
    "enable_access_invalid_csr_level":
        "read only into riscv_instr.include_reg, which nothing reads, and "
        "pyflow generates no CSR instructions",
}

# What has to still be true for UNREACHABLE to hold: the debug ROM generator is
# still a stub, and `include_reg` is still write-only.
UNREACHABLE_EVIDENCE = [
    ("riscv_asm_program_gen.py",
     "    def gen_debug_rom(self, hart):\n        # TODO\n        pass\n",
     "gen_debug_rom is no longer a stub"),
]


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


def check_unreachable() -> None:
    for name, text, complaint in UNREACHABLE_EVIDENCE:
        if text not in (PYGEN_SRC / name).read_text(encoding="utf-8"):
            raise BuildError(f"{name}: {complaint}; UNREACHABLE is out of date")
    # `include_reg` and `exclude_reg` are assigned in create_csr_filter and read
    # nowhere. If that changes, enable_access_invalid_csr_level has an effect
    # again.
    readers = [path.name for path in sorted(PYGEN_SRC.rglob("*.py"))
               if path.name != "riscv_instr.py"
               and "include_reg" in path.read_text(encoding="utf-8")]
    if readers:
        raise BuildError(f"{', '.join(readers)} now reads include_reg; "
                         f"UNREACHABLE is out of date")


def reason(name: str) -> str:
    """Why a `gen_opts` option cannot be passed to pyflow."""
    if name.startswith("pmp_") or name in ("enable_write_pmp_csr", "mseccfg"):
        return ("PMP: no pyflow target sets support_pmp, riscv_pmp_cfg has no "
                "Python implementation, and setup_pmp and gen_pmp_csr_write "
                "are stubs")
    if name.startswith("enable_z"):
        # `enable_bitmanip_groups` is honoured -- riscv_b_instr.is_supported
        # reads it -- but the per-subset flags have no pyflow argument, and it
        # makes no difference: pyflow's only B module is isa/rv32b_instr.py,
        # which is the draft v0.93 encoding. `bfp`, `grevi`, `cmix`, `crc32.h`
        # and `sbclr` are what comes out, and binutils 15.2 assembles none of
        # them, so a bitmanip program does not link at all.
        return ("a bitmanip subset flag pyflow does not have; and pyflow's only "
                "B module is the draft v0.93 encoding, which this binutils "
                "does not assemble")
    if name.startswith("uvm_set_type_override"):
        return "a UVM factory override, which a Python generator has no notion of"
    if name in ("toggle_dit", "toggle_dummy_instr", "gen_all_csrs_by_default",
                "add_csr_write"):
        return "Ibex's own riscv_dv_extension, which is SystemVerilog"
    return "no pyflow equivalent"


# Why `+num_of_sub_program=N` is forced to 0. Two independent reasons, and the
# second is the one that matters: even with the first fixed the sub-programs
# would never be called.
#
#   gen_callstack calls `self.callstack_gen.init(...)` on a local it has just
#   named `callstack_gen`, so it dies with `'riscv_asm_program_gen' object has
#   no attribute 'callstack_gen'`, and reads `callstack_gen.program_h` and
#   `.program_id[i]` where the class has `program_h[i]`.
#
#   riscv_instr_sequence.insert_jump_instr is `pass`, with the whole body
#   commented out above a `# TODO riscv_jump_instr class implementation`. That
#   is the function that puts the jump into the caller. `insert_sub_program`
#   appends the sub-program bodies after the jump to `test_done`, so what a
#   fixed gen_callstack would produce is unreachable code, not a call stack.
SUB_PROGRAM_REASON = (
    "forced to 0: riscv_instr_sequence.insert_jump_instr is `pass  # TODO`, so "
    "a generated sub-program is never called, and gen_callstack fails before "
    "that on `self.callstack_gen`")


# The UVM test library, read to find out which classes assert `debug_req_i`.
TEST_LIB = (ROOT / "deps/ibex/dv/uvm/core_ibex/tests/core_ibex_test_lib.sv")

# What a test class does in its own `send_stimulus` when it drives debug. The
# first three are the helpers `core_ibex_directed_test` provides; the fourth is
# the newer sequence object two classes start directly.
DEBUG_STIMULUS = ("start_debug_single_seq(", "send_debug_stimulus(",
                  "start_debug_stress_seq(", "debug_new_seq_h.start(")

_DEBUG_CLASSES: set[str] | None = None


def debug_driving_classes() -> set[str]:
    """UVM test classes that assert `debug_req_i` without being asked to.

    Derived from `core_ibex_test_lib.sv` rather than listed here, because the
    list would be a guess and this is a fact about that file. A class counts if
    its own body calls one of DEBUG_STIMULUS; `core_ibex_debug_intr_basic_test`
    and `core_ibex_directed_test` only *define* those helpers, and run them when
    the entry passes `+enable_debug_seq=1`, which is handled separately.
    """
    global _DEBUG_CLASSES
    if _DEBUG_CLASSES is None:
        if not TEST_LIB.is_file():
            raise BuildError(f"no UVM test library at {TEST_LIB}")
        text = TEST_LIB.read_text(encoding="utf-8")
        starts = list(re.finditer(r"^class (\w+) extends (\w+);", text, re.M))
        if not starts:
            raise BuildError(f"{TEST_LIB.name}: no test classes found; the "
                             f"library has changed shape")
        found = set()
        for index, match in enumerate(starts):
            end = (starts[index + 1].start() if index + 1 < len(starts)
                   else len(text))
            body = text[match.start():end]
            # The two base classes define the helpers; a subclass calling one
            # is what drives the stimulus.
            if match.group(1) in ("core_ibex_debug_intr_basic_test",
                                  "core_ibex_directed_test"):
                continue
            if any(call in body for call in DEBUG_STIMULUS):
                found.add(match.group(1))
        _DEBUG_CLASSES = found
    return _DEBUG_CLASSES


# What `riscv_rand_instr_test.randomize_cfg` assigns to `cfg.instr_cnt`, in
# pyflow and in the SystemVerilog alike, and why this has to reach the command
# line rather than being left to that assignment.
#
# `cfg.instr_cnt` is a plain Python int, and the constraint that reads it --
#
#     self.main_program_instr_cnt in vsc.rangelist(vsc.rng(10, self.instr_cnt))
#
# -- is elaborated into the pyvsc model when the config object is built, which
# happens at import, before any test's `randomize_cfg` runs. So assigning
# `cfg.instr_cnt = 10000` there is too late: the range the solver draws from is
# still `[10, argv.instr_cnt]`. In SystemVerilog the constraint is evaluated at
# `randomize()`, so the same two lines mean different things.
#
# The effect is not subtle. With `--instr_cnt 400` on the command line and
# `cfg.instr_cnt = 10000` in randomize_cfg, `riscv_ebreak_test` came out with
# `test_done:` at line 354; with `--instr_cnt 10000` it is at 5,906. pyflow's
# own riscv_rand_instr_test, run as shipped, generates a main program of between
# 10 and 200 instructions -- the argparse default -- and reports 10,000 in its
# config dump.
RAND_INSTR_CNT = 10000


def needs_debug_rom(entry: dict) -> bool:
    """Whether this entry's run will send the core into the debug ROM.

    Two ways it can happen, and the testlist only states one of them.
    `+enable_debug_seq=1` in `sim_opts` makes `core_ibex_debug_intr_basic_test`
    run its stress sequence; the rest of the debug classes start a debug
    sequence from their own `send_stimulus` with nothing in the testlist to say
    so. A core that takes a debug request jumps to `DEBUG_ROM_ENTRY`, and
    pyflow's `gen_debug_rom` is a stub, so what is at that address is the
    self-loop this build's own program header puts there: the core enters debug
    mode and never leaves it.
    """
    if "+enable_debug_seq=1" in entry.get("sim_opts", "").split():
        return True
    return entry.get("rtl_test", "") in debug_driving_classes()


# A dropped option that removes the thing its entry exists to exercise. The
# result of such a run is not evidence about the entry's name, whatever the
# testbench reports, so `run_tests.py` reports these separately from the tests
# whose program merely differs in detail.
#
# The debug options are deliberately not here. `+gen_debug_section=1` costs an
# entry nothing if nothing sends the core into debug mode -- `riscv_umode_tw_test`
# asks for it and has no debug stimulus at all -- so what carries the verdict is
# the synthesized `debug_rom` note, which is raised only when the run will
# actually enter the ROM.
DEFINING = frozenset({
    # PMP, ePMP and the mseccfg entries: with these gone the program is a plain
    # random program under a PMP-shaped name.
    "mseccfg", "enable_write_pmp_csr", "debug_rom",
    # "Inject debug/interrupt stimulus during dummy writes to xSTATUS and xIE".
    "enable_dummy_csr_write",
    # "generate csr accesses to invalid CSRs (at a higher priv mode)".
    "enable_access_invalid_csr_level",
})

# The same, where it depends on the entry rather than the option. Checked
# against the entry's own gen_opts at build time so it cannot go stale quietly.
DEFINING_PER_TEST = {
    # "Jump among large number of sub-programs": with num_of_sub_program forced
    # to 0 there is nothing to jump among.
    "riscv_rand_jump_test": ("num_of_sub_program",),
    # "Loop test": its one directed stream is the test. See BROKEN_STREAMS.
    "riscv_loop_test": ("directed_instr_1",),
}


def is_defining(test: str, name: str) -> bool:
    if name.startswith("pmp_") or name.startswith("enable_z"):
        return True
    if name in DEFINING_PER_TEST.get(test, ()):
        return True
    if name.startswith("uvm_set_type_override"):
        return True
    return name in DEFINING or name in DEFINING_PER_TEST.get(test, ())


def verdict(test: str, notes: list[str], dropped: list[str]) -> str:
    """How far the built program is from the entry that asked for it."""
    if any(is_defining(test, name) for name in dropped):
        return "hollow"
    return "partial" if dropped else "faithful"


def check_defining() -> None:
    """DEFINING_PER_TEST names options those entries actually ask for."""
    entries = {entry["test"]: entry for entry in testlist() if entry.get("test")}
    for test, names in DEFINING_PER_TEST.items():
        if test not in entries:
            raise BuildError(f"DEFINING_PER_TEST names {test}, which is not in "
                             f"the testlist")
        asked = {name for name, _ in gen_opts(entries[test])}
        stale = sorted(set(names) - asked)
        if stale:
            raise BuildError(
                f"DEFINING_PER_TEST[{test}] names {', '.join(stale)}, which "
                f"the entry no longer asks for")


def translate(entry: dict) -> tuple[list[str], list[str]]:
    """pyflow flags for one entry, and the options it cannot honour.

    The second half is the point. Dropping an option changes what the program
    tests, so each one comes back as a line naming the option and the reason,
    which ends up in the manifest and in `run_tests.py`'s report.
    """
    accepted, streams = pyflow_arguments(), pyflow_streams()
    options: list[str] = []
    notes: list[str] = []
    dropped: list[str] = []

    def drop(name: str, opt: str, why: str) -> None:
        notes.append(f"{opt}: {why}")
        dropped.append(name)

    rand_instr_test = entry.get("gen_test") == "riscv_rand_instr_test"
    for name, value in gen_opts(entry):
        opt = f"+{name}={value}"
        if name in ("require_signature_addr", "signature_addr"):
            # Set for every program built here; see SIGNATURE_ADDR.
            continue
        if name == "instr_cnt" and rand_instr_test:
            # Not a limitation of pyflow: `riscv_rand_instr_test::randomize_cfg`
            # assigns cfg.instr_cnt = 10000 after the config has read the
            # plusarg, in the SystemVerilog as well as in pyflow, so this entry
            # gets 10,000 instructions upstream too. Recorded because the entry
            # asks for something else and neither flow gives it. The 10000 is
            # put on the command line below, for the reason RAND_INSTR_CNT
            # gives.
            notes.append(f"{opt}: overridden to {RAND_INSTR_CNT} by "
                         f"riscv_rand_instr_test.randomize_cfg, which assigns "
                         f"it after the config has read the plusarg -- upstream "
                         f"does the same")
            continue
        if name in VACUOUS:
            # Recorded, but not counted against the entry: see VACUOUS.
            notes.append(f"{opt}: not passed, and costs nothing -- "
                         f"{VACUOUS[name]}")
            continue
        if name in NEVER_GENERATED and value == "0":
            drop(name, opt, NEVER_GENERATED[name])
            continue
        if name == "num_of_sub_program" and value != "0":
            drop(name, opt, SUB_PROGRAM_REASON)
            continue
        if name.startswith("directed_instr_"):
            stream = value.split(",")[0]
            if stream in BROKEN_STREAMS:
                drop(name, opt, BROKEN_STREAMS[stream])
                continue
            if stream not in streams:
                drop(name, opt,
                     f"{stream} is not in pyflow's stream factory")
                continue
        if name not in accepted:
            drop(name, opt, reason(name))
            continue
        if name in INERT:
            drop(name, opt, f"accepted by pyflow and never read -- "
                            f"{INERT[name]}")
            continue
        if name in UNREACHABLE:
            drop(name, opt, f"accepted by pyflow and read on a path it never "
                            f"takes -- {UNREACHABLE[name]}")
            continue
        if name == "enable_bitmanip_groups":
            # nargs='*', and pyflow spells the groups in upper case.
            options.append(f"--{name}")
            options += [group.strip().upper() for group in value.split(",")]
            continue
        options += [f"--{name}", value]

    if rand_instr_test:
        # See RAND_INSTR_CNT. Appended last so it wins over generate.py's own
        # --instr_cnt, which argparse resolves to the last occurrence.
        options += ["--instr_cnt", str(RAND_INSTR_CNT)]

    asked_for_rom = any(name == "gen_debug_section" and value == "1"
                        for name, value in gen_opts(entry))
    if needs_debug_rom(entry) and asked_for_rom:
        # Without `+gen_debug_section=1` upstream's debug ROM is a bare `dret`
        # too, so a note there would be false. With it, upstream generates a
        # program into the ROM and this does not.
        notes.append("the UVM class sends the core into the debug ROM, and "
                     "pyflow's gen_debug_rom is a stub: `debug_rom:` here is "
                     "the bare `dret` riscv_debug_rom_gen emits when "
                     "gen_debug_section is 0, not the ROM this entry asks for")
        dropped.append("debug_rom")
    return options, notes, dropped


def target_for(entry: dict, default: str) -> str:
    """The pyflow target one entry needs.

    `+enable_b_extension=1` only means anything if the target's `supported_isa`
    contains RV32B, which of pyflow's six targets is true of `rv32imcb` alone.
    Generating the three bitmanip entries against the default rv32imc target
    would produce a program with no B instructions in it and a clean link, which
    is the worst of both: the entry would look built and would test nothing.
    """
    if any(name == "enable_b_extension" for name, _ in gen_opts(entry)):
        target = PYGEN_SRC / "target" / "rv32imcb" / "riscv_core_setting.py"
        if not target.is_file():
            raise BuildError("pyflow has no rv32imcb target; the bitmanip "
                             "entries have nowhere to generate against")
        return "rv32imcb"
    return default


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
    options, notes, dropped = translate(entry)
    record = {"test": name,
              "rtl_test": entry.get("rtl_test", ""),
              "gen_test": gen_test,
              "gen_opts": entry.get("gen_opts", "").split(),
              "sim_opts": entry.get("sim_opts", "").split(),
              "timeout_s": entry.get("timeout_s", ""),
              "march": march_for(entry, args.march),
              "target": target_for(entry, args.target),
              "unsupported": notes,
              "verdict": verdict(name, notes, dropped)}
    if not gen_test:
        # riscv_csr_test is generated by riscv-dv's gen_csr_test.py from the
        # target's CSR description, not by the instruction generator at all.
        record["error"] = "entry has no gen_test; not a generator program"
        record["verdict"] = "not generated"
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
            if generate(1, args.instructions, seed, record["target"], name=name,
                        options=options, gen_test=gen_test,
                        timeout=args.generate_timeout) == 0:
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
    check_unreachable()
    check_defining()
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
        detail = f"{record['bytes']:>8} bytes  {record['verdict']}"
        if notes:
            detail += f", {len(notes)} note(s)"
        print(f"  {record['test']:<42} {detail}")
    for record in records:
        for note in record["unsupported"]:
            print(f"    {record['test']}: {note}")
    tally: dict[str, int] = {}
    for record in records:
        if "error" not in record:
            tally[record["verdict"]] = tally.get(record["verdict"], 0) + 1
    print(f"\nbuilt {len(records) - failed} of {len(records)} test program(s): "
          + ", ".join(f"{count} {name}" for name, count in sorted(tally.items())))
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
    parser.add_argument("--generate-timeout", type=int, default=1200,
                        help="wall-clock seconds one pyflow attempt may take; "
                             "pyflow hangs rather than failing when a worker "
                             "calls sys.exit")
    parser.add_argument("--list-tests", action="store_true",
                        help="print the testlist entries and stop")
    args = parser.parse_args(argv)

    if args.list_tests:
        check_defining()
        for entry in testlist():
            options, notes, dropped = translate(entry)
            mark = (verdict(entry["test"], notes, dropped)
                    if entry.get("gen_test") else "not generated")
            print(f"  {entry['test']:<42} {entry.get('rtl_test', ''):<46} "
                  f"{mark}"
                  f"{'  (' + str(len(dropped)) + ' dropped)' if dropped else ''}")
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
