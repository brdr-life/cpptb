#!/usr/bin/env python3
"""Build Ibex's icache UVM testbench (dv/uvm/icache) with Verilator.

This is Ibex's second UVM testbench, and a block-level one: the DUT is
`ibex_icache` alone, driven by a core agent on the fetch side and a memory
agent on the bus side, with a scoreboard that models the cache. Upstream signs
it off with VCS and runs it through OpenTitan's dvsim; the tool list in
`dv/ibex_icache_sim_cfg.hjson` does not include Verilator.

    python3 build_tb.py
    python3 build_tb.py --jobs 4

The build description is a FuseSoC CAPI=2 graph rather than the `.f` file lists
`dv/uvm/core_ibex` uses. `corelist.py` walks it in place; see the docstring
there for why that rather than driving fusesoc.

Standard library only, matching the other tools here.
"""

from __future__ import annotations

import argparse
import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

import corelist

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
IBEX = ROOT / "deps" / "ibex"
LOWRISC_IP = IBEX / "vendor" / "lowrisc_ip"
ICACHE = IBEX / "dv" / "uvm" / "icache"
UVM = ROOT / "deps" / "uvm_core" / "src"
BUILD = HERE / "build"
OBJ = BUILD / "obj"

# The core and target `ibex_icache_sim_cfg.hjson` names in `fusesoc_core`.
TOP_CORE = "lowrisc:dv:ibex_icache_sim:0.1"
TOP_TARGET = "sim"

# The build defines `common_sim_cfg.hjson` applies to every OpenTitan-style DV
# build, reproduced here because dvsim is not what drives this. UVM_REGEX_NO_DPI
# is deliberately not among them: it is in upstream's list, and UVM's own
# uvm_regex.cc is compiled by shims/uvm_dpi_verilator.cc, so the DPI regex is
# available and is the faster path.
COMMON_DEFINES = {
    "UVM": "",
    "UVM_NO_DEPRECATED": "",
    "UVM_REG_ADDR_WIDTH": "32",
    "UVM_REG_DATA_WIDTH": "32",
    "UVM_REG_BYTENABLE_WIDTH": "4",
    "SIMULATION": "",
    "DUT_HIER": "tb.dut",
}


class BuildError(RuntimeError):
    pass


# ---------------------------------------------------------------------------
# Overlays
#
# Exact-text patches applied to copies under build/overlay/, never to deps/.
# A replacement that stops matching exactly once fails the build with the file
# named, so an upstream edit is something to look at rather than something that
# silently changes what is built.
# ---------------------------------------------------------------------------

OVERLAYS: list[tuple[Path, str, str]] = [
    # Both agent interfaces open their driver clocking block with
    #
    #     clocking driver_cb @(posedge clk);
    #       default output negedge;
    #
    # which is an edge-based output skew. Verilator does not implement it:
    # "Unsupported: clocking event edge override".
    #
    # Dropping the line and taking the default #0 output skew drives the same
    # value into the same posedge of the DUT. A clocking block output written
    # with a skew of 0 is driven in the NBA region of the clocking event, so a
    # design sampling on that same posedge still reads the old value and picks
    # the new one up on the next posedge -- exactly where the negedge drive
    # would have put it. Upstream says as much in the comment above the line:
    # the negedge is there to make dumped waves easier to read, not because
    # the design needs it.
    #
    # The alternative, moving the clocking event to @(negedge clk), is what
    # ports/core_ibex_uvm does for irq_if.sv. It is worse here, because
    # driver_cb in the core interface has inputs as well: valid and err would
    # then be sampled after the posedge rather than before it, which advances
    # every input the driver reads by a cycle.
    (
        ICACHE / "dv/ibex_icache_core_agent/ibex_icache_core_if.sv",
        "\n"
        "    // Drive signals on the following negedge: this isn't needed by"
        " the design, but makes it\n"
        "    // slightly easier to read dumped waves.\n"
        "    default output negedge;\n",
        "\n"
        "    // The output skew that was here drove signals on the following\n"
        "    // negedge, for wave readability rather than for the design. The\n"
        "    // default #0 skew lands the same value at the same posedge of\n"
        "    // the DUT; see build_tb.py.\n",
    ),
    (
        ICACHE / "dv/ibex_icache_mem_agent/ibex_icache_mem_if.sv",
        "  default clocking driver_cb @(posedge clk);\n"
        "    default output negedge;\n",
        "  default clocking driver_cb @(posedge clk);\n"
        "    // The output skew that was here is dropped; see build_tb.py and\n"
        "    // the note on the core interface.\n",
    ),
    # tb.sv overrides a parameter the DUT does not have. `ibex_icache` calls
    # it `TweakInfection`; `ICacheTweakInfection` is the name the wrapper
    # parameter carries in ibex_if_stage.sv, ibex_core.sv and ibex_top.sv,
    # which is presumably where it was copied from. Verilator reports
    # "Parameter not found: 'ICacheTweakInfection'" and stops.
    #
    # This is an upstream defect rather than anything about Verilator: no
    # simulator can elaborate tb.sv as vendored. See README.md.
    (
        ICACHE / "dv/tb/tb.sv",
        "      .ICacheTweakInfection (ICacheTweakInfection),\n",
        "      .TweakInfection       (ICacheTweakInfection),\n",
    ),
    # `DV_COMMON_CLK_CONSTRAINT is a weighted `dist` over 5..100 MHz, and
    # ibex_icache_env_cfg derives from the class that carries it and adds
    #
    #     constraint clk_freq_50_c { clk_freq_mhz == 50; }
    #
    # 50 is inside the distribution's support, so the pair is satisfiable.
    # This simulator draws a sample from the distribution first and then
    # asserts equality against that sample, so the whole solve is unsatisfiable
    # unless the sample happened to be 50. Measured over 25 seeds of this
    # testbench, one got past `DV_CHECK_RANDOMIZE_FATAL in dv_base_test; the
    # other 24 died at time 0 with "Randomization failed!" and no diagnostic.
    # Reduced case, with the rates, in shims/verilator_dist_plus_equality.sv.
    #
    # Replacing the distribution with its own support as an `inside` keeps
    # every frequency the constraint allows and drops the weights. In this
    # testbench that costs nothing at all: clk_freq_50_c determines the value,
    # and the `foreach` arm of the same constraint iterates clk_freqs_mhz,
    # which is empty here because the icache has no RAL model.
    (
        LOWRISC_IP / "dv/sv/dv_utils/dv_macros.svh",
        "`define DV_COMMON_CLK_CONSTRAINT(FREQ_) \\\n"
        "  FREQ_ dist { \\\n"
        "    [5:23]  :/ 2, \\\n"
        "    [24:25] :/ 2, \\\n"
        "    [26:47] :/ 1, \\\n"
        "    [48:50] :/ 2, \\\n"
        "    [51:95] :/ 1, \\\n"
        "    96      :/ 1, \\\n"
        "    [97:99] :/ 1, \\\n"
        "    100     :/ 1  \\\n"
        "  };\n",
        "`define DV_COMMON_CLK_CONSTRAINT(FREQ_) \\\n"
        "  FREQ_ inside {[5:100]};\n",
    ),
    # lowRISC's shared DV library excludes itself under Verilator:
    # clk_rst_if.sv wraps its UVM includes and imports in `ifndef VERILATOR, so
    # the interface compiles without `DV_CHECK_FATAL and fails at the first use
    # of it. The guard dates from when no open simulator could elaborate UVM.
    (
        LOWRISC_IP / "dv/sv/common_ifs/clk_rst_if.sv",
        "`ifndef VERILATOR\n"
        "  // include macros and import pkgs\n"
        "  `include \"dv_macros.svh\"\n"
        "  `include \"uvm_macros.svh\"\n"
        "  import uvm_pkg::*;\n"
        "  import common_ifs_pkg::*;\n"
        "`endif\n",
        "  // include macros and import pkgs\n"
        "  `include \"dv_macros.svh\"\n"
        "  `include \"uvm_macros.svh\"\n"
        "  import uvm_pkg::*;\n"
        "  import common_ifs_pkg::*;\n",
    ),
    # clk_rst_if declares `logic o_rst_n;` with no initialiser and then
    # apply_reset() drives it low. On a 4-state simulator that is X -> 0, a
    # real falling edge, so every `always_ff @(posedge clk or negedge rst_ni)`
    # takes its reset branch. Verilator zero-initialises it, the same
    # assignment is 0 -> 0, there is no edge, and the asynchronous resets never
    # fire while rst_n sits at 0 looking perfectly asserted.
    #
    # Found and diagnosed in ports/core_ibex_uvm; the same file is in this
    # build and the same fix applies. Starting it deasserted gives apply_reset
    # the 1 -> 0 edge it assumes.
    (
        LOWRISC_IP / "dv/sv/common_ifs/clk_rst_if.sv",
        "  logic o_rst_n;\n",
        "  logic o_rst_n = 1'b1;\n",
    ),
    # IEEE 1800 13.2.2 makes it illegal to write an automatic task's output
    # argument after a timing control, and csr_rd_sub and mem_rd_sub in
    # csr_utils_pkg.sv do exactly that: they hand their own output formals to
    # uvm_reg::read inside a `fork ... join_any; disable fork`. Verilator
    # enforces the rule and the commercial simulators do not.
    #
    # The fix writes automatic locals inside the fork and copies them out once
    # it has joined, which is what the code already means: the outer `join`
    # guarantees the fork has finished before the task returns.
    #
    # csr_utils_pkg.sv is in this compile only because dv_lib_pkg imports it.
    # Nothing in dv/uvm/icache names either task.
    (
        LOWRISC_IP / "dv/sv/csr_utils/csr_utils_pkg.sv",
        "    if (backdoor) begin\n"
        "      value = csr_peek(ptr, check);\n"
        "      status = UVM_IS_OK;\n"
        "      return;\n"
        "    end\n",
        "    uvm_reg_data_t value_l;\n"
        "    uvm_status_e   status_l;\n"
        "    if (backdoor) begin\n"
        "      value = csr_peek(ptr, check);\n"
        "      status = UVM_IS_OK;\n"
        "      return;\n"
        "    end\n",
    ),
    (
        LOWRISC_IP / "dv/sv/csr_utils/csr_utils_pkg.sv",
        "              csr_or_fld.field.read(.status(status), .value(value),"
        " .path(path), .map(map),\n"
        "                                    .prior(100));\n"
        "            end else begin\n"
        "              csr_or_fld.csr.read(.status(status), .value(value),"
        " .path(path), .map(map),\n"
        "                                  .prior(100));\n",
        "              csr_or_fld.field.read(.status(status_l), .value(value_l),"
        " .path(path), .map(map),\n"
        "                                    .prior(100));\n"
        "            end else begin\n"
        "              csr_or_fld.csr.read(.status(status_l), .value(value_l),"
        " .path(path), .map(map),\n"
        "                                  .prior(100));\n",
    ),
    (
        LOWRISC_IP / "dv/sv/csr_utils/csr_utils_pkg.sv",
        "              `DV_CHECK_EQ(status, UVM_IS_OK,\n"
        "                           $sformatf(\"trying to read csr/field %0s\","
        " ptr.get_full_name()),\n",
        "              `DV_CHECK_EQ(status_l, UVM_IS_OK,\n"
        "                           $sformatf(\"trying to read csr/field %0s\","
        " ptr.get_full_name()),\n",
    ),
    (
        LOWRISC_IP / "dv/sv/csr_utils/csr_utils_pkg.sv",
        "      end : isolation_fork\n"
        "    join\n"
        "  endtask\n"
        "\n"
        "  // backdoor read csr\n",
        "      end : isolation_fork\n"
        "    join\n"
        "    value  = value_l;\n"
        "    status = status_l;\n"
        "  endtask\n"
        "\n"
        "  // backdoor read csr\n",
    ),
    (
        LOWRISC_IP / "dv/sv/csr_utils/csr_utils_pkg.sv",
        "                            input  uvm_reg_frontdoor  user_ftdr ="
        " default_user_frontdoor);\n"
        "    fork\n"
        "      begin : isolating_fork\n"
        "        uvm_status_e status;\n",
        "                            input  uvm_reg_frontdoor  user_ftdr ="
        " default_user_frontdoor);\n"
        "    bit [31:0] data_l;\n"
        "    fork\n"
        "      begin : isolating_fork\n"
        "        uvm_status_e status;\n",
    ),
    (
        LOWRISC_IP / "dv/sv/csr_utils/csr_utils_pkg.sv",
        "            ptr.read(.status(status), .offset(offset), .value(data),"
        " .map(map), .prior(100));\n",
        "            ptr.read(.status(status), .offset(offset), .value(data_l),"
        " .map(map), .prior(100));\n",
    ),
    (
        LOWRISC_IP / "dv/sv/csr_utils/csr_utils_pkg.sv",
        "      end : isolating_fork\n"
        "    join\n"
        "  endtask : mem_rd_sub\n",
        "      end : isolating_fork\n"
        "    join\n"
        "    data = data_l;\n"
        "  endtask : mem_rd_sub\n",
    ),
]


def apply_overlays() -> dict[str, str]:
    """Write the patched copies and return upstream path -> overlay path."""
    out = BUILD / "overlay"
    if out.is_dir():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    patched: dict[Path, str] = {}
    for source, old, new in OVERLAYS:
        if not source.is_file():
            raise BuildError(f"overlay target missing: {source}")
        text = patched.get(source, source.read_text(encoding="utf-8"))
        if text.count(old) != 1:
            raise BuildError(
                f"{source.name}: the text this build patches is no longer "
                f"present exactly once; upstream has changed and the overlay "
                f"in build_tb.py needs revisiting")
        patched[source] = text.replace(old, new)

    mapping: dict[str, str] = {}
    for source, text in patched.items():
        target = out / source.name
        if target.exists():
            raise BuildError(f"two overlays share the basename {source.name}")
        target.write_text(text, encoding="utf-8")
        mapping[str(source)] = str(target)
    return mapping


# ---------------------------------------------------------------------------
# The Verilator command
# ---------------------------------------------------------------------------

def verilator_command(jobs: int, extra: list[str]) -> tuple[list[str], str]:
    if not UVM.is_dir():
        raise BuildError(f"no UVM at {UVM}\n"
                         f"run: python3 {ROOT / 'fetch.py'} uvm_core")

    index = corelist.Index(IBEX)
    files = corelist.walk(index, TOP_CORE, TOP_TARGET)
    sources, incdirs, control = corelist.sources_and_incdirs(files)
    top = corelist.toplevel(index, TOP_CORE, TOP_TARGET)

    overlay = apply_overlays()
    sources = [Path(overlay.get(str(path), str(path))) for path in sources]

    command = [
        "verilator", "--binary", "--timing",
        # UVM's plusarg list comes from vpi_get_vlog_info, so without --vpi
        # uvm_cmdline_processor sees no arguments and +UVM_TESTNAME never
        # reaches run_test(). See shims/uvm_dpi_verilator.cc.
        "--vpi",
        # Verilator makes warnings fatal by default. This compile raises a few
        # thousand, nearly all WIDTHTRUNC and WIDTHEXPAND out of UVM's own
        # source and lowRISC's DV library. They stay in the log.
        "-Wno-fatal",
        # Verilator's skip-identical check hashes the command line and the
        # files it read last time, and notices neither when a source moves into
        # build/overlay/ -- an include path is not on the command line and the
        # upstream file has not changed. An overlay then silently has no
        # effect. Diagnosed in ports/core_ibex_uvm; the cost is a full
        # re-verilation per build.
        "--no-skip-identical",
        "-j", str(jobs),
        "--top-module", top,
        # common_sim_cfg.hjson says 1ns/1ps. Verilator's minimum time precision
        # here is set the same.
        "--timescale", "1ns/1ps",
        "--Mdir", str(OBJ),
        "-o", "ibex_icache_tb",
        # The overlay directory first, so a patched `include wins over the
        # upstream copy. Only files this build patches are in it.
        f"+incdir+{BUILD / 'overlay'}",
        f"+incdir+{UVM}",
    ]
    command += [f"+incdir+{path}" for path in incdirs]
    command += [f"+define+{key}={value}" if value else f"+define+{key}"
                for key, value in COMMON_DEFINES.items()]
    command += [str(path) for path in control]
    command += [str(UVM / "uvm_pkg.sv")]
    command += [str(path) for path in sources]
    command += [str(HERE / "shims" / "uvm_dpi_verilator.cc")]
    command += ["-CFLAGS", shlex.join([f"-I{UVM}/dpi"])]
    command += extra
    return command, top


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    # Half the cores, not all of them: compiling UVM together with the design
    # peaks well over a gigabyte per job, and oversubscribing gets the compiler
    # killed part way through with "fatal error: Killed signal terminated
    # program cc1plus", which reads like a compiler bug rather than OOM.
    parser.add_argument("--jobs", type=int,
                        default=max(1, (os.cpu_count() or 4) // 2))
    parser.add_argument("--show", action="store_true",
                        help="print the command without running it")
    parser.add_argument("--list-sources", action="store_true",
                        help="print the resolved file list and stop")
    parser.add_argument("extra", nargs="*",
                        help="further options passed to Verilator")
    args = parser.parse_args(argv)

    if args.list_sources:
        try:
            index = corelist.Index(IBEX)
            files = corelist.walk(index, TOP_CORE, TOP_TARGET)
        except corelist.CoreError as error:
            print(f"build_tb: {error}", file=sys.stderr)
            return 1
        for entry in files:
            kind = "include" if entry.is_include else entry.file_type or "-"
            print(f"{kind:20s} {entry.path.relative_to(IBEX)}  [{entry.core}]")
        return 0

    BUILD.mkdir(parents=True, exist_ok=True)
    try:
        command, top = verilator_command(args.jobs, args.extra)
    except (BuildError, corelist.CoreError) as error:
        print(f"build_tb: {error}", file=sys.stderr)
        return 1

    if args.show:
        print(shlex.join(command))
        return 0

    log = BUILD / "compile_tb.log"
    # The command is a page wide with a hundred absolute paths in it. Keeping
    # it beside the log rather than at the top of it leaves the log greppable.
    (BUILD / "compile_tb.cmd").write_text(shlex.join(command) + "\n",
                                          encoding="utf-8")
    print(f"build_tb: top module {top}, logging to {log.relative_to(HERE)}")
    with log.open("w", encoding="utf-8") as handle:
        completed = subprocess.run(command, cwd=BUILD, stdout=handle,
                                   stderr=subprocess.STDOUT, check=False)
    if completed.returncode != 0:
        print(f"build_tb: failed; see {log}", file=sys.stderr)
        return completed.returncode
    print(f"build_tb: built {OBJ / 'ibex_icache_tb'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
