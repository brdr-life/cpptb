import datetime
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock


REPO = Path(__file__).resolve().parents[3]
RUNNER_PATH = REPO / "benchmarks/peripheral_suite/tools/run_runtime_ab.py"
SPEC = importlib.util.spec_from_file_location("peripheral_runtime_ab", RUNNER_PATH)
RUNTIME_AB = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNTIME_AB)


class RuntimeABTests(unittest.TestCase):
    def setUp(self):
        self._sample_environment = mock.patch.object(
            RUNTIME_AB.benchmark,
            "sample_environment",
            return_value={
                "load_average_1m": 1.0,
                "load_average_5m": 1.0,
                "load_average_15m": 1.0,
                "cpu_count": 8,
            },
        )
        self._pair_boundary = mock.patch.object(
            RUNTIME_AB.benchmark,
            "probe_pair_boundary",
            return_value={
                "power": {"available": True, "source": "ac"},
                "thermal": {"available": True, "status": "nominal"},
            },
        )
        self._sample_environment.start()
        self._pair_boundary.start()
        self.addCleanup(self._sample_environment.stop)
        self.addCleanup(self._pair_boundary.stop)

    def test_real_probe_schema_reaches_shared_validity_helper(self):
        sample_environment = {
            "load_average_1m": 20.0,
            "load_average_5m": 1.0,
            "load_average_15m": 0.5,
            "cpu_count": 8,
        }
        boundary = {
            "power": {"available": True, "source": "ac"},
            "thermal": {"available": True, "status": "nominal"},
        }
        with (
            mock.patch.object(
                RUNTIME_AB.benchmark,
                "sample_environment",
                return_value=sample_environment,
            ),
            mock.patch.object(
                RUNTIME_AB.benchmark,
                "probe_pair_boundary",
                return_value=boundary,
            ),
        ):
            probe = RUNTIME_AB._pair_boundary_probe(1, "measured")
            validity = RUNTIME_AB._environment_validity([probe])

        self.assertEqual(validity["validity"], "invalid")
        self.assertIn("load average", validity["reasons"][0])

    @staticmethod
    def runner(ratio_for_run, mutate=None):
        calls = []

        def sample(run, iters, label):
            calls.append((run, label, iters))
            ratio = 1.0 if run == 0 else ratio_for_run(run)
            result = {
                "run": run,
                "process_wall_ms": 100.0 if label == "old" else 100.0 * ratio,
                "internal_wall_ms": 90.0,
                "checks": 20,
                "sim_cycles": 10,
                "failures": 0,
                "iterations": iters,
                "result_identity": RUNTIME_AB.RESULT_IDENTITY,
                "command": [label, str(iters)],
            }
            if mutate:
                mutate(result, run, label)
            return result

        return sample, calls

    @staticmethod
    def hashes():
        return {"old": "a" * 64, "new": "b" * 64}

    @staticmethod
    def aa_artifact(now, **overrides):
        artifact = {
            "status": "passed",
            "binary_sha256": "b" * 64,
            "measured_pairs": 20,
            "metadata": {
                "timestamp_utc": now.isoformat(),
                "git": {"commit": "commit-a"},
                "config": {"iterations": 10_000},
            },
        }
        for key, value in overrides.items():
            if key == "iterations":
                artifact["metadata"]["config"]["iterations"] = value
            elif key == "timestamp_utc":
                artifact["metadata"]["timestamp_utc"] = value
            else:
                artifact[key] = value
        return artifact

    @staticmethod
    def write_artifact(path, artifact):
        path.write_text(json.dumps(artifact), encoding="utf-8")

    def test_exact_balanced_adjacent_ordering(self):
        sample, calls = self.runner(lambda run: 1.0)
        result = RUNTIME_AB.run_runtime_comparison(
            10_000, sample_runner=sample, binary_hashes=self.hashes()
        )

        self.assertEqual(result["status"], "no_material_regression")
        self.assertEqual(result["measured_pairs"], 16)
        self.assertEqual(len(calls), 34)
        self.assertEqual(
            calls[:6],
            [
                (0, "old", 10_000),
                (0, "new", 10_000),
                (1, "old", 10_000),
                (1, "new", 10_000),
                (2, "new", 10_000),
                (2, "old", 10_000),
            ],
        )
        old_samples = result["old"]["samples"]
        self.assertEqual(
            sum(item["pair_order"] == ["old", "new"] for item in old_samples),
            8,
        )
        self.assertEqual(
            sum(item["pair_order"] == ["new", "old"] for item in old_samples),
            8,
        )

    def test_valid_aa_preflight_records_artifact_evidence(self):
        now = datetime.datetime(2026, 7, 11, 17, 0, tzinfo=datetime.timezone.utc)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "aa.json"
            self.write_artifact(path, self.aa_artifact(now))
            preflight = RUNTIME_AB.validate_aa_preflight(
                path, 10_000, "b" * 64, now=now, current_git_commit="commit-a"
            )

        self.assertEqual(preflight["artifact_path"], str(path))
        self.assertEqual(preflight["artifact_status"], "passed")
        self.assertEqual(preflight["artifact_timestamp_utc"], now.isoformat())
        self.assertEqual(preflight["artifact_binary_sha256"], "b" * 64)
        self.assertEqual(preflight["artifact_iterations"], 10_000)
        self.assertEqual(preflight["artifact_measured_pairs"], 20)
        self.assertEqual(len(preflight["artifact_sha256"]), 64)
        self.assertEqual(preflight["artifact_age_seconds"], 0.0)
        self.assertEqual(preflight["artifact_git_commit"], "commit-a")

    def test_aa_preflight_rejects_git_commit_mismatch(self):
        now = datetime.datetime(2026, 7, 11, 17, 0, tzinfo=datetime.timezone.utc)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "aa.json"
            self.write_artifact(path, self.aa_artifact(now))
            with self.assertRaisesRegex(
                RUNTIME_AB.AAPreflightError, "current start commit"
            ):
                RUNTIME_AB.validate_aa_preflight(
                    path,
                    10_000,
                    "b" * 64,
                    now=now,
                    current_git_commit="commit-b",
                )

    def test_exact_thirty_minute_aa_age_is_accepted(self):
        now = datetime.datetime(2026, 7, 11, 17, 0, tzinfo=datetime.timezone.utc)
        timestamp = now - datetime.timedelta(minutes=30)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "aa.json"
            self.write_artifact(path, self.aa_artifact(timestamp))
            preflight = RUNTIME_AB.validate_aa_preflight(
                path, 10_000, "b" * 64, now=now
            )
        self.assertEqual(preflight["artifact_age_seconds"], 1800.0)

    def test_aa_preflight_rejects_mismatch_stale_and_malformed_cases(self):
        now = datetime.datetime(2026, 7, 11, 17, 0, tzinfo=datetime.timezone.utc)
        cases = {
            "status": (self.aa_artifact(now, status="failed"), "status"),
            "binary": (
                self.aa_artifact(now, binary_sha256="c" * 64),
                "binary SHA256",
            ),
            "iterations": (self.aa_artifact(now, iterations=9_999), "iterations"),
            "pairs": (self.aa_artifact(now, measured_pairs=19), "measured_pairs"),
            "timestamp": (
                self.aa_artifact(now, timestamp_utc="not-a-timestamp"),
                "timestamp is malformed",
            ),
            "naive timestamp": (
                self.aa_artifact(now, timestamp_utc="2026-07-11T17:00:00"),
                "UTC offset",
            ),
            "stale": (
                self.aa_artifact(now - datetime.timedelta(minutes=30, microseconds=1)),
                "older than 30 minutes",
            ),
            "future": (
                self.aa_artifact(now + datetime.timedelta(microseconds=1)),
                "in the future",
            ),
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "aa.json"
            for name, (artifact, message) in cases.items():
                with self.subTest(name=name):
                    self.write_artifact(path, artifact)
                    with self.assertRaisesRegex(RUNTIME_AB.AAPreflightError, message):
                        RUNTIME_AB.validate_aa_preflight(
                            path, 10_000, "b" * 64, now=now
                        )

    def test_aa_preflight_rejects_missing_and_malformed_json(self):
        now = datetime.datetime(2026, 7, 11, 17, 0, tzinfo=datetime.timezone.utc)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "aa.json"
            with self.assertRaisesRegex(RUNTIME_AB.AAPreflightError, "cannot read"):
                RUNTIME_AB.validate_aa_preflight(
                    path, 10_000, "b" * 64, now=now
                )
            path.write_text("{broken", encoding="utf-8")
            with self.assertRaisesRegex(RUNTIME_AB.AAPreflightError, "malformed JSON"):
                RUNTIME_AB.validate_aa_preflight(
                    path, 10_000, "b" * 64, now=now
                )

    def test_cli_aa_artifact_default_and_override(self):
        self.assertEqual(
            RUNTIME_AB._parse_args([]).aa_artifact,
            RUNTIME_AB.DEFAULT_AA_ARTIFACT,
        )
        self.assertEqual(
            RUNTIME_AB._parse_args(["--aa-artifact", "custom-aa.json"]).aa_artifact,
            Path("custom-aa.json"),
        )

    def test_main_persists_preflight_refusal_without_sampling(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            old_binary = directory / "old"
            new_binary = directory / "new"
            old_binary.touch()
            new_binary.touch()
            aa_path = directory / "aa.json"
            aa_path.write_text("{broken", encoding="utf-8")
            result_path = directory / "result.json"
            markdown_path = directory / "result.md"
            journal_path = directory / "result.jsonl"
            argv = [
                "--skip-build",
                "--old-binary", str(old_binary),
                "--new-binary", str(new_binary),
                "--aa-artifact", str(aa_path),
                "--json", str(result_path),
                "--markdown", str(markdown_path),
                "--journal", str(journal_path),
                "--iters", "10000",
            ]
            evidence = {"build_provenance": {"binary_sha256": "a" * 64}}
            with (
                mock.patch.object(
                    RUNTIME_AB,
                    "_capture_git_state",
                    return_value={"commit": "commit-a", "dirty": False},
                ),
                mock.patch.object(RUNTIME_AB, "load_source_evidence", return_value=evidence),
                mock.patch.object(
                    RUNTIME_AB,
                    "_sha256",
                    side_effect=lambda path: "a" * 64 if path == old_binary else "b" * 64,
                ),
                mock.patch.object(RUNTIME_AB, "run_runtime_comparison") as comparison,
                mock.patch.object(
                    RUNTIME_AB.benchmark,
                    "collect_metadata",
                    side_effect=lambda config, **kwargs: {
                        "config": config,
                        "git": kwargs["git_state"],
                    },
                ),
                mock.patch.object(RUNTIME_AB, "persist_result") as persist,
            ):
                exit_code = RUNTIME_AB.main(argv)

        self.assertEqual(exit_code, 1)
        comparison.assert_not_called()
        persisted = persist.call_args.args[0]
        self.assertEqual(persisted["status"], "invalid_environment")
        self.assertEqual(persisted["measured_pairs"], 0)
        self.assertEqual(persisted["aa_preflight"]["artifact_path"], str(aa_path))
        self.assertIsNotNone(persisted["aa_preflight"]["artifact_sha256"])
        self.assertEqual(
            persisted["metadata"]["config"]["aa_preflight"],
            persisted["aa_preflight"],
        )
        self.assertEqual(persisted["metadata"]["git"]["commit"], "commit-a")

    def test_main_samples_after_valid_preflight_and_records_it(self):
        now = datetime.datetime.now(datetime.timezone.utc)
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            old_binary = directory / "old"
            new_binary = directory / "new"
            old_binary.touch()
            new_binary.touch()
            aa_path = directory / "aa.json"
            self.write_artifact(aa_path, self.aa_artifact(now))
            argv = [
                "--skip-build",
                "--old-binary", str(old_binary),
                "--new-binary", str(new_binary),
                "--aa-artifact", str(aa_path),
                "--json", str(directory / "result.json"),
                "--markdown", str(directory / "result.md"),
                "--journal", str(directory / "result.jsonl"),
                "--iters", "10000",
            ]
            evidence = {"build_provenance": {"binary_sha256": "a" * 64}}
            comparison_result = {
                "status": "no_material_regression",
                "measured_pairs": 16,
                "extra_batch_collected": False,
            }
            with (
                mock.patch.object(
                    RUNTIME_AB,
                    "_capture_git_state",
                    return_value={"commit": "commit-a", "dirty": False},
                ),
                mock.patch.object(RUNTIME_AB, "load_source_evidence", return_value=evidence),
                mock.patch.object(
                    RUNTIME_AB,
                    "_sha256",
                    side_effect=lambda path: "a" * 64 if path == old_binary else "b" * 64,
                ),
                mock.patch.object(
                    RUNTIME_AB,
                    "run_runtime_comparison",
                    return_value=comparison_result,
                ) as comparison,
                mock.patch.object(
                    RUNTIME_AB.benchmark,
                    "collect_metadata",
                    side_effect=lambda config, **kwargs: {
                        "config": config,
                        "git": kwargs["git_state"],
                    },
                ),
                mock.patch.object(RUNTIME_AB, "persist_result") as persist,
                mock.patch.object(RUNTIME_AB, "render_markdown", return_value="ok\n"),
            ):
                exit_code = RUNTIME_AB.main(argv)

        self.assertEqual(exit_code, 0)
        comparison.assert_called_once()
        persisted = persist.call_args.args[0]
        self.assertEqual(persisted["aa_preflight"]["artifact_status"], "passed")
        self.assertEqual(persisted["aa_preflight"]["artifact_path"], str(aa_path))
        self.assertEqual(
            persisted["metadata"]["config"]["aa_preflight"],
            persisted["aa_preflight"],
        )
        self.assertEqual(persisted["metadata"]["git"]["commit"], "commit-a")

    def test_workload_check_covers_counts_cycles_failures_and_identity(self):
        mutations = {
            "checks": lambda item: item.update(checks=21),
            "sim_cycles": lambda item: item.update(sim_cycles=11),
            "failures": lambda item: item.update(failures=1),
            "identity": lambda item: item.update(result_identity="OTHER_RESULT"),
        }
        for name, mutation in mutations.items():
            with self.subTest(name=name):
                def mutate(item, run, label):
                    if run == 1 and label == "new":
                        mutation(item)

                sample, _ = self.runner(lambda run: 1.0, mutate=mutate)
                result = RUNTIME_AB.run_runtime_comparison(
                    10_000, sample_runner=sample, binary_hashes=self.hashes()
                )
                self.assertEqual(result["status"], "invalid_environment")

    def test_regression_confirmed(self):
        sample, _ = self.runner(lambda run: 1.10)
        result = RUNTIME_AB.run_runtime_comparison(
            10_000, sample_runner=sample, binary_hashes=self.hashes()
        )
        self.assertEqual(result["status"], "regression_confirmed")
        self.assertGreater(
            result["statistics"]["two_sided_95_median_ci"]["lower"], 1.05
        )
        self.assertGreater(result["statistics"]["old_first_paired_median"], 1.05)
        self.assertGreater(result["statistics"]["new_first_paired_median"], 1.05)

    def test_invalid_environment_prevents_confirmed_regression(self):
        sample, _ = self.runner(lambda run: 1.10)
        invalid = {"status": "invalid", "reasons": ["thermal pressure"]}
        with mock.patch.object(
            RUNTIME_AB.benchmark,
            "environment_validity",
            return_value=invalid,
            create=True,
        ):
            result = RUNTIME_AB.run_runtime_comparison(
                10_000, sample_runner=sample, binary_hashes=self.hashes()
            )
        self.assertEqual(result["status"], "invalid_environment")
        self.assertEqual(result["environment"]["validity"], invalid)
        self.assertEqual(result["measured_pairs"], 16)

    def test_no_material_regression(self):
        sample, _ = self.runner(lambda run: 1.03)
        result = RUNTIME_AB.run_runtime_comparison(
            10_000, sample_runner=sample, binary_hashes=self.hashes()
        )
        self.assertEqual(result["status"], "no_material_regression")
        self.assertLessEqual(
            result["statistics"]["two_sided_95_median_ci"]["upper"], 1.05
        )

    def test_inconclusive_collects_one_extra_batch_and_can_resolve(self):
        sample, calls = self.runner(
            lambda run: 1.0 if run <= 16 and run % 4 in {1, 2} else 1.10
        )
        result = RUNTIME_AB.run_runtime_comparison(
            10_000, sample_runner=sample, binary_hashes=self.hashes()
        )
        self.assertTrue(result["extra_batch_collected"])
        self.assertEqual(result["measured_pairs"], 32)
        self.assertEqual(result["status"], "regression_confirmed")
        self.assertEqual(len(calls), 66)

    def test_final_inconclusive_stops_after_exactly_one_extra_batch(self):
        sample, calls = self.runner(
            lambda run: 1.0 if run % 4 in {1, 2} else 1.10
        )
        result = RUNTIME_AB.run_runtime_comparison(
            10_000, sample_runner=sample, binary_hashes=self.hashes()
        )
        self.assertEqual(result["status"], "inconclusive")
        self.assertTrue(result["extra_batch_collected"])
        self.assertEqual(result["measured_pairs"], 32)
        self.assertEqual(len(calls), 66)

    def test_unhealthy_order_strata_are_invalid(self):
        sample, _ = self.runner(lambda run: 1.0 if run % 2 else 1.10)
        result = RUNTIME_AB.run_runtime_comparison(
            10_000, sample_runner=sample, binary_hashes=self.hashes()
        )
        self.assertEqual(result["status"], "invalid_environment")
        self.assertIn("order strata", result["error"])

    def test_strict_classification_boundaries(self):
        def statistics(lower, upper, strata=1.06, disagreement=0.05):
            return {
                "status": "ok",
                "two_sided_95_median_ci": {"lower": lower, "upper": upper},
                "old_first_paired_median": strata,
                "new_first_paired_median": strata,
                "independent_paired_relative_disagreement": disagreement,
            }

        self.assertEqual(
            RUNTIME_AB.classify_runtime(statistics(1.05, 1.06)), "inconclusive"
        )
        self.assertEqual(
            RUNTIME_AB.classify_runtime(statistics(1.050001, 1.06)),
            "regression_confirmed",
        )
        self.assertEqual(
            RUNTIME_AB.classify_runtime(statistics(1.0, 1.05)),
            "no_material_regression",
        )
        self.assertEqual(
            RUNTIME_AB.classify_runtime(statistics(1.06, 1.07, disagreement=0.05)),
            "regression_confirmed",
        )
        self.assertEqual(
            RUNTIME_AB.classify_runtime(
                statistics(1.06, 1.07, disagreement=0.050001)
            ),
            "invalid_environment",
        )

    def test_journal_raw_samples_hashes_and_atomic_results(self):
        sample, _ = self.runner(lambda run: 1.0)
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            journal = RUNTIME_AB.benchmark.SampleJournal(
                directory / "runtime.jsonl", truncate=True
            )
            result = RUNTIME_AB.run_runtime_comparison(
                10_000,
                sample_runner=sample,
                journal=journal,
                binary_hashes=self.hashes(),
            )
            json_path = directory / "runtime.json"
            markdown_path = directory / "runtime.md"
            RUNTIME_AB.persist_result(result, json_path, markdown_path)

            entries = [
                json.loads(line)
                for line in journal.path.read_text(encoding="utf-8").splitlines()
            ]
            self.assertEqual(len(entries), 34 + 17)
            samples = [entry for entry in entries if entry["event"] == "sample"]
            probes = [
                entry for entry in entries if entry["event"] == "environment_probe"
            ]
            self.assertEqual(len(samples), 34)
            self.assertEqual(len(probes), 17)
            for index, entry in enumerate(entries):
                if entry["event"] == "environment_probe":
                    self.assertEqual(
                        [item["event"] for item in entries[index + 1 : index + 3]],
                        ["sample", "sample"],
                    )
            self.assertTrue(
                all(
                    entry["sample"]["binary_sha256"]
                    == self.hashes()[entry["label"]]
                    for entry in samples
                )
            )
            persisted = json.loads(json_path.read_text(encoding="utf-8"))
            self.assertEqual(persisted["binary_sha256"], self.hashes())
            self.assertIn("Paired new/old median", markdown_path.read_text())

    def test_source_extraction_provenance_hashes_and_line_counts(self):
        provenance = json.loads(RUNTIME_AB.SOURCE_PROVENANCE.read_text())
        evidence = RUNTIME_AB.load_source_evidence()
        self.assertEqual(
            evidence["build_provenance"]["binary_sha256"],
            "48589eac3978a7bc890f59dcb48110072d38080f42f47070e4c6816b18d336b5",
        )
        self.assertGreaterEqual(
            len(evidence["build_provenance"]["compiler"]["build_commands"]), 5
        )
        expected_lines = {
            "cpptb/coro_runtime.hpp": 1230,
            "benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_fixture.cpp": 197,
            "benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_fixture.hpp": 75,
            "benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite.hpp": 4,
            "benchmarks/peripheral_suite/cpp_dpi/testbench.cpp": 240,
        }
        for entry in provenance["files"]:
            path = RUNTIME_AB.SNAPSHOT_ROOT / entry["snapshot_path"]
            content = path.read_bytes()
            self.assertEqual(RUNTIME_AB._sha256(path), entry["sha256"])
            self.assertEqual(len(content.decode("utf-8").split("\n")), entry["line_count"])
            self.assertEqual(entry["line_count"], expected_lines[entry["snapshot_path"]])
            self.assertIsNotNone(entry["tool_use_id"])

        runtime_hash = expected = next(
            item["sha256"]
            for item in provenance["files"]
            if item["snapshot_path"] == "cpptb/coro_runtime.hpp"
        )
        confirmations = provenance["runtime_confirmation_captures"]
        self.assertEqual(len(confirmations), 2)
        self.assertTrue(all(item["sha256"] == runtime_hash for item in confirmations))
        self.assertTrue(all(item["byte_identical_to_accepted"] for item in confirmations))
        self.assertEqual(expected, "518f648bb291f418420316551554b17804d649eab1b55ac0e896132ab6be5cbe")
        self.assertEqual(provenance["excluded_runtime_capture"]["line_count"], 1071)
        self.assertFalse(
            provenance["excluded_runtime_capture"]["byte_identical_to_accepted"]
        )
        self.assertEqual(
            provenance["ambiguity"]["historical_binary"], "unrecoverable"
        )

    def test_make_target_is_static_and_isolated(self):
        makefile = (REPO / "Makefile").read_text(encoding="utf-8")
        start = makefile.index(
            "$(PERIPHERAL_SUITE_RUNTIME_OLD_OBJ_DIR)/Vdpi_peripheral_suite:"
        )
        end = makefile.index("$(AUTHORING_CORE_DPI_CODEGEN_STAMP):", start)
        rule = makefile[start:end]
        self.assertIn(
            "PERIPHERAL_SUITE_RUNTIME_OLD_OBJ_DIR := "
            "$(BUILD_DIR)/diagnostics/runtime_old_obj",
            makefile,
        )
        old_include = "\t\t-CFLAGS -I$(CURDIR)/$(PERIPHERAL_SUITE_RUNTIME_OLD_ROOT) \\\n"
        repo_include = "\t\t-CFLAGS -I$(CURDIR) \\\n"
        self.assertIn(old_include, rule)
        self.assertLess(
            rule.index(old_include),
            rule.index(repo_include),
        )
        self.assertIn("benchmarks/peripheral_suite/cpp_dpi/framework/dpi_transport.cpp", rule)
        self.assertIn("test -f benchmarks/peripheral_suite/cpp_dpi/generated", rule)
        self.assertNotIn("$(PERIPHERAL_SUITE_DPI_GENERATED)", rule)
        self.assertNotIn("cpp_dpi_obj/Vdpi_peripheral_suite", rule)


if __name__ == "__main__":
    unittest.main()
