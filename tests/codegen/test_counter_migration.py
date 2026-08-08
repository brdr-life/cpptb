import re
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
EXAMPLES = REPO / "examples"
ORDINARY_EXAMPLES = {
    "counter": ("counter", "counter_sv_tb.sv", "kCountCycles", 8),
    "multiclock": (
        "dual_clock_mailbox",
        "dual_clock_mailbox_sv_tb.sv",
        "kTransferCount",
        16,
    ),
    "timer_only": (
        "timer_only_probe",
        "timer_only_probe_sv_tb.sv",
        "kCadenceSamples",
        9,
    ),
    "fifo_scoreboard": (
        "stream_fifo",
        "stream_fifo_sv_tb.sv",
        "kWordCount",
        24,
    ),
    "component_fifo": (
        "component_fifo",
        "component_fifo_sv_tb.sv",
        "kWordCount",
        24,
    ),
    "apb_regfile": (
        "apb_regfile",
        "apb_regfile_sv_tb.sv",
        "kRegisterTransactions",
        12,
    ),
    "watchdog_timeout": (
        "stalling_responder",
        "stalling_responder_sv_tb.sv",
        "kTransactionCount",
        8,
    ),
}
GENERATED_MODULES = {
    **{directory: values[0] for directory, values in ORDINARY_EXAMPLES.items()},
    "fault_injection": "fault_injection",
    "rich_data": "rich_data",
}


class OrdinaryExampleMigrationTests(unittest.TestCase):
    def test_cpp_and_systemverilog_use_exact_fixed_workloads(self):
        for directory, (_, sv_name, constant, expected) in (
            ORDINARY_EXAMPLES.items()
        ):
            with self.subTest(example=directory):
                example = EXAMPLES / directory
                cpp = (example / "testbench.cpp").read_text()
                systemverilog = (
                    example / "systemverilog" / sv_name
                ).read_text()
                cpp_count = re.search(
                    rf"{constant}\s*=\s*(\d+)", cpp
                )
                sv_count = re.search(
                    rf"{constant}\s*=\s*(\d+)", systemverilog
                )

                self.assertIsNotNone(cpp_count)
                self.assertIsNotNone(sv_count)
                self.assertEqual(cpp_count.group(1), sv_count.group(1))
                self.assertEqual(int(cpp_count.group(1)), expected)
                self.assertNotIn("iterations()", cpp)
                self.assertNotIn("$value$plusargs", systemverilog)

    def test_examples_use_public_surface_and_clean_source_tree(self):
        for directory, module in GENERATED_MODULES.items():
            with self.subTest(example=directory):
                example = EXAMPLES / directory
                testbench = (example / "testbench.cpp").read_text()

                self.assertRegex(
                    testbench, r"Task<void>\s+\w+\(Dut dut, TestContext& test\)"
                )
                self.assertIn("CPPTB_REGISTER_TEST(", testbench)
                self.assertNotRegex(testbench, r"class\s+\w+Tb\b")
                self.assertIn('#include "dut.hpp"', testbench)
                self.assertIn("using cpptb::Dut;", testbench)
                self.assertNotIn(f'#include "{module}_dut.hpp"', testbench)
                self.assertFalse((example / "generated").exists())

                self.assertEqual(list(example.glob("*.dpi.json")), [])
                for obsolete in (
                    "framework.hpp",
                    "framework.cpp",
                    "dpi_transport.cpp",
                ):
                    self.assertFalse((example / obsolete).exists())

        makefile = (REPO / "Makefile").read_text()
        self.assertIn("CPPTB_$(2)_GENERATED_DIR", makefile)
        self.assertIn("$$(CPPTB) build $$(CPPTB_$(2)_PROJECT_ARGS)", makefile)
        self.assertNotIn("CPPTB_$(2)_FINAL_CODEGEN_COMMAND", makefile)

        standalone = (EXAMPLES / "counter" / "Makefile").read_text()
        self.assertIn("cpptb\n", standalone)
        self.assertNotIn("verilator --binary", standalone)
        self.assertNotIn("discover_counter_clocks.cpp", standalone)

    def test_clock_timing_is_owned_by_cpp_testbenches(self):
        makefile = (REPO / "Makefile").read_text()
        self.assertNotIn("CPPTB_COUNTER_ITERS", makefile)
        self.assertNotIn("CPPTB_MULTICLOCK_ITERS", makefile)
        self.assertNotIn("--clock ", makefile)
        self.assertNotIn("--primary-clock", makefile)
        self.assertNotIn("--edge-observer", makefile)

        single_clock_examples = (
            "counter",
            "fifo_scoreboard",
            "component_fifo",
            "apb_regfile",
            "watchdog_timeout",
        )
        for directory in single_clock_examples:
            with self.subTest(example=directory):
                testbench = (EXAMPLES / directory / "testbench.cpp").read_text()
                # set_now: the examples run deferred_writes = true, where
                # pre-clock initialization uses the immediate escape hatch
                # (cocotb's setimmediatevalue). Ownership is what matters:
                # the clock pin is initialized and started from C++.
                self.assertIn("dut.clk.set_now(0);", testbench)
                self.assertIn("test.start_clock(dut.clk, 10_ns);", testbench)

        multiclock = (EXAMPLES / "multiclock" / "testbench.cpp").read_text()
        self.assertIn("test.start_clock(dut.write_clk, 4_ns);", multiclock)
        self.assertIn(
            "test.start_clock(dut.read_clk, 6_ns, 1_ns);", multiclock
        )
        self.assertIn("RisingEdge{dut.output_clk}", multiclock)


if __name__ == "__main__":
    unittest.main()
