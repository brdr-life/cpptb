import tempfile
import unittest
from pathlib import Path

from cpptb.codegen.generate_dpi_bindings import (
    CodegenError,
    Port,
    build_tree,
    discover_ports,
    map_ports,
    render_cpp_binding,
    render_cpp_dut,
    render_sv,
    time_literal_femtoseconds,
    validate_clock_ports,
    validate_transport_ports,
    write_or_check,
)


def manifest():
    return {
        "module": "sample_dut",
        "top_module": "dpi_sample_dut",
        "namespace": "cpptb::sample",
        "root_type": "SampleDut",
        "clock": {"port": "clk"},
        "parameters": {},
        "path_rules": [
            {
                "prefix": "bus_",
                "path": "bus",
                "groups": [{"path": "apb", "members": ["PADDR", "PWDATA"]}],
            }
        ],
        "type_names": {"": "SampleDut", "bus": "BusDut", "bus.apb": "ApbBus"},
        "outputs": {
            "cpp_include": "generated/sample_dut.hpp",
        },
    }


class CodegenTests(unittest.TestCase):
    def test_maps_flat_ports_to_typed_hierarchy(self):
        ports = map_ports(
            [
                Port("clk", "input", 1),
                Port("bus_PADDR", "input", 12),
                Port("bus_PWDATA", "input", 32),
                Port("bus_ready", "output", 1),
            ],
            manifest(),
        )

        self.assertEqual(ports[1].cpp_path, ("bus", "apb", "PADDR"))
        self.assertEqual(ports[3].cpp_path, ("bus", "ready"))

        tree = build_tree(ports)
        header = render_cpp_dut(ports, tree, manifest(), "sample.json")
        binding = render_cpp_binding(ports, tree, manifest(), "sample.json")
        wrapper = render_sv(ports, manifest(), "sample.json")

        self.assertIn("struct ApbBus", header)
        self.assertIn("ApbBus apb;", header)
        self.assertIn("kSignalBusPaddr", binding)
        self.assertIn("logic [11:0] bus_PADDR;", wrapper)
        self.assertIn("bus_PADDR = out_words[SIGNAL_BUSPADDR][11:0];", wrapper)
        self.assertNotIn("bus_ready = out_words", wrapper)

    def test_discovers_elaborated_port_widths(self):
        ast = {
            "modulesp": [
                {
                    "type": "MODULE",
                    "name": "sample_dut",
                    "stmtsp": [
                        {
                            "type": "VAR",
                            "varType": "PORT",
                            "name": "data",
                            "direction": "OUTPUT",
                            "dtypep": "(A)",
                        }
                    ],
                }
            ],
            "typeTable": [
                {
                    "type": "BASICDTYPE",
                    "addr": "(A)",
                    "name": "logic",
                    "range": "15:0",
                }
            ],
        }

        self.assertEqual(discover_ports(ast, "sample_dut"), [Port("data", "output", 16)])

    def test_generates_multiple_clock_drivers_and_delay_transport(self):
        config = manifest()
        del config["clock"]
        config["clocks"] = [
            {"port": "write_clk", "half_period": "2ns", "primary": True},
            {"port": "read_clk", "half_period": "3ns", "phase": "1ns"},
        ]
        ports = map_ports(
            [
                Port("write_clk", "input", 1),
                Port("read_clk", "input", 1),
                Port("bus_PADDR", "input", 12),
                Port("bus_ready", "output", 1),
            ],
            config,
        )
        tree = build_tree(ports)

        binding = render_cpp_binding(ports, tree, config, "sample.json")
        wrapper = render_sv(ports, config, "sample.json")

        self.assertIn("std::array<uint32_t, 2> kClockSignalIds", binding)
        self.assertNotIn("kSignalWriteClk,\n    kSignalReadClk,\n    kSignalBusPaddr", binding)
        self.assertIn("task automatic drive_clock_0", wrapper)
        self.assertIn("task automatic drive_clock_1", wrapper)
        self.assertNotIn("task automatic observe_clock_0", wrapper)
        self.assertNotIn("task automatic observe_clock_1", wrapper)
        self.assertIn("realtime next_edge", wrapper)
        self.assertIn("STEP_FALLING_EDGES", wrapper)
        self.assertIn("STEP_OUTPUTS_CHANGED", wrapper)
        self.assertIn("timeunit 1ps", wrapper)
        self.assertIn("TIMEPRECISION_FS = 1000", wrapper)
        self.assertIn("cpptb_dpi_init(iterations, TIMEPRECISION_FS)", wrapper)
        self.assertNotIn("PHASE_SAMPLE", wrapper)
        self.assertNotIn("PHASE_DRIVE", wrapper)
        self.assertNotIn("sample_delay", wrapper)
        self.assertIn("cpptb_dpi_next_timer_deadline", wrapper)

    def test_parses_timeprecision_for_delay_transport(self):
        self.assertEqual(time_literal_femtoseconds("1fs", "precision"), 1)
        self.assertEqual(time_literal_femtoseconds("10 ps", "precision"), 10_000)
        self.assertEqual(
            time_literal_femtoseconds("2ns", "precision"), 2_000_000
        )
        with self.assertRaises(CodegenError):
            time_literal_femtoseconds("0ps", "precision")
        with self.assertRaises(CodegenError):
            time_literal_femtoseconds("0.5ps", "precision")

    def test_supports_generated_testbench_and_dut_clock_sources(self):
        config = manifest()
        del config["clock"]
        config["clocks"] = [
            {
                "port": "core_clk",
                "source": "generated",
                "half_period": "2ns",
                "primary": True,
            },
            {"port": "scan_clk", "source": "testbench"},
            {"port": "gated_clk", "source": "dut"},
        ]
        ports = map_ports(
            [
                Port("core_clk", "input", 1),
                Port("scan_clk", "input", 1),
                Port("gated_clk", "output", 1),
                Port("bus_PADDR", "input", 12),
            ],
            config,
        )
        tree = build_tree(ports)

        validate_clock_ports(config, ports)
        binding = render_cpp_binding(ports, tree, config, "sample.json")
        wrapper = render_sv(ports, config, "sample.json")

        driven_section = binding.split("kDrivenSignalIds = {", 1)[1].split("};", 1)[0]
        self.assertIn("kSignalScanClk,", driven_section)
        self.assertIn("kSignalBusPaddr,", driven_section)
        self.assertNotIn("kSignalCoreClk,", driven_section)
        self.assertNotIn("kSignalGatedClk,", driven_section)
        self.assertIn("scan_clk = out_words[SIGNAL_SCANCLK][0];", wrapper)
        self.assertNotIn("core_clk = out_words[SIGNAL_CORECLK][0];", wrapper)
        self.assertIn("task automatic drive_clock_0", wrapper)
        self.assertNotIn("task automatic drive_clock_1", wrapper)
        self.assertNotIn("task automatic drive_clock_2", wrapper)
        self.assertNotIn("task automatic observe_clock_0", wrapper)
        self.assertIn("task automatic observe_clock_1", wrapper)
        self.assertIn("task automatic observe_clock_2", wrapper)

    def test_rejects_invalid_clock_source_port_combinations(self):
        ports = [
            Port("input_clk", "input", 1),
            Port("output_clk", "output", 1),
        ]

        config = manifest()
        config["clock"] = {"port": "output_clk", "source": "generated"}
        with self.assertRaisesRegex(CodegenError, "must be a DUT input"):
            validate_clock_ports(config, ports)

        config["clock"] = {"port": "input_clk", "source": "dut"}
        with self.assertRaisesRegex(CodegenError, "must be a DUT output"):
            validate_clock_ports(config, ports)

        config["clock"] = {
            "port": "input_clk",
            "source": "testbench",
            "half_period": "2ns",
        }
        with self.assertRaisesRegex(CodegenError, "cannot define half_period"):
            validate_clock_ports(config, ports)

    def test_supports_a_clockless_dut(self):
        config = manifest()
        del config["clock"]
        config["clocks"] = []
        ports = map_ports(
            [
                Port("bus_PADDR", "input", 12),
                Port("bus_ready", "output", 1),
            ],
            config,
        )
        tree = build_tree(ports)

        validate_clock_ports(config, ports)
        binding = render_cpp_binding(ports, tree, config, "sample.json")
        wrapper = render_sv(ports, config, "sample.json")

        self.assertIn("std::array<uint32_t, 0> kClockSignalIds", binding)
        self.assertNotIn("drive_clock_", wrapper)
        self.assertNotIn("observe_clock_", wrapper)
        self.assertNotIn("fork\n      join_none", wrapper)

    def test_rejects_ports_wider_than_transport(self):
        ast = {
            "modulesp": [
                {
                    "type": "MODULE",
                    "name": "sample_dut",
                    "stmtsp": [
                        {
                            "type": "VAR",
                            "varType": "PORT",
                            "name": "wide",
                            "direction": "INPUT",
                            "dtypep": "(A)",
                        }
                    ],
                }
            ],
            "typeTable": [
                {
                    "type": "BASICDTYPE",
                    "addr": "(A)",
                    "name": "logic",
                    "range": "63:0",
                }
            ],
        }

        ports = discover_ports(ast, "sample_dut")
        self.assertEqual(ports[0].width, 64)
        with self.assertRaisesRegex(CodegenError, "at most 32 bits"):
            validate_transport_ports(ports)

    def test_check_detects_stale_generated_file(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "generated.hpp"
            output.write_text("old\n")
            with self.assertRaisesRegex(CodegenError, "generated files are stale"):
                write_or_check({output: "new\n"}, check=True)


if __name__ == "__main__":
    unittest.main()
