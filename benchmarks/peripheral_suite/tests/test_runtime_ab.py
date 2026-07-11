import importlib.util
import json
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[3]
RUNNER_PATH = REPO / "benchmarks/peripheral_suite/tools/run_runtime_ab.py"
SPEC = importlib.util.spec_from_file_location("peripheral_runtime_ab", RUNNER_PATH)
RUNTIME_AB = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNTIME_AB)


class RuntimeABTests(unittest.TestCase):
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
            self.assertEqual(len(entries), 34)
            self.assertTrue(all(entry["event"] == "sample" for entry in entries))
            self.assertTrue(
                all(
                    entry["sample"]["binary_sha256"]
                    == self.hashes()[entry["label"]]
                    for entry in entries
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
