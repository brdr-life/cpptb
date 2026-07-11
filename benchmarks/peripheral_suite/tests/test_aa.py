import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock


AA_PATH = Path(__file__).resolve().parents[1] / "tools" / "run_aa.py"
SPEC = importlib.util.spec_from_file_location("peripheral_aa", AA_PATH)
AA = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AA)


class AATests(unittest.TestCase):
    def setUp(self):
        self._sample_environment = mock.patch.object(
            AA.benchmark,
            "sample_environment",
            return_value={
                "load_average_1m": 1.0,
                "load_average_5m": 1.0,
                "load_average_15m": 1.0,
                "cpu_count": 8,
            },
        )
        self._pair_boundary = mock.patch.object(
            AA.benchmark,
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

    @staticmethod
    def runner(ratio_for_run):
        calls = []

        def sample(run, iters, label):
            calls.append((run, label, iters))
            ratio = 1.0 if run == 0 else ratio_for_run(run)
            return {
                "run": run,
                "process_wall_ms": 100.0 if label == "A" else ratio * 100.0,
                "internal_wall_ms": 90.0,
                "checks": 20,
                "sim_cycles": 10,
                "failures": 0,
            }

        return sample, calls

    def test_balanced_ordering_and_identical_labels(self):
        sample, calls = self.runner(lambda run: 1.0)
        result = AA.run_aa_comparison(10_000, sample_runner=sample)

        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["measured_pairs"], 20)
        self.assertFalse(result["extra_batch_collected"])
        self.assertEqual(calls[:6], [(0, "A", 10_000), (0, "B", 10_000),
                                    (1, "A", 10_000), (1, "B", 10_000),
                                    (2, "B", 10_000), (2, "A", 10_000)])
        self.assertEqual(
            sum(sample["pair_order"] == ["A", "B"] for sample in result["A"]["samples"]),
            10,
        )
        self.assertEqual(
            sum(sample["pair_order"] == ["B", "A"] for sample in result["A"]["samples"]),
            10,
        )
        self.assertEqual({sample["slot"] for sample in result["A"]["samples"]}, {1, 2})

    def test_odd_count_is_rejected(self):
        sample, _ = self.runner(lambda run: 1.0)
        with self.assertRaisesRegex(ValueError, "positive and even"):
            AA.collect_aa_pairs(10_000, 19, sample_runner=sample)

    def test_pass_criteria(self):
        sample, _ = self.runner(lambda run: 1.0)
        result = AA.run_aa_comparison(10_000, sample_runner=sample)
        statistics = result["statistics"]
        self.assertEqual(result["status"], "passed")
        self.assertEqual(statistics["ratio"], 1.0)
        self.assertEqual(statistics["a_first_paired_median"], 1.0)
        self.assertEqual(statistics["b_first_paired_median"], 1.0)
        self.assertEqual(statistics["second_slot_over_first_slot_median"], 1.0)
        self.assertEqual(statistics["half_split_drift"], 0.0)
        self.assertLessEqual(statistics["two_sided_95_median_ci"]["lower"], 1.0)
        self.assertGreaterEqual(statistics["two_sided_95_median_ci"]["upper"], 1.0)

    def test_fail_when_ci_excludes_one(self):
        sample, calls = self.runner(lambda run: 1.06)
        result = AA.run_aa_comparison(10_000, sample_runner=sample)
        self.assertEqual(result["status"], "failed")
        self.assertEqual(result["measured_pairs"], 20)
        self.assertEqual(len(calls), 42)

    def test_fail_when_one_stratum_is_outside_outer_band(self):
        sample, _ = self.runner(lambda run: 1.06 if run % 2 else 1.0)
        result = AA.run_aa_comparison(10_000, sample_runner=sample)
        self.assertEqual(result["status"], "failed")
        self.assertGreater(result["statistics"]["a_first_paired_median"], 1.05)

    def test_fail_when_stratum_gap_exceeds_five_percent(self):
        sample, _ = self.runner(lambda run: 1.05 if run % 2 else 0.998)
        result = AA.run_aa_comparison(10_000, sample_runner=sample)
        self.assertEqual(result["status"], "failed")
        self.assertGreater(result["statistics"]["order_stratum_gap"], 0.05)

    def test_slot_effect_five_percent_boundary_is_inclusive(self):
        statistics = self.passing_statistics()
        statistics["second_slot_over_first_slot_median"] = 1.05
        self.assertEqual(AA.classify_aa(statistics), "passed")

        statistics["second_slot_over_first_slot_median"] = 1.050001
        self.assertEqual(AA.classify_aa(statistics), "failed")

        statistics["second_slot_over_first_slot_median"] = 0.95
        self.assertEqual(AA.classify_aa(statistics), "passed")

        statistics["second_slot_over_first_slot_median"] = 0.949999
        self.assertEqual(AA.classify_aa(statistics), "failed")

    def test_half_split_drift_five_percent_boundary_is_inclusive(self):
        statistics = self.passing_statistics()
        statistics["half_split_drift"] = 0.05
        self.assertEqual(AA.classify_aa(statistics), "passed")

        statistics["half_split_drift"] = 0.050001
        self.assertEqual(AA.classify_aa(statistics), "failed")

    def test_inconclusive_collects_exactly_one_extra_batch_then_passes(self):
        sample, calls = self.runner(lambda run: 1.0 if run <= 9 or run > 20 else 1.04)
        result = AA.run_aa_comparison(10_000, sample_runner=sample)
        self.assertEqual(result["status"], "passed")
        self.assertTrue(result["extra_batch_collected"])
        self.assertEqual(result["measured_pairs"], 40)
        self.assertEqual(len(calls), 82)

    def test_final_inconclusive_stops_after_40(self):
        sample, calls = self.runner(
            lambda run: 1.0 if (run - 1) % 20 < 9 else 1.04
        )
        result = AA.run_aa_comparison(10_000, sample_runner=sample)
        self.assertEqual(result["status"], "inconclusive")
        self.assertEqual(result["measured_pairs"], 40)
        self.assertEqual(len(calls), 82)

    def test_hash_is_attached_to_result_and_every_sample(self):
        sample, _ = self.runner(lambda run: 1.0)
        digest = "d" * 64
        with mock.patch.object(AA.benchmark, "binary_sha256", return_value=digest):
            result = AA.run_aa_comparison(10_000, sample_runner=sample)
        self.assertEqual(result["binary_sha256"], digest)
        self.assertTrue(
            all(item["binary_sha256"] == digest for item in result["A"]["samples"])
        )
        self.assertTrue(
            all(item["binary_sha256"] == digest for item in result["B"]["samples"])
        )

    def test_environment_probes_are_journaled_only_before_pairs(self):
        sample, _ = self.runner(lambda run: 1.0)
        with tempfile.TemporaryDirectory() as directory:
            journal = AA.benchmark.SampleJournal(
                Path(directory) / "aa.jsonl", truncate=True
            )
            result = AA.run_aa_comparison(
                10_000,
                sample_runner=sample,
                journal=journal,
                probe_runner=lambda run, phase: {"load": run, "phase_seen": phase},
            )
            entries = [
                json.loads(line)
                for line in journal.path.read_text(encoding="utf-8").splitlines()
            ]

        self.assertEqual(len(result["environment"]["probes"]), 21)
        self.assertEqual(len(entries), 21 + 42)
        for index, entry in enumerate(entries):
            if entry["event"] != "environment_probe":
                continue
            first, second = entries[index + 1 : index + 3]
            self.assertEqual([first["event"], second["event"]], ["sample", "sample"])
            self.assertEqual(
                first["sample"]["environment_probe_reference"], entry["reference"]
            )
            self.assertEqual(
                second["sample"]["environment_probe_reference"], entry["reference"]
            )

    def test_real_probe_schema_reaches_shared_validity_helper(self):
        sample_environment = {
            "load_average_1m": 2.0,
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
                AA.benchmark, "sample_environment", return_value=sample_environment
            ),
            mock.patch.object(
                AA.benchmark, "probe_pair_boundary", return_value=boundary
            ),
        ):
            probe = AA._pair_boundary_probe(1, "measured")
            validity = AA._environment_validity([probe])

        self.assertEqual(probe["sample_environment"], sample_environment)
        self.assertEqual(probe["power_thermal"], boundary)
        self.assertEqual(validity["validity"], "valid")

    def test_bad_environment_does_not_change_aa_bands(self):
        sample, _ = self.runner(lambda run: 1.0)
        invalid = {"status": "invalid", "reasons": ["high load"]}
        with mock.patch.object(
            AA.benchmark, "environment_validity", return_value=invalid, create=True
        ):
            result = AA.run_aa_comparison(10_000, sample_runner=sample)
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["environment"]["validity"], invalid)

    def test_main_threads_pre_write_git_snapshot_into_metadata(self):
        start = {"commit": "clean-start", "dirty": False}
        journal = mock.Mock()
        collect_metadata = mock.Mock(
            side_effect=lambda config, **kwargs: {
                "config": config,
                "git": kwargs["git_state"],
            }
        )
        with (
            mock.patch.object(AA, "_capture_git_state", side_effect=[start, start]),
            mock.patch.object(AA.benchmark, "SampleJournal", return_value=journal),
            mock.patch.object(
                AA,
                "run_aa_comparison",
                return_value={"status": "passed"},
            ),
            mock.patch.object(AA.benchmark, "collect_metadata", collect_metadata),
            mock.patch.object(AA.benchmark, "atomic_write_json"),
            mock.patch.object(AA.benchmark, "atomic_write_text"),
            mock.patch.object(AA, "render_markdown", return_value="ok\n"),
        ):
            self.assertEqual(AA.main(["--skip-build"]), 0)

        self.assertEqual(collect_metadata.call_args.kwargs["git_state"], start)

    @staticmethod
    def passing_statistics():
        return {
            "status": "ok",
            "ratio": 1.0,
            "a_first_paired_median": 1.0,
            "b_first_paired_median": 1.0,
            "second_slot_over_first_slot_median": 1.0,
            "half_split_drift": 0.0,
            "order_stratum_gap": 0.0,
            "two_sided_95_median_ci": {"lower": 0.99, "upper": 1.01},
        }


if __name__ == "__main__":
    unittest.main()
