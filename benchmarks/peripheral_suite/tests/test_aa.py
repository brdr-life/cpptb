import importlib.util
import unittest
from pathlib import Path
from unittest import mock


AA_PATH = Path(__file__).resolve().parents[1] / "tools" / "run_aa.py"
SPEC = importlib.util.spec_from_file_location("peripheral_aa", AA_PATH)
AA = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AA)


class AATests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
