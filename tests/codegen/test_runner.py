from __future__ import annotations

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from cpptb_codegen import runner


class RunnerTest(unittest.TestCase):
    def test_discover_tests_parses_catalog_lines(self) -> None:
        invocation = runner.Invocation(
            returncode=0,
            stdout=(
                "simulator banner\n"
                "CPPTB_TEST reset_defaults\n"
                "CPPTB_TEST counter_wraps\n"
                "summary\n"
            ),
            stderr="",
        )
        with mock.patch.object(runner, "_run_command", return_value=invocation):
            self.assertEqual(
                runner.discover_tests(["sim"], None),
                ["reset_defaults", "counter_wraps"],
            )

    def test_discover_tests_rejects_duplicates(self) -> None:
        invocation = runner.Invocation(
            returncode=0,
            stdout="CPPTB_TEST duplicate\nCPPTB_TEST duplicate\n",
            stderr="",
        )
        with mock.patch.object(runner, "_run_command", return_value=invocation):
            with self.assertRaisesRegex(runner.RunnerError, "duplicate"):
                runner.discover_tests(["sim"], None)

    def test_run_tests_writes_logs_and_aggregates_results(self) -> None:
        def fake_run(command, environment, timeout):
            test_name = environment["CPPTB_TEST"]
            result = {
                "schema_version": 2,
                "test_name": test_name,
                "status": "passed",
                "checks": 4,
                "failures": 0,
                "warnings": 0,
                "simulation_time_fs": 1000,
                "wall_time_ns": 2000,
                "failure_records": [],
            }
            Path(environment["CPPTB_RESULT_FILE"]).write_text(
                json.dumps(result), encoding="utf-8"
            )
            return runner.Invocation(0, f"ran {test_name}\n", "")

        with tempfile.TemporaryDirectory() as directory:
            result_dir = Path(directory)
            with mock.patch.object(runner, "_run_command", side_effect=fake_run):
                status = runner.run_tests(
                    ["sim"], ["reset_defaults", "counter_wraps"], result_dir, 1.0
                )
            self.assertEqual(status, 0)
            self.assertTrue((result_dir / "reset_defaults.json").is_file())
            self.assertEqual(
                (result_dir / "counter_wraps.log").read_text(encoding="utf-8"),
                "ran counter_wraps\n",
            )

    def test_read_result_accepts_legacy_and_lifecycle_statuses(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "result.json"
            for schema_version, status in (
                (1, "passed"),
                (2, "skipped"),
                (2, "expected_failure"),
                (2, "unexpected_pass"),
                (2, "timed_out"),
            ):
                with self.subTest(schema_version=schema_version, status=status):
                    path.write_text(
                        json.dumps(
                            {
                                "schema_version": schema_version,
                                "test_name": "case",
                                "status": status,
                            }
                        ),
                        encoding="utf-8",
                    )
                    self.assertEqual(
                        runner._read_result(path, "case")["status"], status
                    )

    def test_run_tests_classifies_lifecycle_outcomes(self) -> None:
        statuses = {
            "skip": "skipped",
            "xfail": "expected_failure",
            "xpass": "unexpected_pass",
            "timeout": "timed_out",
        }

        def fake_run(command, environment, timeout):
            test_name = environment["CPPTB_TEST"]
            status = statuses[test_name]
            result = {
                "schema_version": 2,
                "test_name": test_name,
                "status": status,
                "checks": 1,
                "wall_time_ns": 0,
            }
            Path(environment["CPPTB_RESULT_FILE"]).write_text(
                json.dumps(result), encoding="utf-8"
            )
            return runner.Invocation(
                0 if status in runner.SUCCESSFUL_STATUSES else 1, "", ""
            )

        with tempfile.TemporaryDirectory() as directory:
            output = io.StringIO()
            with (
                mock.patch.object(runner, "_run_command", side_effect=fake_run),
                contextlib.redirect_stdout(output),
            ):
                status = runner.run_tests(
                    ["sim"], list(statuses), Path(directory), 1.0
                )
        self.assertEqual(status, 1)
        self.assertIn("SKIP  skip", output.getvalue())
        self.assertIn("XFAIL xfail", output.getvalue())
        self.assertIn("XPASS xpass", output.getvalue())
        self.assertIn("TIMEOUT timeout", output.getvalue())

    def test_main_requires_command_separator(self) -> None:
        self.assertEqual(runner.main(["list", "sim"]), 2)

    def test_main_all_uses_public_discovery_and_execution(self) -> None:
        with (
            mock.patch.object(
                runner, "discover_tests", return_value=["first", "second"]
            ) as discover,
            mock.patch.object(runner, "run_tests", return_value=0) as run,
        ):
            self.assertEqual(runner.main(["run", "--all", "--", "sim"]), 0)
        discover.assert_called_once_with(["sim"], None)
        self.assertEqual(run.call_args.args[1], ["first", "second"])


if __name__ == "__main__":
    unittest.main()
