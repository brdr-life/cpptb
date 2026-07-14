import unittest

from benchmarks.framework_comparison.run_benchmark import (
    BenchmarkError,
    evaluate_performance_guard,
    markdown,
    parse_result,
    validate_authoring_result,
)


class RunnerTests(unittest.TestCase):
    def test_parse_normalizes_authoring_kernel_to_workload(self):
        result = parse_result(
            "AUTHORING_CORE_RESULT mode=pure_sv kernel=control iterations=2 "
            "transactions=2 checks=4 sim_cycles=13 checksum=2409415489 "
            "failures=0 wide_echo_137=0 signal_edges=0"
        )
        self.assertEqual(result["workload"], "control")
        self.assertEqual(result["mode"], "pure_sv")
        self.assertEqual(result["sim_cycles"], 13)

    def test_semantic_validation_accepts_exact_result(self):
        result = {
            "mode": "cpp_vpi",
            "workload": "control",
            "iterations": 2,
            "transactions": 2,
            "checks": 4,
            "sim_cycles": 13,
            "checksum": 2409415489,
            "failures": 0,
            "wide_echo_137": 0,
            "signal_edges": 0,
        }
        validate_authoring_result(result, "cpp_vpi", "control", 2, 13)

    def test_semantic_validation_rejects_cycle_mismatch(self):
        result = {
            "mode": "cocotb",
            "workload": "control",
            "iterations": 2,
            "transactions": 2,
            "checks": 4,
            "sim_cycles": 12,
            "checksum": 2409415489,
            "failures": 0,
            "wide_echo_137": 0,
            "signal_edges": 0,
        }
        with self.assertRaises(BenchmarkError):
            validate_authoring_result(result, "cocotb", "control", 2, 13)

    def test_markdown_renders_normalized_matrix(self):
        def mode(value):
            return {"process_wall_ms_median": value}

        result = {
            "workloads": {
                "control": {
                    "description": {"title": "Clocked request/response"},
                    "iterations": 100,
                    "semantic_only": False,
                    "modes": {
                        "pure_sv": mode(10.0),
                        "cpp_dpi": mode(11.0),
                        "cpp_vpi": mode(20.0),
                        "cocotb": mode(30.0),
                    },
                    "ratios": {
                        "cpp_dpi_over_pure_sv": 1.1,
                        "cpp_vpi_over_pure_sv": 2.0,
                        "cocotb_over_pure_sv": 3.0,
                        "cpp_vpi_over_cpp_dpi": 1.82,
                        "cocotb_over_cpp_dpi": 2.73,
                    },
                }
            }
        }
        rendered = markdown(result)
        self.assertIn("Clocked request/response", rendered)
        self.assertIn("11.0 ms / 1.10x", rendered)
        self.assertIn("30.0 ms / 3.00x", rendered)

    def test_performance_guard_rejects_dpi_ratio_above_ten_percent(self):
        guard = evaluate_performance_guard(
            {
                "control": {
                    "semantic_only": False,
                    "ratios": {"cpp_dpi_over_pure_sv": 1.11},
                },
                "peripheral_suite": {
                    "semantic_only": False,
                    "ratios": {"cpp_dpi_over_pure_sv": 0.99},
                },
            }
        )
        self.assertEqual(guard["status"], "hard_failure")
        self.assertEqual(guard["violations"], {"control": 1.11})


if __name__ == "__main__":
    unittest.main()
