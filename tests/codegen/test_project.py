import tempfile
import unittest
from pathlib import Path
from unittest import mock

from cpptb_codegen.project import ProjectError, resolve_project


class ProjectResolutionTests(unittest.TestCase):
    def test_flat_project_needs_no_configuration(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            rtl = root / "counter.sv"
            testbench = root / "testbench.cpp"
            rtl.write_text("module counter(input logic clk); endmodule\n")
            testbench.write_text("// testbench\n")

            spec = resolve_project(project=root)

            self.assertEqual(spec.rtl_sources, (rtl.resolve(),))
            self.assertEqual(spec.testbench_sources, (testbench.resolve(),))
            self.assertEqual(spec.top, "counter")
            self.assertEqual(spec.target, "counter")
            self.assertEqual(
                spec.target_build_dir,
                root.resolve() / "build" / "cpptb" / "counter",
            )

    def test_conventional_layout_and_toml_options(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "rtl" / "include").mkdir(parents=True)
            (root / "tests" / "support").mkdir(parents=True)
            package = root / "rtl" / "00_types.sv"
            design = root / "rtl" / "processor.sv"
            package.write_text("package types; endpackage\n")
            design.write_text("module processor; endmodule\n")
            first_test = root / "tests" / "testbench.cpp"
            helper = root / "tests" / "support" / "driver.cpp"
            first_test.write_text("// tests\n")
            helper.write_text("// driver\n")
            (root / "cpptb.toml").write_text(
                """
[design]
sources = ["rtl/00_types.sv", "rtl/processor.sv"]
top = "processor"
include_dirs = ["rtl/include"]
defines = ["SIMULATION=1"]
parameters = { WIDTH = 16 }

[testbench]
sources = ["tests/testbench.cpp", "tests/support/*.cpp"]
include_dirs = ["tests/support"]

[build]
directory = "out"
cxx_flags = ["-O2"]
verilator_args = ["--trace"]
experimental_four_state = true
""".strip()
                + "\n"
            )

            spec = resolve_project(project=root)

            self.assertEqual(spec.rtl_sources, (package.resolve(), design.resolve()))
            self.assertEqual(spec.testbench_sources, (first_test.resolve(), helper.resolve()))
            self.assertEqual(spec.include_dirs, ((root / "rtl/include").resolve(),))
            self.assertEqual(spec.defines, ("SIMULATION=1",))
            self.assertEqual(spec.parameter_map, {"WIDTH": 16})
            self.assertEqual(spec.build_root, (root / "out").resolve())
            self.assertEqual(spec.cxx_flags, ("-O2",))
            self.assertEqual(spec.optimization, "-O2")
            self.assertEqual(spec.verilator_args, ("--trace",))
            self.assertTrue(spec.experimental_four_state)

    def test_optimization_defaults_to_an_optimized_build(self):
        # Verilator optimizes neither the testbench nor the model by default,
        # so an unset value must not mean "no optimization".
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "counter.sv").write_text("module counter; endmodule\n")
            (root / "testbench.cpp").write_text("int main() { return 0; }\n")
            spec = resolve_project(project=root)
            self.assertEqual(spec.optimization, "-O2")

    def test_optimization_accepts_a_debug_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "counter.sv").write_text("module counter; endmodule\n")
            (root / "testbench.cpp").write_text("int main() { return 0; }\n")
            (root / "cpptb.toml").write_text(
                '[build]\noptimization = "-O0"\n', encoding="utf-8"
            )
            spec = resolve_project(project=root)
            self.assertEqual(spec.optimization, "-O0")

    def test_optimization_rejects_arbitrary_flags(self):
        for value in ("-ffast-math", "O2", "-O2 -g", "", "-Ofast"):
            with self.subTest(value=value), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "counter.sv").write_text("module counter; endmodule\n")
                (root / "testbench.cpp").write_text("int main() { return 0; }\n")
                (root / "cpptb.toml").write_text(
                    f'[build]\noptimization = "{value}"\n', encoding="utf-8"
                )
                with self.assertRaises(ProjectError):
                    resolve_project(project=root)

    def test_four_state_configuration_must_use_the_guarded_option(self):
        for argument in (
            "--fourstate",
            "-fourstate",
            "--no-fourstate",
            "-no-fourstate",
        ):
            with self.subTest(argument=argument):
                with tempfile.TemporaryDirectory() as temp_dir:
                    root = Path(temp_dir)
                    (root / "design.sv").write_text(
                        "module design; endmodule\n"
                    )
                    (root / "testbench.cpp").write_text("// testbench\n")
                    (root / "cpptb.toml").write_text(
                        f'[build]\nverilator_args = ["{argument}"]\n'
                    )

                    with self.assertRaisesRegex(
                        ProjectError, "build.experimental_four_state"
                    ):
                        resolve_project(project=root, top="design")

    def test_four_state_configuration_must_be_boolean(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "design.sv").write_text("module design; endmodule\n")
            (root / "testbench.cpp").write_text("// testbench\n")
            (root / "cpptb.toml").write_text(
                '[build]\nexperimental_four_state = "yes"\n'
            )

            with self.assertRaisesRegex(
                ProjectError, "experimental_four_state must be a boolean"
            ):
                resolve_project(project=root, top="design")

    def test_cli_four_state_override_enables_the_guard(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "design.sv").write_text("module design; endmodule\n")
            (root / "testbench.cpp").write_text("// testbench\n")

            spec = resolve_project(
                project=root,
                top="design",
                experimental_four_state=True,
            )

            self.assertTrue(spec.experimental_four_state)

    def test_cli_overrides_configured_sources_and_build_location(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            selected = root / "selected.sv"
            testbench = root / "selected.cpp"
            selected.write_text("module selected; endmodule\n")
            testbench.write_text("// selected test\n")
            (root / "cpptb.toml").write_text(
                '[design]\nsources = ["missing.sv"]\n'
                '[testbench]\nsources = ["missing.cpp"]\n'
            )

            spec = resolve_project(
                project=root,
                sources=[selected],
                testbenches=[testbench],
                top="selected",
                build_dir=Path("artifacts"),
            )

            self.assertEqual(spec.rtl_sources, (selected.resolve(),))
            self.assertEqual(spec.testbench_sources, (testbench.resolve(),))
            self.assertEqual(spec.build_root, (root / "artifacts").resolve())

    def test_missing_files_report_the_available_conventions(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            with self.assertRaisesRegex(ProjectError, "no RTL sources found"):
                resolve_project(project=root)

            (root / "design.sv").write_text("module design; endmodule\n")
            with self.assertRaisesRegex(ProjectError, "no C\\+\\+ testbench found"):
                resolve_project(project=root)

    def test_inferred_top_is_cached_by_project_content(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "design.sv").write_text("module design; endmodule\n")
            (root / "testbench.cpp").write_text("// testbench\n")
            with mock.patch(
                "cpptb_codegen.project.infer_top_module", return_value="design"
            ) as infer:
                first = resolve_project(project=root)
                second = resolve_project(project=root)
                refreshed = resolve_project(project=root, refresh_top=True)

            self.assertEqual(first.top, "design")
            self.assertEqual(second.top, "design")
            self.assertEqual(refreshed.top, "design")
            self.assertEqual(infer.call_count, 2)
            self.assertTrue(
                (root / "build" / "cpptb" / "project-cache.json").is_file()
            )


if __name__ == "__main__":
    unittest.main()
