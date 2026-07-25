"""Every measured benchmark binary must be optimized the same way.

Two independent flags decide this, and missing either silently biases the
comparison the guard enforces:

* ``-MAKEFLAGS OPT_FAST`` reaches only the files Verilator generates.
* ``-CFLAGS`` reaches the C++ testbench sources named on the command line,
  which Verilator otherwise compiles with no optimization flag at all.

A suite that sets the first but not the second measures an unoptimized cpptb
against an optimized model.
"""

import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MAKEFILE = (REPO / "Makefile").read_text(encoding="utf-8")

SUITES = {
    "AUTHORING_CORE_OPT_FAST",
    "PERIPHERAL_SUITE_OPT_FAST",
    "HEAVY_SUITE_OPT_FAST",
    "OPEN_CORES_OPT_FAST",
}


class SharedDefaultTests(unittest.TestCase):
    def test_shared_default_is_o3(self):
        self.assertIn("CPPTB_BENCH_OPT_FAST ?= -O3", MAKEFILE)

    def test_every_suite_derives_from_the_shared_default(self):
        for suite in sorted(SUITES):
            with self.subTest(suite=suite):
                self.assertIn(
                    f"{suite} ?= $(CPPTB_BENCH_OPT_FAST)",
                    MAKEFILE,
                    f"{suite} must follow the shared benchmark optimization "
                    "default so suites stay comparable",
                )


MEASURED_TOP_MODULES = {
    # pure SystemVerilog
    "peripheral_suite_sv_tb", "authoring_core_sv_tb", "force_direct_sv_tb",
    "heavy_benchmark_sv_tb", "open_cores_sv_tb",
    # C++ DPI
    "dpi_peripheral_suite", "dpi_authoring_core", "dpi_heavy_benchmark",
    "dpi_open_cores_benchmark",
    # C++ VPI
    "vpi_peripheral_suite", "authoring_core_vpi_top", "heavy_benchmark_vpi_top",
    "open_cores_benchmark_top",
}

COCOTB_RUNNERS = [
    "benchmarks/framework_comparison/testbenches/cocotb/run_cocotb.py",
    "benchmarks/framework_comparison/heavy_suite/testbenches/cocotb/run_cocotb.py",
    "benchmarks/framework_comparison/open_cores/testbenches/cocotb/run_cocotb.py",
    "benchmarks/peripheral_suite/testbenches/cocotb/run_cocotb.py",
]


def _verilator_invocations():
    """Yield (line, text) for each verilator command, joining continuations."""
    lines = MAKEFILE.split("\n")
    index = 0
    while index < len(lines):
        if re.match(r"\s*verilator ", lines[index]):
            start, buffer = index, []
            while index < len(lines):
                buffer.append(lines[index])
                if not lines[index].rstrip().endswith("\\"):
                    break
                index += 1
            yield start + 1, "\n".join(buffer)
        index += 1


class MeasuredBinaryTests(unittest.TestCase):
    """Every mode of a comparison must be built with the same optimization.

    The guard compares modes against each other, so optimizing one mode and not
    another silently changes the result it reports.
    """

    def test_every_measured_model_is_optimized(self):
        seen = set()
        for line, text in _verilator_invocations():
            match = re.search(r"--top-module (\S+)", text)
            if not match:
                continue
            top = match.group(1).replace("$$(", "$(")
            if top not in MEASURED_TOP_MODULES:
                continue
            seen.add(top)
            with self.subTest(top=top, line=line):
                self.assertIn(
                    "OPT_FAST=", text,
                    f"{top} at Makefile:{line} builds its model without "
                    "OPT_FAST, so it is measured against optimized modes",
                )
        missing = MEASURED_TOP_MODULES - seen
        self.assertEqual(missing, set(), f"measured targets not found: {missing}")

    def test_cocotb_models_match_the_other_modes(self):
        for runner in COCOTB_RUNNERS:
            with self.subTest(runner=runner):
                source = (REPO / runner).read_text(encoding="utf-8")
                self.assertIn(
                    'f"OPT_FAST={_opt_fast()}"', source,
                    "cocotb drives Verilator itself; without OPT_FAST its model "
                    "is built less optimized than the modes it is compared to",
                )
                self.assertIn("CPPTB_BENCH_OPT_FAST", source)

    def test_shared_default_is_exported_for_the_cocotb_runners(self):
        self.assertIn("export CPPTB_BENCH_OPT_FAST", MAKEFILE)


class FlagCoverageTests(unittest.TestCase):
    def test_no_measured_build_hardcodes_an_optimization_level(self):
        # A literal level cannot be swept and drifts from the other suites.
        hardcoded = re.findall(r'-MAKEFLAGS "OPT_FAST=(-O[0-9s]+)"', MAKEFILE)
        self.assertEqual(
            hardcoded, [],
            f"hardcoded OPT_FAST levels found: {hardcoded}; use a suite variable",
        )

    def test_every_cpptb_cflags_carries_an_optimization_variable(self):
        # Each -CFLAGS that compiles cpptb testbench sources is C++20; every one
        # of them must also request optimization.
        cflags = re.findall(r'-CFLAGS "(-std=c\+\+20[^"]*)"', MAKEFILE)
        self.assertTrue(cflags, "expected cpptb -CFLAGS invocations")
        for flags in cflags:
            with self.subTest(flags=flags[:60]):
                self.assertRegex(
                    flags,
                    r"\$\$?\([A-Z_]*OPT_FAST\)",
                    "testbench sources would be compiled unoptimized while the "
                    "pure-SystemVerilog model is optimized",
                )

    def test_opt_fast_reaches_generated_and_testbench_sources(self):
        # Inside a `define` the variables are written $$(...); normalise so one
        # assertion covers both spellings.
        normalised = MAKEFILE.replace("$$(", "$(")
        for suite in sorted(SUITES):
            with self.subTest(suite=suite):
                self.assertIn(
                    f'-MAKEFLAGS "OPT_FAST=$({suite})"', normalised,
                    f"{suite} must optimize Verilator's generated model",
                )
                self.assertRegex(
                    normalised, rf'-CFLAGS "-std=c\+\+20 \$\({suite}\)',
                    f"{suite} must optimize the C++ testbench sources",
                )


if __name__ == "__main__":
    unittest.main()
