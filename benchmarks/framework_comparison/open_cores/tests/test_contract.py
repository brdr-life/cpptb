import ast
import hashlib
import unittest
from pathlib import Path

from benchmarks.framework_comparison.open_cores.workload import (
    DEFAULT_ITERATIONS,
    MODES,
    WORKLOADS,
    expected_result,
    frame_length,
    rotated_mode_order,
)


class OpenCoreContractTests(unittest.TestCase):
    def test_workloads_cover_cpu_block_and_streaming_cores(self):
        self.assertEqual(
            WORKLOADS,
            ("picorv32_firmware", "secworks_aes128", "ethernet_fcs64"),
        )
        self.assertEqual(set(DEFAULT_ITERATIONS), set(WORKLOADS))

    def test_known_small_results(self):
        self.assertEqual(expected_result("picorv32_firmware", 3)["checksum"], 3407432851)
        self.assertEqual(expected_result("secworks_aes128", 3)["checksum"], 2309395819)
        self.assertEqual(expected_result("ethernet_fcs64", 3)["checksum"], 1488893890)

    def test_ethernet_lengths_cover_legal_frame_range(self):
        lengths = [frame_length(packet) for packet in range(1455)]
        self.assertEqual(min(lengths), 64)
        self.assertEqual(max(lengths), 1518)

    def test_rotation_balances_slots(self):
        orders = [rotated_mode_order(index) for index in range(4)]
        for slot in range(4):
            self.assertEqual({order[slot] for order in orders}, set(MODES))

    def test_vendored_licenses_are_present(self):
        third_party = Path(__file__).resolve().parents[1] / "third_party"
        self.assertTrue((third_party / "picorv32" / "COPYING").is_file())
        self.assertTrue((third_party / "secworks_aes" / "LICENSE").is_file())
        self.assertTrue((third_party / "verilog_ethernet" / "COPYING").is_file())

    def test_vendored_rtl_matches_pinned_sources(self):
        third_party = Path(__file__).resolve().parents[1] / "third_party"
        expected = {
            "picorv32": "7d931f911cf225c0bb045528b990860d6981beefb7f07f755ac24aa89aa8cf5b",
            "secworks_aes": "cfc4799b3c0c135a21f9c8009756dd62c56c4436adab405642d81cdfe310fa22",
            "verilog_ethernet": "62270dd3e01541ff8aad700e050c432acf22383ce659acf08c151080797d9b7a",
        }
        for directory, expected_digest in expected.items():
            digest = hashlib.sha256()
            for rtl in sorted((third_party / directory).glob("*.v")):
                digest.update(rtl.name.encode())
                digest.update(b"\0")
                digest.update(rtl.read_bytes())
            self.assertEqual(digest.hexdigest(), expected_digest)

    def test_cocotb_example_exposes_every_open_core_workload(self):
        suite = Path(__file__).resolve().parents[1]
        source_path = suite / "testbenches" / "cocotb" / "test_open_cores.py"
        tree = ast.parse(source_path.read_text(encoding="utf-8"))
        async_functions = {
            node.name for node in tree.body if isinstance(node, ast.AsyncFunctionDef)
        }
        self.assertTrue(
            {
                "run_picorv32",
                "run_aes",
                "run_fcs",
                "open_cores_benchmark",
            }.issubset(async_functions)
        )
        self.assertIn("cocotb", MODES)


if __name__ == "__main__":
    unittest.main()
