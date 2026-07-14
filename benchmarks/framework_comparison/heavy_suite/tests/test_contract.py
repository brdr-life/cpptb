import unittest

from benchmarks.framework_comparison.heavy_suite.workload import (
    DEFAULT_ITERATIONS,
    MODES,
    WORKLOADS,
    expected_result,
    rotated_mode_order,
)


class HeavyContractTests(unittest.TestCase):
    def test_workloads_have_realistic_independent_defaults(self):
        self.assertEqual(WORKLOADS, ("streaming_fir", "packet_crc32", "matrix4x4"))
        self.assertGreater(DEFAULT_ITERATIONS["streaming_fir"], DEFAULT_ITERATIONS["packet_crc32"])

    def test_known_small_results_match_all_testbench_implementations(self):
        self.assertEqual(expected_result("streaming_fir", 3)["checksum"], 3743260270)
        self.assertEqual(expected_result("packet_crc32", 3)["checksum"], 528355919)
        matrix = expected_result("matrix4x4", 3)
        self.assertEqual(matrix["checksum"], 2584832245)
        self.assertEqual(matrix["checks"], 97)

    def test_rotation_balances_slots(self):
        orders = [rotated_mode_order(index) for index in range(4)]
        for slot in range(4):
            self.assertEqual({order[slot] for order in orders}, set(MODES))


if __name__ == "__main__":
    unittest.main()
