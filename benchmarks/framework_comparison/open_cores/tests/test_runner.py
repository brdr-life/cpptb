import unittest

from benchmarks.framework_comparison.open_cores.run_benchmark import (
    BenchmarkError,
    parse_result,
    validate,
)


class OpenCoreRunnerTests(unittest.TestCase):
    def test_parse_result(self):
        result = parse_result(
            "OPEN_CORE_BENCH_RESULT mode=cpp_dpi workload=picorv32_firmware "
            "iterations=3 transactions=3 checks=2 sim_cycles=100 "
            "checksum=3407432851 failures=0"
        )
        self.assertEqual(result["checksum"], 3407432851)
        self.assertEqual(result["mode"], "cpp_dpi")

    def test_validate_requires_exact_cycles(self):
        result = {
            "mode": "pure_sv",
            "workload": "picorv32_firmware",
            "iterations": 3,
            "transactions": 3,
            "checks": 2,
            "sim_cycles": 100,
            "checksum": 3407432851,
            "failures": 0,
        }
        validate(result, "pure_sv", "picorv32_firmware", 3, 100)
        with self.assertRaises(BenchmarkError):
            validate(result, "pure_sv", "picorv32_firmware", 3, 101)


if __name__ == "__main__":
    unittest.main()
