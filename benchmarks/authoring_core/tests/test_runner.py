import math
import sys
import unittest
from pathlib import Path


BENCH_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BENCH_DIR))

import run_benchmark as runner  # noqa: E402
import workload  # noqa: E402


def sample(mode, pair, process_ms, kernel="control", iterations=1, sequence=None):
    expected = workload.expected_counts(kernel, iterations).fields()
    order = ["cpp_dpi", "pure_sv"] if pair % 2 else ["pure_sv", "cpp_dpi"]
    if sequence is None:
        sequence = (pair - 1) * 2 + order.index(mode)
    return {
        "mode": mode,
        "kernel": kernel,
        **expected,
        "sim_cycles": 7,
        "checksum": workload.expected_checksum(iterations),
        "failures": 0,
        "pair": pair,
        "pair_order": order,
        "sequence_index": sequence,
        "process_wall_ms": process_ms,
    }


def paired_samples(ratios):
    cpp, sv = [], []
    for pair, ratio in enumerate(ratios, 1):
        order = ["cpp_dpi", "pure_sv"] if pair % 2 else ["pure_sv", "cpp_dpi"]
        base = (pair - 1) * 2
        cpp.append(sample("cpp_dpi", pair, ratio, sequence=base + order.index("cpp_dpi")))
        sv.append(sample("pure_sv", pair, 1.0, sequence=base + order.index("pure_sv")))
    return cpp, sv


class StatisticsTests(unittest.TestCase):
    def test_exact_confidence_bounds(self):
        values = list(range(1, 16))
        upper = runner.one_sided_upper_median_bound(values)
        self.assertEqual(upper["order_statistic"], 12)
        self.assertEqual(upper["bound"], 12)
        self.assertAlmostEqual(upper["coverage"], 0.982421875)

        interval = runner.two_sided_median_confidence_interval(values)
        self.assertEqual(interval["lower_order_statistic"], 4)
        self.assertEqual(interval["upper_order_statistic"], 12)
        self.assertEqual((interval["lower"], interval["upper"]), (4, 12))
        self.assertAlmostEqual(interval["coverage"], 0.96484375)

    def test_confidence_bounds_reject_empty_nonfinite_and_bad_confidence(self):
        for values in ([], [1.0, math.nan], [1.0, math.inf]):
            with self.assertRaises(ValueError):
                runner.one_sided_upper_median_bound(values)
        with self.assertRaises(ValueError):
            runner.two_sided_median_confidence_interval([1.0], confidence=1.0)

    def test_invalid_process_samples(self):
        for bad_cpp, bad_sv in ((0.0, 1.0), (math.nan, 1.0), (1.0, 0.0), (1.0, math.inf)):
            cpp, sv = paired_samples([1.0] * 15)
            cpp[0]["process_wall_ms"] = bad_cpp
            sv[0]["process_wall_ms"] = bad_sv
            with self.assertRaises(ValueError):
                runner.paired_ratio_statistics(cpp, sv)

    def test_pair_order_requires_alternation_adjacency_and_matching_ids(self):
        cpp, sv = paired_samples([1.0] * 15)
        runner.validate_pair_order(cpp, sv)

        cpp[0]["pair_order"] = ["pure_sv", "cpp_dpi"]
        with self.assertRaisesRegex(ValueError, "order"):
            runner.validate_pair_order(cpp, sv)
        cpp, sv = paired_samples([1.0] * 15)
        sv[0]["sequence_index"] += 3
        with self.assertRaisesRegex(ValueError, "adjacently"):
            runner.validate_pair_order(cpp, sv)
        cpp, sv = paired_samples([1.0] * 15)
        sv[0]["pair"] = 99
        with self.assertRaisesRegex(ValueError, "pair IDs"):
            runner.validate_pair_order(cpp, sv)

    def test_equivalence_mismatch_is_rejected(self):
        cpp, sv = paired_samples([1.0] * 15)
        sv[0]["sim_cycles"] += 1
        with self.assertRaisesRegex(ValueError, "sim_cycles"):
            runner.validate_pair_order(cpp, sv)

    def test_strict_guard_boundary(self):
        cpp, sv = paired_samples([1.10] * 15)
        result = runner.evaluate_guard(cpp, sv)
        self.assertEqual(result["status"], "passed")

        cpp, sv = paired_samples([1.100001] * 15)
        with self.assertRaises(runner.GuardFailure):
            runner.evaluate_guard(cpp, sv)

    def test_extra_batch_is_collected_for_inconclusive_bound(self):
        calls = []

        def fake_runner(mode, kernel, pair, iterations):
            calls.append((mode, pair))
            ratio = 2.0 if 12 <= pair <= 15 else 1.0
            return sample(mode, pair, ratio if mode == "cpp_dpi" else 1.0,
                          kernel=kernel, iterations=iterations)

        summaries, raw = runner.run_comparison(
            ["control"], 1, 15, sample_runner=fake_runner
        )
        guard = summaries["control"]["guard"]
        self.assertTrue(guard["extra_batch_collected"])
        self.assertEqual(guard["status"], "passed")
        self.assertEqual(len(raw), 60)
        self.assertEqual(len(calls), 62)  # two warmups plus 30 adjacent pairs


if __name__ == "__main__":
    unittest.main()
