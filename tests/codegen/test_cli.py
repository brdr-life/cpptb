import contextlib
import io
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from cpptb_codegen.cli import _parser, main


class PublicCliTests(unittest.TestCase):
    def test_help_describes_commands_and_timeout_units(self):
        root_help = _parser().format_help()
        self.assertIn("build", root_help)
        self.assertIn("list", root_help)
        self.assertIn("test", root_help)

        output = io.StringIO()
        with contextlib.redirect_stdout(output), self.assertRaises(SystemExit):
            _parser().parse_args(["test", "--help"])
        test_help = output.getvalue()
        self.assertIn("wall-time limit in seconds for each test process", test_help)
        self.assertIn("result directory", test_help)
        self.assertIn("experimental four-state mode", test_help)
        self.assertIn("currently upstream-blocked", test_help)

    def test_test_without_names_runs_the_complete_catalog(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            binary = root / "Vdpi_counter"
            spec = SimpleNamespace(result_dir=root / "results", wave="")
            with mock.patch(
                "cpptb_codegen.cli._resolve", return_value=spec
            ), mock.patch(
                "cpptb_codegen.cli._build", return_value=binary
            ), mock.patch(
                "cpptb_codegen.cli.discover_tests",
                return_value=["reset_defaults", "counts"],
            ), mock.patch(
                "cpptb_codegen.cli.run_tests", return_value=0
            ) as run:
                result = main(["test"])

            self.assertEqual(result, 0)
            self.assertEqual(
                run.call_args.args,
                ([str(binary)], ["reset_defaults", "counts"], root / "results", None),
            )

    def test_unknown_test_reports_the_compiled_catalog(self):
        spec = SimpleNamespace(result_dir=Path("results"), wave="")
        errors = io.StringIO()
        with mock.patch(
            "cpptb_codegen.cli._resolve", return_value=spec
        ), mock.patch(
            "cpptb_codegen.cli._build", return_value=Path("simulator")
        ), mock.patch(
            "cpptb_codegen.cli.discover_tests", return_value=["known_test"]
        ), contextlib.redirect_stderr(errors):
            result = main(["test", "missing_test"])

        self.assertEqual(result, 2)
        self.assertIn("unknown test 'missing_test'", errors.getvalue())
        self.assertIn("available tests: known_test", errors.getvalue())

    def test_build_does_not_start_the_simulator(self):
        spec = object()
        with mock.patch(
            "cpptb_codegen.cli._resolve", return_value=spec
        ), mock.patch(
            "cpptb_codegen.cli._build", return_value=Path("simulator")
        ), mock.patch("cpptb_codegen.cli.discover_tests") as discover:
            result = main(["build"])

        self.assertEqual(result, 0)
        discover.assert_not_called()


if __name__ == "__main__":
    unittest.main()
