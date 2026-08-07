import json
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from cpptb_codegen.design_ir import CodegenError, UnpackedRange
from cpptb_codegen.frontends.slang import SlangFrontend
from cpptb_codegen.generate_dpi_bindings import generate_sources


REPO = Path(__file__).resolve().parents[2]
FIXTURES = REPO / "tests" / "codegen" / "fixtures"
CPP_RESULT = re.compile(
    r"CPP_DPI_INTERFACE_CATALOG_RESULT .*checks=(?P<checks>\d+).*"
    r"failures=(?P<failures>\d+)"
)
SV_RESULT = re.compile(
    r"PURE_SV_INTERFACE_CATALOG_RESULT checks=(?P<checks>\d+) "
    r"failures=(?P<failures>\d+)"
)


class InterfaceFrontendTests(unittest.TestCase):
    def elaborate(self, source: str):
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        root = Path(temp_dir.name)
        (root / "sample.sv").write_text(source)
        design = SlangFrontend().elaborate(
            {
                "module": "sample",
                "sources": ["sample.sv"],
                "frontend_options": {
                    "slang": {"standard": "1800-2023"}
                },
            },
            root,
        )
        return design

    def test_discovers_parameterized_modport_and_array_members(self):
        design = self.elaborate(
            """
interface bus_if #(parameter int WIDTH = 9) (input logic clk);
  logic valid;
  logic [WIDTH-1:0] data [1:0];
  logic ready;
  wire sideband;
  modport target(input clk, valid, data, output ready, inout sideband);
endinterface
module sample(bus_if.target links[2]); endmodule
"""
        )

        self.assertEqual(len(design.interfaces), 1)
        interface = design.interfaces[0]
        self.assertEqual(interface.name, "links")
        self.assertEqual(interface.definition, "bus_if")
        self.assertEqual(interface.modport, "target")
        self.assertEqual(interface.unpacked, (UnpackedRange(0, 1),))
        self.assertEqual(
            [(item.name, item.value) for item in interface.parameters],
            [("WIDTH", "9")],
        )
        self.assertEqual(
            [(item.name, item.direction) for item in interface.constructor_ports],
            [("clk", "input")],
        )

        members = {port.name: port for port in design.ports}
        self.assertEqual(members["links.valid"].cpp_path, ("links", "valid"))
        self.assertEqual(members["links.valid"].direction, "input")
        self.assertEqual(members["links.ready"].direction, "output")
        self.assertEqual(members["links.sideband"].direction, "inout")
        self.assertTrue(members["links.clk"].interface_constructor_port)
        self.assertEqual(
            members["links.data"].unpacked,
            (UnpackedRange(0, 1), UnpackedRange(1, 0)),
        )

    def test_requires_modport_for_unambiguous_directions(self):
        with self.assertRaisesRegex(CodegenError, "does not select a modport"):
            self.elaborate(
                """
interface bus_if; logic value; endinterface
module sample(bus_if bus); endmodule
"""
            )

    def test_rejects_interface_type_parameters_with_clear_diagnostic(self):
        with self.assertRaisesRegex(
            CodegenError, "type and non-integral interface parameters"
        ):
            self.elaborate(
                """
interface typed_if #(parameter type VALUE = logic [3:0]);
  VALUE data;
  modport target(input data);
endinterface
module sample(typed_if.target bus); endmodule
"""
            )

    def test_preserves_interface_and_member_array_dimensions(self):
        design = self.elaborate(
            """
interface grid_if(input logic clk);
  logic [3:0] payload [1:0];
  modport target(input clk, payload);
endinterface
module sample(grid_if.target grids [1:0][2:4]); endmodule
"""
        )

        interface = design.interfaces[0]
        self.assertEqual(
            interface.unpacked,
            (UnpackedRange(1, 0), UnpackedRange(2, 4)),
        )
        payload = next(port for port in design.ports
                       if port.name == "grids.payload")
        self.assertEqual(
            payload.unpacked,
            (
                UnpackedRange(1, 0),
                UnpackedRange(2, 4),
                UnpackedRange(1, 0),
            ),
        )
        self.assertEqual(payload.interface_rank, 2)

    @unittest.skipUnless(
        shutil.which("c++") and shutil.which("verilator"),
        "C++ and Verilator are required",
    )
    def test_generates_multidimensional_interface_and_member_arrays(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            source = root / "sample.sv"
            generated = root / "generated"
            source.write_text(
                """
interface grid_if(input logic clk);
  logic [3:0] payload [1:0];
  logic [3:0] observed [1:0];
  modport target(input clk, payload, output observed);
endinterface
module sample(grid_if.target grids [1:0][2:4]);
  for (genvar outer = 0; outer < 2; outer++) begin
    for (genvar inner = 2; inner <= 4; inner++) begin
      for (genvar word = 0; word < 2; word++) begin
        assign grids[outer][inner].observed[word] =
            grids[outer][inner].payload[word];
      end
    end
  end
endmodule
""",
                encoding="utf-8",
            )
            generate_sources(
                [source],
                top="sample",
                target="sample",
                output_dir=generated,
                base_dir=root,
            )
            wrapper = (generated / "dpi_sample.sv").read_text()
            header = (generated / "sample_dut.hpp").read_text()
            self.assertIn("grid_if grids [1:0] [2:4]", wrapper)
            self.assertIn("grids[1][2].payload[cpptb_", wrapper)
            self.assertIn("operator[](std::int32_t index)", header)

            verilator_root = subprocess.run(
                ["verilator", "--getenv", "VERILATOR_ROOT"],
                check=True,
                text=True,
                capture_output=True,
            ).stdout.strip()
            api_check = root / "interface_array_api.cpp"
            api_check.write_text(
                r'''
#include <utility>
#include "sample_dut.hpp"

template <typename T>
concept HasGet = requires(T value) { value.get(); };

template <typename T>
concept HasSet = requires(T value) {
    value.set(typename T::value_type{});
};

using Dut = cpptb::generated::sample::Dut;
using Cell = decltype(std::declval<Dut>().grids[1][3]);

static_assert(HasSet<decltype(std::declval<Cell>().clk)>);
static_assert(HasSet<decltype(std::declval<Cell>().payload[0])>);
static_assert(HasGet<decltype(std::declval<Cell>().payload[1])>);
static_assert(HasGet<decltype(std::declval<Cell>().observed[0])>);
static_assert(!HasSet<decltype(std::declval<Cell>().observed[1])>);

void documented_usage(Dut dut) {
    dut.grids[1][3].clk.set(0);
    dut.grids[1][3].payload[0].set(0xa);
    dut.grids[1][3].payload[1].set(0x5);
    (void)dut.grids[1][3].observed[0].get();
    (void)dut.grids[0][4].observed[1].get();
}
''',
                encoding="utf-8",
            )
            api_compile = subprocess.run(
                [
                    "c++",
                    "-std=c++20",
                    "-fsyntax-only",
                    f"-I{REPO / 'include'}",
                    f"-I{generated}",
                    f"-I{Path(verilator_root) / 'include' / 'vltstd'}",
                    str(api_check),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            self.assertEqual(api_compile.returncode, 0, api_compile.stdout)

            lint = subprocess.run(
                [
                    "verilator",
                    "--lint-only",
                    "--timing",
                    "--no-sched-zero-delay",
                    "-Wno-TIMESCALEMOD",
                    "-Wno-WIDTH",
                    "-Wno-UNUSEDSIGNAL",
                    "--top-module",
                    "dpi_sample",
                    str(source),
                    str(generated / "dpi_sample.sv"),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            self.assertEqual(lint.returncode, 0, lint.stdout)


@unittest.skipUnless(
    shutil.which("c++") and shutil.which("verilator"),
    "C++ and Verilator are required",
)
class InterfaceRuntimeTests(unittest.TestCase):
    def test_interfaces_arrays_clocks_and_inouts_match_pure_sv(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            generated = temp / "generated"
            clocks = temp / "clocks.json"
            accesses = temp / "accesses.json"
            discovery = temp / "discover"
            source = FIXTURES / "interface_catalog.sv"
            testbench = FIXTURES / "interface_catalog_testbench.cpp"

            generate_sources(
                [source],
                top="interface_catalog",
                target="interface_catalog",
                output_dir=generated,
                base_dir=REPO,
                clock_discovery_source=True,
            )
            header = (generated / "interface_catalog_dut.hpp").read_text()
            wrapper = (generated / "dpi_interface_catalog.sv").read_text()
            self.assertIn("struct LinksDut", header)
            self.assertIn("operator[](std::int32_t index)", header)
            self.assertIn("cpptb::InoutArray<1", header)
            self.assertIn("cpptb::InoutRef<65", header)
            self.assertIn("stream_if #(.WIDTH(8)) links [0:2]", wrapper)
            self.assertIn("assign links[0].sideband", wrapper)

            verilator_root = subprocess.run(
                ["verilator", "--getenv", "VERILATOR_ROOT"],
                check=True,
                text=True,
                capture_output=True,
            ).stdout.strip()

            api_check = temp / "interface_api_check.cpp"
            api_check.write_text(
                r'''
#include <utility>
#include "interface_catalog_dut.hpp"

template <typename T>
concept HasGet = requires(T value) { value.get(); };

template <typename T>
concept HasSet = requires(T value) {
    value.set(typename T::value_type{});
};

template <typename T>
concept HasDrive = requires(T value) {
    value.drive(typename T::value_type{});
    value.high_z();
};

using Dut = cpptb::generated::interface_catalog::Dut;
using Link = decltype(std::declval<Dut>().links[0]);

static_assert(HasGet<decltype(std::declval<Link>().clk)>);
static_assert(HasSet<decltype(std::declval<Link>().clk)>);
static_assert(HasSet<decltype(std::declval<Link>().valid)>);
static_assert(HasGet<decltype(std::declval<Link>().ready)>);
static_assert(!HasSet<decltype(std::declval<Link>().ready)>);
static_assert(HasDrive<decltype(std::declval<Link>().sideband)>);
static_assert(!HasSet<decltype(std::declval<Link>().sideband)>);
static_assert(HasSet<decltype(std::declval<Dut>().gpio_drive)>);
static_assert(HasGet<decltype(std::declval<Dut>().gpio_seen)>);
static_assert(!HasSet<decltype(std::declval<Dut>().gpio_seen)>);
static_assert(HasDrive<decltype(std::declval<Dut>().gpio)>);
static_assert(!HasSet<decltype(std::declval<Dut>().gpio)>);
static_assert(HasDrive<decltype(std::declval<Dut>().wide_gpio)>);
static_assert(!HasSet<decltype(std::declval<Dut>().wide_gpio)>);
''',
                encoding="utf-8",
            )
            api_compile = subprocess.run(
                [
                    "c++",
                    "-std=c++20",
                    "-fsyntax-only",
                    f"-I{REPO / 'include'}",
                    f"-I{generated}",
                    f"-I{Path(verilator_root) / 'include' / 'vltstd'}",
                    str(api_check),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            self.assertEqual(api_compile.returncode, 0, api_compile.stdout)

            subprocess.run(
                [
                    "c++",
                    "-std=c++20",
                    "-O3",
                    "-DCPPTB_HIERARCHY_DISCOVERY",
                    f"-I{REPO / 'include'}",
                    f"-I{generated}",
                    f"-I{Path(verilator_root) / 'include' / 'vltstd'}",
                    str(generated / "discover_interface_catalog_clocks.cpp"),
                    str(testbench),
                    "-o",
                    str(discovery),
                ],
                check=True,
            )
            subprocess.run(
                [str(discovery), str(clocks), str(accesses)], check=True
            )
            clock_data = json.loads(clocks.read_text())["clocks"]
            self.assertEqual(len(clock_data), 2)
            self.assertEqual(
                {item["port"] for item in clock_data}, {"links.clk"}
            )
            self.assertEqual(len({item["signal_id"] for item in clock_data}), 2)

            generate_sources(
                [source],
                top="interface_catalog",
                target="interface_catalog",
                output_dir=generated,
                clock_config=clocks,
                access_config=accesses,
                base_dir=REPO,
            )
            wrapper = (generated / "dpi_interface_catalog.sv").read_text()
            self.assertIn("links[0]", wrapper)
            self.assertIn("links[1]", wrapper)
            self.assertIn("links[2]", wrapper)
            self.assertIn("!= 0", wrapper)
            self.assertIn("!= 1", wrapper)

            common = [
                "verilator",
                "--binary",
                "--timing",
                "--no-sched-zero-delay",
                "-Wno-TIMESCALEMOD",
                "-Wno-WIDTH",
                "-Wno-UNUSEDSIGNAL",
                "-Wno-MULTIDRIVEN",
            ]
            cpp_obj = temp / "cpp_obj"
            subprocess.run(
                [
                    *common,
                    "-CFLAGS",
                    f"-std=c++20 -I{REPO / 'include'} -I{generated}",
                    "--Mdir",
                    str(cpp_obj),
                    "--top-module",
                    "dpi_interface_catalog",
                    str(source),
                    str(generated / "dpi_interface_catalog.sv"),
                    str(generated / "dpi_interface_catalog.cpp"),
                    str(testbench),
                ],
                check=True,
            )
            cpp_run = subprocess.run(
                [str(cpp_obj / "Vdpi_interface_catalog")],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            self.assertEqual(cpp_run.returncode, 0, cpp_run.stdout)
            cpp_result = CPP_RESULT.search(cpp_run.stdout)
            self.assertIsNotNone(cpp_result, cpp_run.stdout)
            self.assertEqual(int(cpp_result.group("failures")), 0)

            sv_obj = temp / "sv_obj"
            subprocess.run(
                [
                    *common,
                    "--Mdir",
                    str(sv_obj),
                    "--top-module",
                    "interface_catalog_sv_tb",
                    str(source),
                    str(FIXTURES / "interface_catalog_sv_tb.sv"),
                ],
                check=True,
            )
            sv_run = subprocess.run(
                [str(sv_obj / "Vinterface_catalog_sv_tb")],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            self.assertEqual(sv_run.returncode, 0, sv_run.stdout)
            sv_result = SV_RESULT.search(sv_run.stdout)
            self.assertIsNotNone(sv_result, sv_run.stdout)
            self.assertEqual(int(sv_result.group("failures")), 0)
            self.assertEqual(
                int(cpp_result.group("checks")),
                int(sv_result.group("checks")),
            )


@unittest.skipUnless(shutil.which("c++"), "C++ is required")
class SimulatorCapabilityTests(unittest.TestCase):
    def test_verilator_rejects_unknown_logic_writes_with_context(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            source = temp / "capability.cpp"
            binary = temp / "capability"
            source.write_text(
                r'''
#include "cpptb/simulator_capabilities.hpp"

int main(int argc, char**) {
    const auto capabilities = cpptb::simulator_capabilities();
    if (capabilities.name != "Verilator" ||
        capabilities.four_state_values ||
        capabilities.four_state_net_resolution) {
        return 1;
    }
    cpptb::require_logic_write_supported(
        cpptb::LogicBits<4>::from_uint(5), "dut.known", "deposit_logic");
    if (argc > 1) {
        cpptb::require_logic_write_supported(
            cpptb::LogicBits<4>::from_string("10xz"),
            "dut.block.signal", "force_logic");
    }
    return 0;
}
''',
                encoding="utf-8",
            )
            subprocess.run(
                [
                    "c++",
                    "-std=c++20",
                    "-DVERILATOR",
                    f"-I{REPO / 'include'}",
                    str(source),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True)
            rejected = subprocess.run(
                [str(binary), "unknown"],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("force_logic on 'dut.block.signal'", rejected.stdout)
            self.assertIn("Verilator is a two-state simulator", rejected.stdout)
            self.assertIn("would silently coerce the value", rejected.stdout)
            self.assertIn("upstream-under-development", rejected.stdout)


if __name__ == "__main__":
    unittest.main()
