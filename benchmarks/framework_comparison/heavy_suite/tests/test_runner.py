import unittest

from benchmarks.framework_comparison.heavy_suite.run_benchmark import (
    BenchmarkError,
    parse_result,
    validate,
)


class HeavyRunnerTests(unittest.TestCase):
    def test_parse_result(self):
        result = parse_result(
            "HEAVY_BENCH_RESULT mode=cpp_dpi workload=streaming_fir iterations=3 "
            "transactions=3 checks=4 sim_cycles=7 checksum=3743260270 failures=0"
        )
        self.assertEqual(result["checksum"], 3743260270)
        self.assertEqual(result["mode"], "cpp_dpi")

    def test_validate_requires_exact_cycles(self):
        result = {
            "mode": "pure_sv",
            "workload": "streaming_fir",
            "iterations": 3,
            "transactions": 3,
            "checks": 4,
            "sim_cycles": 7,
            "checksum": 3743260270,
            "failures": 0,
        }
        validate(result, "pure_sv", "streaming_fir", 3, 7)
        with self.assertRaises(BenchmarkError):
            validate(result, "pure_sv", "streaming_fir", 3, 8)


if __name__ == "__main__":
    unittest.main()
