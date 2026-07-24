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
