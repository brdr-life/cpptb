import json
import re
import shutil
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path

from cpptb_codegen.generate_dpi_bindings import generate_sources, main


REPO = Path(__file__).resolve().parents[2]
FIXTURES = REPO / "tests" / "codegen" / "fixtures"
CPP_RESULT = re.compile(
    r"CPP_DPI_HIERARCHY_CATALOG_RESULT .*checks=(?P<checks>\d+).*"
    r"failures=(?P<failures>\d+)"
)
SV_RESULT = re.compile(
    r"PURE_SV_HIERARCHY_CATALOG_RESULT checks=(?P<checks>\d+) "
    r"failures=(?P<failures>\d+)"
)


@unittest.skipUnless(shutil.which("c++") and shutil.which("verilator"),
                     "C++ and Verilator are required")
class HierarchyGenerationTests(unittest.TestCase):
    def test_hierarchy_can_be_inspected_and_snapshot_checked(self):
        source = FIXTURES / "hierarchy_catalog.sv"
        output = StringIO()
        with redirect_stdout(output):
            result = main(
                [
                    str(source),
                    "--top",
                    "hierarchy_catalog",
                    "--inspect-hierarchy",
                ]
            )
        self.assertEqual(result, 0)
        self.assertIn("DUT hierarchy_catalog", output.getvalue())
        self.assertIn("block1.storage: variable logic[8]", output.getvalue())
        self.assertIn("lanes[1].block2.storage", output.getvalue())

        with tempfile.TemporaryDirectory() as temp_dir:
            snapshot = Path(temp_dir) / "hierarchy.json"
            with redirect_stdout(StringIO()):
                self.assertEqual(
                    main(
                        [
                            str(source),
                            "--top",
                            "hierarchy_catalog",
                            "--hierarchy-json",
                            str(snapshot),
                        ]
                    ),
                    0,
                )
            catalog = json.loads(snapshot.read_text())
            self.assertEqual(catalog["module"], "hierarchy_catalog")
            by_path = {entry["path"]: entry for entry in catalog["signals"]}
            self.assertEqual(by_path["block1.memory"]["unpacked"], [
                {"left": 2, "right": 8}
            ])
            self.assertFalse(by_path["wide_value"]["four_state"])
            self.assertTrue(by_path["four_state_value"]["four_state"])

            with redirect_stdout(StringIO()):
                self.assertEqual(
                    main(
                        [
                            str(source),
                            "--top",
                            "hierarchy_catalog",
                            "--check-hierarchy",
                            str(snapshot),
                        ]
                    ),
                    0,
                )
            snapshot.write_text("{}\n")
            with redirect_stderr(StringIO()):
                self.assertEqual(
                    main(
                        [
                            str(source),
                            "--top",
                            "hierarchy_catalog",
                            "--check-hierarchy",
                            str(snapshot),
                        ]
                    ),
                    1,
                )

    def test_compiled_usage_generates_a_pruned_access_plan(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            generated = temp / "generated"
            clocks = temp / "clocks.json"
            accesses = temp / "accesses.json"
            discovery = temp / "discover"
            source = FIXTURES / "hierarchy_catalog.sv"

            generate_sources(
                [source],
                top="hierarchy_catalog",
                target="hierarchy_catalog",
                output_dir=generated,
                base_dir=REPO,
            )
            header = (generated / "hierarchy_catalog_dut.hpp").read_text()
            wrapper = (generated / "dpi_hierarchy_catalog.sv").read_text()
            self.assertIn("HierarchyBlock1Scope block1", header)
            self.assertIn("ScopeElement<1, HierarchyLanes1Scope>", header)
            self.assertIn("struct HierarchyLanesArrayElement", header)
            self.assertIn("HierarchyLanesArrayElement operator[](", header)
            self.assertIn("static constexpr std::int64_t WIDTH = 8", header)
            self.assertNotIn("i_dut.block1.storage", wrapper)
            self.assertNotIn("svdpi.h", header)
            self.assertNotIn("svLogicVecVal", header)
            self.assertNotIn("HierarchyImmediateForceCache", header)

            verilator_root = subprocess.run(
                ["verilator", "--getenv", "VERILATOR_ROOT"],
                check=True,
                text=True,
                capture_output=True,
            ).stdout.strip()
            subprocess.run(
                [
                    "c++",
                    "-std=c++20",
                    "-O3",
                    "-DCPPTB_HIERARCHY_DISCOVERY",
                    f"-I{REPO / 'include'}",
                    f"-I{generated}",
                    f"-I{Path(verilator_root) / 'include' / 'vltstd'}",
                    str(generated / "discover_hierarchy_catalog_clocks.cpp"),
                    str(FIXTURES / "hierarchy_catalog_testbench.cpp"),
                    "-o",
                    str(discovery),
                ],
                check=True,
            )
            subprocess.run(
                [str(discovery), str(clocks), str(accesses)], check=True
            )

            plan = json.loads(accesses.read_text())
            self.assertEqual(plan["port_edges"], [0, 4])
            selected = {
                (entry["path"], entry["operation"])
                for entry in plan["accesses"]
            }
            self.assertIn(("block1.storage", "get"), selected)
            self.assertIn(("block1.memory", "force"), selected)
            self.assertIn(("block1.matrix", "deposit"), selected)
            self.assertIn(("lanes[1].block2.storage", "get"), selected)
            self.assertIn(("lanes[1].block2.storage", "deposit"), selected)
            self.assertIn(("lanes[1].block2.storage", "force"), selected)
            self.assertIn(("lanes[1].block2.storage", "release"), selected)
            self.assertIn(("lanes[1].block2.storage", "get_logic"), selected)
            self.assertIn(
                ("lanes[1].block2.storage", "deposit_logic"), selected
            )
            self.assertIn(
                ("lanes[1].block2.storage", "force_logic"), selected
            )
            self.assertIn(("internal_flag", "any_edge"), selected)
            self.assertIn(("four_state_value", "get_logic"), selected)
            self.assertIn(("four_state_value", "deposit_logic"), selected)
            self.assertIn(("four_state_value", "force_logic"), selected)
            self.assertIn(("wide_value", "get"), selected)
            self.assertIn(("wide_value", "deposit"), selected)
            self.assertIn(("wide_value", "force"), selected)
            self.assertIn(("lanes[0].block2.storage", "get"), selected)

            generate_sources(
                [source],
                top="hierarchy_catalog",
                target="hierarchy_catalog",
                output_dir=generated,
                clock_config=clocks,
                access_config=accesses,
                base_dir=REPO,
            )
            wrapper = (generated / "dpi_hierarchy_catalog.sv").read_text()
            header = (generated / "hierarchy_catalog_dut.hpp").read_text()
            self.assertIn("i_dut.block1.storage", wrapper)
            self.assertIn("i_dut.lanes[1].block2.storage", wrapper)
            self.assertIn("2: i_dut.block1.memory[2] = value;", wrapper)
            self.assertIn("2: release i_dut.block1.memory[2];", wrapper)
            self.assertIn("get_block4", header)
            self.assertIn("deposit_block4", header)
            self.assertIn("get_span", header)
            self.assertIn("deposit_span", header)
            self.assertIn("block read index %0d is out of bounds", wrapper)
            self.assertIn("block deposit index %0d is out of bounds", wrapper)
            self.assertNotIn("force i_dut.block1.memory[index]", wrapper)
            self.assertIn("i_dut.block1.state = state_t'(value);", wrapper)
            self.assertIn(
                "output logic [3:0] value", wrapper
            )
            self.assertIn("i_dut.four_state_value = value;", wrapper)
            self.assertIn("svdpi.h", header)
            self.assertIn("svLogicVecVal", header)
            self.assertIn("HierarchyImmediateForceCache", header)
            self.assertIn("current_callback_epoch()", header)
            self.assertIn("bit [136:0] value", wrapper)
            self.assertIn("observe_hierarchy_signal_0", wrapper)
            self.assertIn("@(event_out);", wrapper)
            self.assertNotIn("@(clk);", wrapper)
            self.assertIn("i_dut.lanes[0].block2.storage", wrapper)

            common_verilator_args = [
                "verilator",
                "--binary",
                "--timing",
                "--no-sched-zero-delay",
                "-Wno-TIMESCALEMOD",
                "-Wno-WIDTH",
                "-Wno-UNUSEDSIGNAL",
                "-Wno-BLKANDNBLK",
                "-Wno-MULTIDRIVEN",
            ]
            cpp_object_dir = temp / "cpp_obj"
            subprocess.run(
                [
                    *common_verilator_args,
                    "-CFLAGS",
                    f"-I{REPO / 'include'} -I{generated}",
                    "--Mdir",
                    str(cpp_object_dir),
                    "--top-module",
                    "dpi_hierarchy_catalog",
                    str(source),
                    str(generated / "dpi_hierarchy_catalog.sv"),
                    str(generated / "dpi_hierarchy_catalog.cpp"),
                    str(FIXTURES / "hierarchy_catalog_testbench.cpp"),
                ],
                check=True,
            )
            cpp_run = subprocess.run(
                [str(cpp_object_dir / "Vdpi_hierarchy_catalog")],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            self.assertEqual(cpp_run.returncode, 0, cpp_run.stdout)
            cpp_result = CPP_RESULT.search(cpp_run.stdout)
            self.assertIsNotNone(cpp_result, cpp_run.stdout)
            self.assertEqual(int(cpp_result.group("failures")), 0)

            sv_object_dir = temp / "sv_obj"
            subprocess.run(
                [
                    *common_verilator_args,
                    "--Mdir",
                    str(sv_object_dir),
                    "--top-module",
                    "hierarchy_catalog_sv_tb",
                    str(source),
                    str(FIXTURES / "hierarchy_catalog_sv_tb.sv"),
                ],
                check=True,
            )
            sv_run = subprocess.run(
                [str(sv_object_dir / "Vhierarchy_catalog_sv_tb")],
                check=False,
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
                "C++ and pure-SV hierarchy fixtures must perform the same checks",
            )

    def test_immediate_force_read_cache_obeys_callback_boundaries(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            generated = temp / "generated"
            accesses = temp / "accesses.json"
            source = FIXTURES / "hierarchy_catalog.sv"
            accesses.write_text(
                json.dumps(
                    {
                        "schema_version": 1,
                        "accesses": [
                            {"path": "block1.storage", "operation": "get"},
                            {"path": "block1.storage", "operation": "force"},
                            {"path": "block1.storage", "operation": "release"},
                        ],
                        "port_edges": [],
                    }
                )
                + "\n"
            )
            generate_sources(
                [source],
                top="hierarchy_catalog",
                target="hierarchy_catalog",
                output_dir=generated,
                access_config=accesses,
                base_dir=REPO,
            )
            header_path = generated / "hierarchy_catalog_dut.hpp"
            header = header_path.read_text()
            namespace = re.search(r"namespace ([A-Za-z0-9_:]+) \{", header).group(1)
            signal_id = re.search(
                r'Signal<HierarchyTransport,\s*(\d+),\s*"block1\.storage"',
                header,
            ).group(1)
            get_name = re.search(
                rf"unsigned int (\w+_hierarchy_{signal_id}_get)\(int index\);",
                header,
            ).group(1)
            force_name = re.search(
                rf"void (\w+_hierarchy_{signal_id}_force)"
                r"\(int index, unsigned int value\);",
                header,
            ).group(1)
            release_name = re.search(
                rf"void (\w+_hierarchy_{signal_id}_release)\(int index\);",
                header,
            ).group(1)

            test_source = temp / "force_cache_test.cpp"
            test_source.write_text(
                f'''#include "{header_path.name}"

namespace {namespace} {{
static unsigned int simulator_value = 0;
static unsigned int exported_get_calls = 0;

extern "C" unsigned int {get_name}(int) {{
    ++exported_get_calls;
    return simulator_value;
}}

extern "C" void {force_name}(int, unsigned int value) {{
    simulator_value = value;
}}

extern "C" void {release_name}(int) {{ simulator_value = 0x56; }}
}}  // namespace {namespace}

int main() {{
    using Transport = {namespace}::HierarchyTransport;
    constexpr std::uint32_t id = {signal_id};
    {namespace}::simulator_value = 0x12;
    if (Transport::get<8>(id, 0) != 0x12 ||
        {namespace}::exported_get_calls != 1) return 1;
    {{
        cpptb::probe::detail::DpiCallbackScope callback;
        Transport::force<8>(id, 0, 0x34);
        if (Transport::get<8>(id, 0) != 0x34 ||
            {namespace}::exported_get_calls != 1) return 2;

        Transport::release(id, 0);
        if (Transport::get<8>(id, 0) != 0x56 ||
            {namespace}::exported_get_calls != 2) return 3;

        Transport::force<8>(id, 0, 0x78);
        if (Transport::get<8>(id, 0) != 0x78 ||
            {namespace}::exported_get_calls != 2) return 4;
    }}
    {{
        cpptb::probe::detail::DpiCallbackScope callback;
        if (Transport::get<8>(id, 0) != 0x78 ||
            {namespace}::exported_get_calls != 3) return 5;
        Transport::release(id, 0);
    }}
    return 0;
}}
'''
            )
            executable = temp / "force_cache_test"
            verilator_root = subprocess.run(
                ["verilator", "--getenv", "VERILATOR_ROOT"],
                check=True,
                text=True,
                capture_output=True,
            ).stdout.strip()
            subprocess.run(
                [
                    "c++",
                    "-std=c++20",
                    "-O3",
                    f"-I{REPO / 'include'}",
                    f"-I{generated}",
                    f"-I{Path(verilator_root) / 'include'}",
                    f"-I{Path(verilator_root) / 'include' / 'vltstd'}",
                    str(test_source),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
