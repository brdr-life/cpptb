import unittest
from pathlib import Path

from benchmarks.framework_comparison.workload import (
    AUTHORING_WORKLOADS,
    MODES,
    expected_authoring_result,
    rotated_mode_order,
)


REPO = Path(__file__).resolve().parents[3]
COCOTB_TEST = (
    REPO
    / "benchmarks"
    / "framework_comparison"
    / "testbenches"
    / "cocotb"
    / "test_authoring_core_comparison.py"
)
VPI_HOST = (
    REPO
    / "benchmarks"
    / "framework_comparison"
    / "testbenches"
    / "cpp_vpi"
    / "authoring_core_vpi_host.cpp"
)


class WorkloadContractTests(unittest.TestCase):
    def test_selected_workloads_cover_distinct_transport_shapes(self):
        self.assertEqual(
            AUTHORING_WORKLOADS,
            ("control", "wide_echo_137", "signal_edge"),
        )

    def test_expected_counts_match_feature_specific_workloads(self):
        control = expected_authoring_result("control", 10)
        wide = expected_authoring_result("wide_echo_137", 10)
        edge = expected_authoring_result("signal_edge", 10)
        self.assertEqual(control["checks"], 12)
        self.assertEqual(wide["checks"], 22)
        self.assertEqual(wide["wide_echo_137"], 10)
        self.assertEqual(edge["signal_edges"], 10)
        self.assertEqual(control["checksum"], wide["checksum"])

    def test_mode_rotation_balances_every_slot(self):
        orders = [rotated_mode_order(index) for index in range(8)]
        for slot in range(len(MODES)):
            counts = {mode: 0 for mode in MODES}
            for order in orders:
                counts[order[slot]] += 1
            self.assertEqual(set(counts.values()), {2})

    def test_cocotb_sequence_contains_exact_observation_and_edge_waits(self):
        source = COCOTB_TEST.read_text(encoding="utf-8")
        self.assertIn('await Timer(1, unit="ps")', source)
        self.assertIn("await FallingEdge(dut.clk)", source)
        self.assertIn("await RisingEdge(dut.rsp_valid)", source)
        self.assertIn("state.checksum =", source)

    def test_vpi_clock_waiters_resume_before_dut_evaluation(self):
        source = VPI_HOST.read_text(encoding="utf-8")
        clock_write = source.index("dut.clk.set(clock_high ? 1u : 0u);")
        notification = source.index("scheduler.notify_edge(", clock_write)
        evaluation = source.index("settle(top.get());", notification)
        self.assertLess(clock_write, notification)
        self.assertLess(notification, evaluation)


if __name__ == "__main__":
    unittest.main()
