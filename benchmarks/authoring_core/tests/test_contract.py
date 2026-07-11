import sys
import unittest
from pathlib import Path


BENCH_DIR = Path(__file__).resolve().parents[1]
REPO = BENCH_DIR.parents[1]
sys.path.insert(0, str(BENCH_DIR))

import run_benchmark as runner  # noqa: E402
import workload  # noqa: E402


def result_line(kernel="control", iterations=1, **overrides):
    expected = workload.expected_counts(kernel, iterations).fields()
    fields = {
        "mode": "cpp_dpi",
        "kernel": kernel,
        **expected,
        "sim_cycles": 7,
        "checksum": workload.expected_checksum(iterations),
        "failures": 0,
        "internal_wall_ms": 1.25,
    }
    fields.update(overrides)
    return "AUTHORING_CORE_RESULT " + " ".join(
        f"{key}={fields[key]}" for key in workload.RESULT_FIELDS
    )


class ContractTests(unittest.TestCase):
    def test_makefile_builds_every_authoring_kernel(self):
        makefile = (REPO / "Makefile").read_text(encoding="utf-8")
        kernel_line = next(
            line for line in makefile.splitlines()
            if line.startswith("AUTHORING_CORE_KERNELS :=")
        )
        self.assertEqual(tuple(kernel_line.split(":=", 1)[1].split()), workload.KERNELS)
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,task_timeout,8))",
            makefile,
        )

    def test_boundary_counts_one_iteration(self):
        control = workload.expected_counts("control", 1)
        self.assertEqual(control.transactions, 1)
        self.assertEqual(control.checks, 3)
        self.assertEqual(control.timeout_hits, 0)

        integrated = workload.expected_counts("all", 1)
        self.assertEqual(integrated.checks, 9)
        self.assertEqual(integrated.task_value, 1)
        self.assertEqual(integrated.clock_cycles, 1)
        self.assertEqual(integrated.timeouts, 1)
        self.assertEqual(integrated.timeout_hits, 0)
        self.assertEqual(integrated.task_timeouts, 1)
        self.assertEqual(integrated.task_timeout_hits, 0)
        self.assertEqual(integrated.event_set, 1)
        self.assertEqual(integrated.channel_receive, 1)

    def test_boundary_counts_even_and_odd(self):
        self.assertEqual(workload.expected_counts("timeout", 2).timeout_hits, 1)
        self.assertEqual(workload.expected_counts("timeout", 3).timeout_hits, 1)
        self.assertEqual(workload.expected_counts("timeout", 4).timeout_hits, 2)
        self.assertEqual(
            workload.expected_counts("task_timeout", 2).task_timeout_hits, 1
        )
        self.assertEqual(
            workload.expected_counts("task_timeout", 3).task_timeout_hits, 1
        )
        self.assertEqual(
            workload.expected_counts("task_timeout", 4).task_timeout_hits, 2
        )
        self.assertEqual(workload.expected_counts("all", 3).checks, 23)

    def test_task_timeout_has_exact_isolated_feature_counts(self):
        counts = workload.expected_counts("task_timeout", 5)
        self.assertEqual(counts.transactions, 5)
        self.assertEqual(counts.checks, 12)
        self.assertEqual(counts.task_timeouts, 5)
        self.assertEqual(counts.task_timeout_hits, 2)
        self.assertEqual(counts.timeouts, 0)
        self.assertEqual(counts.task_value, 0)

    def test_expected_checksum_is_stable(self):
        self.assertEqual(workload.expected_checksum(1), 1_407_418_725)
        self.assertEqual(workload.expected_checksum(3), 932_421_457)

    def test_parser_and_contract_accept_complete_result(self):
        result = runner.parse_result(
            result_line("all", 3), "cpp_dpi", "all", 3
        )
        runner.validate_contract(result)

    def test_parser_rejects_missing_duplicate_and_nonfinite_fields(self):
        complete = result_line()
        with self.assertRaisesRegex(ValueError, "missing fields"):
            runner.parse_result(complete.replace(" channel_receive=0", ""))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            runner.parse_result(complete + " checks=3")
        with self.assertRaisesRegex(ValueError, "finite"):
            runner.parse_result(complete + " internal_wall_ms=nan")
        with self.assertRaisesRegex(ValueError, "non-negative"):
            runner.parse_result(complete.replace("failures=0", "failures=-1"))

    def test_contract_rejects_count_checksum_and_failure_mismatches(self):
        for override, message in (
            ({"transactions": 2}, "transactions"),
            ({"checksum": 0}, "checksum"),
            ({"failures": 1}, "reported"),
        ):
            result = runner.parse_result(result_line(**override))
            with self.assertRaisesRegex(ValueError, message):
                runner.validate_contract(result)

    def test_invalid_contract_arguments(self):
        with self.assertRaises(ValueError):
            workload.expected_counts("missing", 1)
        with self.assertRaises(ValueError):
            workload.expected_counts("control", 0)
        with self.assertRaises(ValueError):
            workload.expected_checksum(0)


if __name__ == "__main__":
    unittest.main()
