from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from benchmarks import run_regression as regression


def feature(
    name: str,
    *,
    build: list[str] | None = None,
    benchmark: list[str] | None = None,
    adapter: str = "runner",
) -> dict[str, object]:
    entry: dict[str, object] = {
        "id": name,
        "semantic_command": ["semantic", name],
        "adapter": adapter,
    }
    if build is not None:
        entry["build_command"] = build
    if benchmark is not None:
        entry["benchmark_command"] = benchmark
    return entry


class FakeClock:
    def __init__(self) -> None:
        self.now = 0.0
        self.sleeps: list[float] = []

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.sleeps.append(seconds)
        self.now += seconds


class SerialRunner:
    def __init__(self, responses: dict[tuple[str, ...], dict] | None = None) -> None:
        self.responses = responses or {}
        self.calls: list[tuple[str, ...]] = []
        self.active = False
        self.maximum_active = 0

    def __call__(self, command: list[str]) -> dict:
        if self.active:
            raise AssertionError("commands overlapped")
        self.active = True
        self.maximum_active = max(self.maximum_active, 1)
        key = tuple(command)
        self.calls.append(key)
        try:
            return self.responses.get(
                key,
                {"returncode": 0, "stdout": "", "stderr": ""},
            )
        finally:
            self.active = False


class SequenceProbe:
    def __init__(self, values: list[float | None], events: list[str] | None = None) -> None:
        self.values = list(values)
        self.last = values[-1]
        self.events = events

    def __call__(self) -> dict[str, object]:
        if self.events is not None:
            self.events.append("probe")
        value = self.values.pop(0) if self.values else self.last
        return {"normalized_load_1m": value, "source": "injected"}


class SettleTests(unittest.TestCase):
    def test_settle_persists_threshold_poll_timeout_and_all_probes(self) -> None:
        clock = FakeClock()
        result = regression.settle_normalized_load(
            SequenceProbe([0.9, 0.7, 0.4]),
            threshold=0.5,
            poll_seconds=2.0,
            timeout_seconds=10.0,
            sleep_runner=clock.sleep,
            monotonic=clock.monotonic,
        )

        self.assertEqual(result["status"], "settled")
        self.assertEqual(result["threshold"], 0.5)
        self.assertEqual(result["poll_seconds"], 2.0)
        self.assertEqual(result["timeout_seconds"], 10.0)
        self.assertEqual(clock.sleeps, [2.0, 2.0])
        self.assertEqual(
            [probe["normalized_load_1m"] for probe in result["probes"]],
            [0.9, 0.7, 0.4],
        )

    def test_settle_is_bounded_and_reports_timeout_evidence(self) -> None:
        clock = FakeClock()
        result = regression.settle_normalized_load(
            SequenceProbe([0.9]),
            threshold=0.5,
            poll_seconds=2.0,
            timeout_seconds=5.0,
            sleep_runner=clock.sleep,
            monotonic=clock.monotonic,
        )

        self.assertEqual(result["status"], "timed_out")
        self.assertEqual(clock.sleeps, [2.0, 2.0, 1.0])
        self.assertEqual(result["elapsed_seconds"], 5.0)
        self.assertEqual(len(result["probes"]), 4)

    def test_unavailable_load_does_not_hang(self) -> None:
        result = regression.settle_normalized_load(SequenceProbe([None]))
        self.assertEqual(result["status"], "unavailable")
        self.assertEqual(len(result["probes"]), 1)


class MulticlockTests(unittest.TestCase):
    CPP = "CPP_DPI_MULTICLOCK_RESULT iterations=16 checks=64 sim_cycles=81 wall_ms=0.125 failures=0\n"
    SV = "PURE_SV_MULTICLOCK_RESULT iterations=16 checks=64 sim_cycles=81 failures=0\n"

    def test_exact_four_field_match_is_equivalence_only(self) -> None:
        result = regression.compare_multiclock(self.CPP, self.SV)
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["measurement_mode"], "equivalence_only")
        self.assertTrue(result["exact_match"])

    def test_timer_only_markers_use_the_same_exact_equivalence_contract(
        self,
    ) -> None:
        cpp = (
            "CPP_DPI_TIMER_ONLY_RESULT iterations=9 checks=39 sim_cycles=0 "
            "wall_ms=0.01 failures=0\n"
        )
        sv = (
            "PURE_SV_TIMER_ONLY_RESULT iterations=9 checks=39 "
            "sim_cycles=0 failures=0\n"
        )
        result = regression.compare_multiclock(cpp, sv)
        self.assertEqual(result["status"], "passed")
        self.assertTrue(result["exact_match"])

    def test_counter_markers_use_the_same_exact_equivalence_contract(self) -> None:
        cpp = (
            "CPP_DPI_COUNTER_RESULT iterations=8 checks=9 sim_cycles=11 "
            "wall_ms=0.01 failures=0\n"
        )
        sv = (
            "PURE_SV_COUNTER_RESULT iterations=8 checks=9 "
            "sim_cycles=11 failures=0\n"
        )
        result = regression.compare_multiclock(cpp, sv)
        self.assertEqual(result["status"], "passed")
        self.assertTrue(result["exact_match"])

    def test_expanded_examples_use_the_same_exact_equivalence_contract(self) -> None:
        examples = {
            "FIFO_SCOREBOARD": (24, 27, 44),
            "APB_REGFILE": (12, 41, 80),
            "WATCHDOG_TIMEOUT": (8, 20, 44),
        }
        for marker, (iterations, checks, sim_cycles) in examples.items():
            with self.subTest(marker=marker):
                fields = (
                    f"iterations={iterations} checks={checks} "
                    f"sim_cycles={sim_cycles} failures=0"
                )
                cpp = f"CPP_DPI_{marker}_RESULT {fields}\n"
                sv = f"PURE_SV_{marker}_RESULT {fields}\n"
                result = regression.compare_multiclock(cpp, sv)
                self.assertEqual(result["status"], "passed")
                self.assertTrue(result["exact_match"])

    def test_all_four_fields_must_match(self) -> None:
        sv = self.SV.replace("sim_cycles=81", "sim_cycles=82")
        result = regression.compare_multiclock(self.CPP, sv)
        self.assertEqual(result["status"], "failed")
        self.assertEqual(
            result["mismatches"]["sim_cycles"],
            {"cpp_dpi": 81, "pure_sv": 82},
        )

    def test_parser_rejects_missing_extra_reordered_and_duplicate_markers(self) -> None:
        invalid = [
            "CPP_DPI_MULTICLOCK_RESULT iterations=16 checks=64 sim_cycles=81",
            self.CPP.strip() + " extra=1",
            "CPP_DPI_MULTICLOCK_RESULT checks=64 iterations=16 sim_cycles=81 failures=0",
            self.CPP + self.CPP,
        ]
        for output in invalid:
            with self.subTest(output=output):
                with self.assertRaises(ValueError):
                    regression.parse_multiclock_result(output, "cpp_dpi")

    def test_nonzero_failures_fail_even_when_fields_match(self) -> None:
        result = regression.compare_multiclock(
            self.CPP.replace("failures=0", "failures=1"),
            self.SV.replace("failures=0", "failures=1"),
        )
        self.assertEqual(result["status"], "failed")
        self.assertTrue(result["exact_match"])


class RegistryTests(unittest.TestCase):
    def test_selection_is_exact_and_unknown_lists_available_ids(self) -> None:
        entries = [feature("task"), feature("task-timeout")]
        self.assertEqual(regression.feature_id(regression.select_feature(entries, "task")), "task")
        with self.assertRaisesRegex(KeyError, "task-timeout"):
            regression.select_feature(entries, "timeout")

    def test_validation_rejects_duplicate_ids_and_parallel_make(self) -> None:
        with self.assertRaisesRegex(ValueError, "duplicate"):
            regression.validate_registry([feature("event"), feature("event")])
        bad = feature("event", benchmark=["make", "-j8", "event-benchmark"])
        with self.assertRaisesRegex(ValueError, "parallel make"):
            regression.validate_registry([bad])

    def test_semantic_check_runs_only_selected_entry(self) -> None:
        runner = SerialRunner()
        result = regression.run_semantic_check(feature("channel"), command_runner=runner)
        self.assertEqual(result["status"], "passed")
        self.assertEqual(runner.calls, [("semantic", "channel")])

    def test_registry_authoring_selection_passes_one_example(self) -> None:
        from benchmarks import registry

        entry = registry.get_benchmark("task_timeout")
        commands = regression._benchmark_commands(entry)
        self.assertEqual(len(commands), 1)
        command = commands[0][1]
        self.assertEqual(command.count("--example"), 1)
        self.assertEqual(command[command.index("--example") + 1], "task_timeout")
        self.assertNotIn("--kernels", command)
        self.assertIn("--skip-build", command)

    def test_registry_authoring_semantic_check_preserves_selected_example(self) -> None:
        from benchmarks import registry

        runner = SerialRunner()
        result = regression.run_semantic_check(
            registry.get_benchmark("task_timeout"), command_runner=runner
        )

        self.assertEqual(result["status"], "passed")
        semantic = runner.calls[-1]
        self.assertEqual(semantic.count("--example"), 1)
        self.assertEqual(semantic[semantic.index("--example") + 1], "task_timeout")
        self.assertIn("--semantic-only", semantic)

    def test_peripheral_semantic_check_builds_only_dpi_and_sv(self) -> None:
        from benchmarks import registry

        runner = SerialRunner()
        with mock.patch.object(
            regression, "_load_runner_result", return_value={"status": "success"}
        ):
            result = regression.run_semantic_check(
                registry.get_benchmark("peripheral_suite"), command_runner=runner
            )

        self.assertEqual(result["status"], "passed")
        self.assertEqual(
            runner.calls[0],
            ("make", "peripheral-suite-dpi-build", "peripheral-suite-sv-build"),
        )
        self.assertNotIn("peripheral-suite-build", runner.calls[0])
        self.assertIn("--semantic-only", runner.calls[-1])


class RegressionTests(unittest.TestCase):
    def run_in_temp(self, entries: list[object], **kwargs: object) -> dict[str, object]:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        return regression.run_benchmarks(entries, result_dir=Path(temporary.name), **kwargs)

    def test_all_builds_finish_before_serial_measurement_and_settle(self) -> None:
        events: list[str] = []

        class EventRunner(SerialRunner):
            def __call__(self, command: list[str]) -> dict:
                events.append("command:" + " ".join(command))
                return super().__call__(command)

        runner = EventRunner(
            {
                ("bench", "a"): {"returncode": 0, "result": {"status": "passed"}},
                ("bench", "b"): {"returncode": 0, "result": {"status": "passed"}},
            }
        )
        entries = [
            feature("a", build=["build", "a"], benchmark=["bench", "a"]),
            feature("b", build=["build", "b"], benchmark=["bench", "b"]),
        ]
        result = self.run_in_temp(
            entries,
            command_runner=runner,
            probe_runner=SequenceProbe([0.2, 0.2], events),
        )

        self.assertEqual(
            events,
            [
                "command:build a",
                "command:build b",
                "probe",
                "command:bench a",
                "probe",
                "command:bench b",
            ],
        )
        self.assertEqual(runner.maximum_active, 1)
        self.assertTrue(result["serial"])
        self.assertTrue(result["builds_completed_before_measurement"])
        self.assertFalse(result["config"]["samples_normalized"])

    def test_shared_build_runs_once(self) -> None:
        runner = SerialRunner()
        entries = [
            feature("a", build=["build", "all"], benchmark=["bench", "a"]),
            feature("b", build=["build", "all"], benchmark=["bench", "b"]),
        ]
        self.run_in_temp(
            entries,
            command_runner=runner,
            probe_runner=SequenceProbe([0.1]),
        )
        self.assertEqual(runner.calls.count(("build", "all")), 1)

    def test_failure_continues_to_later_entries(self) -> None:
        runner = SerialRunner(
            {
                ("bench", "bad"): {"returncode": 7, "stderr": "failed"},
                ("bench", "good"): {"returncode": 0, "result": {"status": "passed"}},
            }
        )
        result = self.run_in_temp(
            [
                feature("bad", benchmark=["bench", "bad"]),
                feature("good", benchmark=["bench", "good"]),
            ],
            command_runner=runner,
            probe_runner=SequenceProbe([0.1]),
        )
        self.assertEqual(runner.calls, [("bench", "bad"), ("bench", "good")])
        self.assertEqual([entry["status"] for entry in result["entries"]], ["failed", "passed"])
        self.assertEqual(result["status"], "failed")

    def test_runner_exception_is_recorded_and_later_entry_runs(self) -> None:
        class RaisingRunner(SerialRunner):
            def __call__(self, command: list[str]) -> dict:
                if command == ["bench", "bad"]:
                    self.calls.append(tuple(command))
                    raise RuntimeError("injected crash")
                return super().__call__(command)

        runner = RaisingRunner()
        result = self.run_in_temp(
            [
                feature("bad", benchmark=["bench", "bad"]),
                feature("good", benchmark=["bench", "good"]),
            ],
            command_runner=runner,
            probe_runner=SequenceProbe([0.1]),
        )
        self.assertEqual(result["entries"][0]["status"], "failed")
        self.assertIn("injected crash", result["entries"][0]["error"])
        self.assertIn(("bench", "good"), runner.calls)

    def test_timeout_skips_entry_and_continues(self) -> None:
        clock = FakeClock()
        runner = SerialRunner()
        result = self.run_in_temp(
            [
                feature("hot", benchmark=["bench", "hot"]),
                feature("cool", benchmark=["bench", "cool"]),
            ],
            command_runner=runner,
            probe_runner=SequenceProbe([0.9, 0.9, 0.1]),
            sleep_runner=clock.sleep,
            monotonic=clock.monotonic,
            threshold=0.5,
            poll_seconds=1.0,
            timeout_seconds=1.0,
        )
        self.assertEqual(runner.calls, [("bench", "cool")])
        self.assertEqual(result["entries"][0]["status"], "invalid_environment")
        self.assertEqual(result["entries"][0]["settle"]["status"], "timed_out")
        self.assertEqual(result["entries"][1]["status"], "passed")

    def test_unavailable_probe_skips_measurement_as_invalid_environment(self) -> None:
        runner = SerialRunner()
        result = self.run_in_temp(
            [feature("event", benchmark=["bench", "event"])],
            command_runner=runner,
            probe_runner=SequenceProbe([None]),
        )
        self.assertEqual(runner.calls, [])
        self.assertEqual(result["status"], "invalid_environment")
        self.assertEqual(result["entries"][0]["settle"]["status"], "unavailable")

    def test_build_failure_skips_only_owners_and_continues(self) -> None:
        runner = SerialRunner(
            {("build", "bad"): {"returncode": 2, "stderr": "compile failed"}}
        )
        result = self.run_in_temp(
            [
                feature("bad", build=["build", "bad"], benchmark=["bench", "bad"]),
                feature("good", build=["build", "good"], benchmark=["bench", "good"]),
            ],
            command_runner=runner,
            probe_runner=SequenceProbe([0.1]),
        )
        self.assertNotIn(("bench", "bad"), runner.calls)
        self.assertIn(("bench", "good"), runner.calls)
        self.assertEqual(result["entries"][0]["status"], "failed")
        self.assertEqual(result["entries"][1]["status"], "passed")

    def test_status_precedence(self) -> None:
        statuses = ["passed", "passed_inconclusive", "invalid_environment", "failed"]
        for index, expected in enumerate(statuses):
            with self.subTest(expected=expected):
                self.assertEqual(regression.aggregate_status(statuses[: index + 1]), expected)

    def test_nested_authoring_guard_preserves_inconclusive_status(self) -> None:
        payload = {
            "status": "success",
            "kernels": {
                "event": {"guard": {"status": "passed_inconclusive"}}
            },
        }
        self.assertEqual(regression._payload_status(payload), "passed_inconclusive")
        self.assertEqual(
            regression._payload_status({"status": "invalid_environment"}, returncode=1),
            "invalid_environment",
        )

    def test_runner_transport_error_statuses_normalize_to_failed(self) -> None:
        for status in ("command_error", "workload_error"):
            with self.subTest(status=status):
                self.assertEqual(regression.normalize_status(status), "failed")

    def test_nonzero_runner_keeps_explicit_invalid_environment(self) -> None:
        runner = SerialRunner(
            {
                ("bench", "event"): {
                    "returncode": 1,
                    "result": {"status": "invalid_environment"},
                }
            }
        )
        result = self.run_in_temp(
            [feature("event", benchmark=["bench", "event"])],
            command_runner=runner,
            probe_runner=SequenceProbe([0.1]),
        )
        self.assertEqual(result["status"], "invalid_environment")

    def test_multiclock_commands_are_adjacent_and_never_timed(self) -> None:
        entry = feature("multiclock", adapter="equivalence_only")
        entry.pop("benchmark_command", None)
        entry["cpp_command"] = ["run", "cpp"]
        entry["sv_command"] = ["run", "sv"]
        runner = SerialRunner(
            {
                ("run", "cpp"): {"returncode": 0, "stdout": MulticlockTests.CPP},
                ("run", "sv"): {"returncode": 0, "stdout": MulticlockTests.SV},
            }
        )
        result = self.run_in_temp(
            [entry], command_runner=runner, probe_runner=SequenceProbe([0.1])
        )
        measured = result["entries"][0]
        self.assertEqual(runner.calls, [("run", "cpp"), ("run", "sv")])
        self.assertEqual(measured["status"], "passed")
        self.assertEqual(measured["comparison"]["measurement_mode"], "equivalence_only")
        self.assertNotIn("process_wall_ms", json.dumps(measured))

    def test_peripheral_diagnostic_is_not_an_implicit_gate(self) -> None:
        runner = SerialRunner()
        self.run_in_temp(
            [feature("event", benchmark=["bench", "event"])],
            command_runner=runner,
            probe_runner=SequenceProbe([0.1]),
        )
        flattened = " ".join(part for command in runner.calls for part in command)
        self.assertNotIn("peripheral", flattened)

    def test_diagnostic_threshold_failure_is_reported_but_not_aggregated(self) -> None:
        entry = feature("peripheral", benchmark=["bench", "peripheral"])
        entry["gate_policy"] = "diagnostic"
        runner = SerialRunner(
            {
                ("bench", "peripheral"): {
                    "returncode": 1,
                    "stdout": "",
                    "stderr": "",
                }
            }
        )
        with mock.patch.object(
            regression,
            "_load_runner_result",
            return_value={
                "status": "hard_failure",
                "performance_guard": {"status": "hard_failure"},
            },
        ):
            result = self.run_in_temp(
                [entry],
                command_runner=runner,
                probe_runner=SequenceProbe([0.1]),
            )

        measured = result["entries"][0]
        self.assertEqual(measured["status"], "passed")
        self.assertEqual(measured["diagnostic_status"], "failed")
        self.assertEqual(result["status"], "passed")

    def test_diagnostic_missing_fresh_result_is_a_framework_failure(self) -> None:
        entry = feature("peripheral", benchmark=["bench", "peripheral"])
        entry["gate_policy"] = "diagnostic"
        runner = SerialRunner(
            {("bench", "peripheral"): {"returncode": 1, "stderr": "crashed"}}
        )
        with tempfile.TemporaryDirectory() as temporary:
            missing = Path(temporary) / "latest.json"
            with mock.patch.object(
                regression, "_runner_result_path", return_value=missing
            ):
                result = self.run_in_temp(
                    [entry],
                    command_runner=runner,
                    probe_runner=SequenceProbe([0.1]),
                )

        measured = result["entries"][0]
        self.assertEqual(measured["status"], "failed")
        self.assertEqual(measured["diagnostic_status"], "failed")
        self.assertEqual(result["status"], "failed")

    def test_unchanged_runner_result_is_stale(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "latest.json"
            path.write_text('{"status":"success"}')
            previous = regression._result_signature(path)
            entry = feature("control")
            with mock.patch.object(
                regression, "_runner_result_path", return_value=path
            ):
                self.assertIsNone(
                    regression._load_runner_result(
                        entry, previous_signature=previous
                    )
                )
                replacement = path.with_name("replacement.json")
                replacement.write_text('{"status":"passed_inconclusive"}')
                replacement.replace(path)
                self.assertEqual(
                    regression._load_runner_result(
                        entry, previous_signature=previous
                    )["status"],
                    "passed_inconclusive",
                )

    def test_index_surfaces_diagnostic_status(self) -> None:
        markdown = regression._render_index(
            {
                "status": "passed",
                "entries": [
                    {
                        "feature": "peripheral",
                        "adapter": "runner",
                        "settle": {"status": "settled"},
                        "status": "passed",
                        "diagnostic_status": "failed",
                    }
                ],
            }
        )
        self.assertIn("| Diagnostic |", markdown)
        self.assertIn("`failed`", markdown)

    def test_per_entry_and_aggregate_outputs_are_isolated(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            result = regression.run_benchmarks(
                [feature("a", benchmark=["bench", "a"]), feature("b", benchmark=["bench", "b"])],
                command_runner=SerialRunner(),
                probe_runner=SequenceProbe([0.1]),
                result_dir=root,
            )
            regression.persist_index(result, root)
            self.assertTrue((root / "entries/a/latest.json").is_file())
            self.assertTrue((root / "entries/b/latest.json").is_file())
            self.assertTrue((root / "latest.json").is_file())
            self.assertTrue((root / "latest.md").is_file())
            self.assertEqual(json.loads((root / "latest.json").read_text())["status"], "passed")
            self.assertEqual(list(root.rglob("*.tmp")), [])


if __name__ == "__main__":
    unittest.main()
