import tempfile
import unittest
from pathlib import Path
from unittest import mock

from cpptb_codegen.build import BuildError, build_project
from cpptb_codegen.project import ProjectSpec
from cpptb_codegen.verilator_capabilities import VerilatorFourStateProbe


class FakeCommandLog:
    def __init__(self, path, verbose):
        self.path = path
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text("")

    def run(self, command, *, cwd, label):
        if label == "testbench discovery":
            Path(command[1]).write_text(
                '{"schema_version": 1, "clocks": []}\n'
            )
            Path(command[2]).write_text(
                '{"schema_version": 1, "accesses": [], "port_edges": []}\n'
            )
        if label == "Verilator build":
            object_dir = Path(command[command.index("--Mdir") + 1])
            top = command[command.index("--top-module") + 1]
            object_dir.mkdir(parents=True, exist_ok=True)
            (object_dir / f"V{top}").write_text("binary\n")


def experimental_four_state_fixture(root):
    rtl = root / "counter.sv"
    testbench = root / "testbench.cpp"
    rtl.write_text("module counter; endmodule\n")
    testbench.write_text("// testbench\n")
    include = root / "framework" / "include"
    (include / "cpptb").mkdir(parents=True)
    (include / "cpptb" / "cpptb.hpp").write_text("#pragma once\n")
    verilator_root = root / "verilator"
    (verilator_root / "include" / "vltstd").mkdir(parents=True)
    spec = ProjectSpec(
        root=root,
        rtl_sources=(rtl,),
        testbench_sources=(testbench,),
        top="counter",
        target="counter",
        build_name="counter",
        build_root=root / "build",
        experimental_four_state=True,
    )
    return spec, include, verilator_root


class BuildPipelineTests(unittest.TestCase):
    def test_experimental_four_state_fails_before_generation(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            spec, include, verilator_root = experimental_four_state_fixture(
                root
            )

            def capture(command, label):
                if "--getenv" in command:
                    return str(verilator_root)
                return "Verilator 5.050"

            unsupported = VerilatorFourStateProbe(
                True, False, False, False, False
            )
            with mock.patch(
                "cpptb_codegen.build.find_framework_include",
                return_value=include,
            ), mock.patch(
                "cpptb_codegen.build._tool_command",
                side_effect=lambda value, label: [value],
            ), mock.patch(
                "cpptb_codegen.build._capture", side_effect=capture
            ), mock.patch(
                "cpptb_codegen.build.probe_verilator_four_state",
                return_value=unsupported,
            ), mock.patch(
                "cpptb_codegen.build.generate_sources"
            ) as generate:
                with self.assertRaisesRegex(
                    BuildError, "SystemVerilog X/Z storage: unavailable"
                ):
                    build_project(spec)

            generate.assert_not_called()

    def test_passing_probe_still_requires_conformance_enablement(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            spec, include, verilator_root = experimental_four_state_fixture(
                root
            )

            def capture(command, label):
                if "--getenv" in command:
                    return str(verilator_root)
                return "Verilator future"

            supported = VerilatorFourStateProbe(
                True, True, True, True, True
            )
            with mock.patch(
                "cpptb_codegen.build.find_framework_include",
                return_value=include,
            ), mock.patch(
                "cpptb_codegen.build._tool_command",
                side_effect=lambda value, label: [value],
            ), mock.patch(
                "cpptb_codegen.build._capture", side_effect=capture
            ), mock.patch(
                "cpptb_codegen.build.probe_verilator_four_state",
                return_value=supported,
            ), mock.patch(
                "cpptb_codegen.build.generate_sources"
            ) as generate:
                with self.assertRaisesRegex(
                    BuildError, "remains disabled until the full four-state"
                ):
                    build_project(spec)

            generate.assert_not_called()

    def test_successful_build_is_content_cached(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            rtl = root / "counter.sv"
            testbench = root / "testbench.cpp"
            rtl.write_text("module counter; endmodule\n")
            testbench.write_text("// testbench\n")
            include = root / "framework" / "include"
            (include / "cpptb").mkdir(parents=True)
            (include / "cpptb" / "cpptb.hpp").write_text("#pragma once\n")
            verilator_root = root / "verilator"
            (verilator_root / "include" / "vltstd").mkdir(parents=True)
            spec = ProjectSpec(
                root=root,
                rtl_sources=(rtl,),
                testbench_sources=(testbench,),
                top="counter",
                target="counter",
                build_name="counter",
                build_root=root / "build",
            )

            def capture(command, label):
                if "--getenv" in command:
                    return str(verilator_root)
                return "tool version 1"

            with mock.patch(
                "cpptb_codegen.build.find_framework_include", return_value=include
            ), mock.patch(
                "cpptb_codegen.build._tool_command",
                side_effect=lambda value, label: [value],
            ), mock.patch(
                "cpptb_codegen.build._capture", side_effect=capture
            ), mock.patch(
                "cpptb_codegen.build._CommandLog", FakeCommandLog
            ), mock.patch(
                "cpptb_codegen.build.generate_sources"
            ) as generate:
                first = build_project(spec)
                second = build_project(spec)
                compared = build_project(spec, compare_frontend="verilator_json")
                rtl.write_text("module counter; logic changed; endmodule\n")
                third = build_project(spec)

            self.assertTrue(first.rebuilt)
            self.assertFalse(second.rebuilt)
            self.assertFalse(compared.rebuilt)
            self.assertTrue(third.rebuilt)
            self.assertEqual(generate.call_count, 5)
            first_generation = generate.call_args_list[0].kwargs
            final_generation = generate.call_args_list[1].kwargs
            self.assertEqual(first_generation["top"], "counter")
            self.assertNotIn("clock_config", first_generation)
            self.assertEqual(
                final_generation["clock_config"], spec.metadata_dir / "clocks.json"
            )
            self.assertEqual(
                generate.call_args_list[2].kwargs["compare_frontend"],
                "verilator_json",
            )
            self.assertTrue(spec.binary.is_file())
            self.assertTrue((spec.target_build_dir / "build-state.json").is_file())


if __name__ == "__main__":
    unittest.main()
