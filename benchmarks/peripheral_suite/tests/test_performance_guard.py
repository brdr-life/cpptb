import importlib.util
import math
import os
import unittest
from datetime import datetime, timedelta
from pathlib import Path
from unittest import mock


RUNNER_PATH = Path(__file__).resolve().parents[1] / "run_benchmark.py"
SPEC = importlib.util.spec_from_file_location("peripheral_benchmark", RUNNER_PATH)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class PerformanceGuardTests(unittest.TestCase):
    @staticmethod
    def summary(ratios, baseline=100.0, label="sample"):
        times = [ratio * baseline for ratio in ratios]
        samples = [
            {"run": run, "process_wall_ms": process_wall_ms}
            for run, process_wall_ms in enumerate(times, start=1)
        ]
        return {
            "label": label,
            "runs": len(samples),
            "process_wall_ms_median": RUNNER.median(times),
            "samples": samples,
        }

    @classmethod
    def guard(cls, ratios, final=True):
        return RUNNER.check_dpi_vs_pure_sv(
            cls.summary(ratios, label="cpp_dpi"),
            cls.summary([1.0] * len(ratios), label="pure_sv"),
            final=final,
        )

    def test_passes_at_ten_percent_limit(self):
        result = self.guard([1.10] * RUNNER.MIN_COMPARISON_PAIRS)
        self.assertEqual(result["status"], "passed")
        self.assertAlmostEqual(result["ratio"], 1.10)

    def test_hard_fails_only_above_median_limit(self):
        ratios = [1.0] * 7 + [1.1001] * 8
        with self.assertRaisesRegex(SystemExit, "Stop and consult"):
            self.guard(ratios)

    def test_passing_median_with_high_bound_is_inconclusive_not_failure(self):
        ratios = [1.0] * 9 + [1.2] * 6
        result = self.guard(ratios)
        self.assertEqual(result["status"], "passed_inconclusive")
        self.assertIn("one-sided 95%", result["warning"])

    def test_nonfinal_high_bound_requests_extra_batch(self):
        ratios = [1.0] * 9 + [1.2] * 6
        result = self.guard(ratios, final=False)
        self.assertEqual(result["status"], "needs_extra_batch")

    def test_rejects_fewer_than_minimum_pairs(self):
        with self.assertRaisesRegex(SystemExit, "at least 15"):
            self.guard([1.0] * 14)

    def test_rejects_zero_pure_sv_time(self):
        dpi = self.summary([1.0] * RUNNER.MIN_COMPARISON_PAIRS)
        pure_sv = self.summary([1.0] * RUNNER.MIN_COMPARISON_PAIRS)
        pure_sv["samples"][4]["process_wall_ms"] = 0.0
        with self.assertRaisesRegex(SystemExit, "greater than zero"):
            RUNNER.check_dpi_vs_pure_sv(dpi, pure_sv)

    def test_uses_paired_ratios_instead_of_independent_medians(self):
        dpi_times = [100.0, 220.0, 300.0] * 5
        sv_times = [100.0, 200.0, 100.0] * 5
        dpi = self.summary([value / 100.0 for value in dpi_times])
        sv = self.summary([value / 100.0 for value in sv_times])
        result = RUNNER.check_dpi_vs_pure_sv(dpi, sv)
        self.assertAlmostEqual(result["ratio"], 1.10)
        self.assertAlmostEqual(result["cpp_dpi_process_wall_ms"], 220.0)
        self.assertAlmostEqual(result["pure_sv_process_wall_ms"], 100.0)

    def test_rejects_mismatched_run_ids(self):
        dpi = self.summary([1.0] * RUNNER.MIN_COMPARISON_PAIRS)
        pure_sv = self.summary([1.0] * RUNNER.MIN_COMPARISON_PAIRS)
        pure_sv["samples"][-1]["run"] = 99
        with self.assertRaisesRegex(SystemExit, "matching run IDs"):
            RUNNER.check_dpi_vs_pure_sv(dpi, pure_sv)

    def test_rejects_duplicate_run_ids(self):
        dpi = self.summary([1.0] * RUNNER.MIN_COMPARISON_PAIRS)
        pure_sv = self.summary([1.0] * RUNNER.MIN_COMPARISON_PAIRS)
        dpi["samples"][-1]["run"] = 1
        with self.assertRaisesRegex(SystemExit, "duplicate run IDs"):
            RUNNER.check_dpi_vs_pure_sv(dpi, pure_sv)


class ConfidenceIntervalTests(unittest.TestCase):
    def test_one_sided_95_bound_uses_twelfth_order_statistic_for_15(self):
        result = RUNNER.one_sided_upper_median_bound(range(1, 16))
        self.assertEqual(result["order_statistic"], 12)
        self.assertEqual(result["bound"], 12.0)
        self.assertGreaterEqual(result["coverage"], 0.95)

    def test_one_sided_bound_is_infinite_when_sample_is_too_small(self):
        result = RUNNER.one_sided_upper_median_bound([1.0])
        self.assertTrue(math.isinf(result["bound"]))

    def test_two_sided_95_interval_is_exact_and_conservative(self):
        result = RUNNER.two_sided_median_confidence_interval(range(1, 16))
        self.assertEqual(result["lower_order_statistic"], 4)
        self.assertEqual(result["upper_order_statistic"], 12)
        self.assertEqual((result["lower"], result["upper"]), (4.0, 12.0))
        self.assertGreaterEqual(result["coverage"], 0.95)

    def test_direction_is_only_reported_when_ci_excludes_one(self):
        below = PerformanceGuardTests.summary([0.9] * 15)
        equal = PerformanceGuardTests.summary([1.0] * 15)
        above = PerformanceGuardTests.summary([1.1] * 15)

        faster = RUNNER.paired_ratio_statistics(below, equal, "detached", "tracked")
        slower = RUNNER.paired_ratio_statistics(above, equal, "detached", "tracked")
        mixed = PerformanceGuardTests.summary([0.9] * 7 + [1.0] + [1.1] * 7)
        unclear = RUNNER.paired_ratio_statistics(mixed, equal, "detached", "tracked")

        self.assertEqual(faster["direction"], "detached_faster")
        self.assertEqual(slower["direction"], "detached_slower")
        self.assertEqual(unclear["direction"], "inconclusive")

    def test_direction_is_inconclusive_when_ci_touches_one(self):
        denominator = PerformanceGuardTests.summary([1.0] * 15)
        upper_touches_one = PerformanceGuardTests.summary(
            [0.9] * 11 + [1.0] * 4
        )
        lower_touches_one = PerformanceGuardTests.summary(
            [1.0] * 4 + [1.1] * 11
        )

        faster_boundary = RUNNER.paired_ratio_statistics(
            upper_touches_one, denominator, "detached", "tracked"
        )
        slower_boundary = RUNNER.paired_ratio_statistics(
            lower_touches_one, denominator, "detached", "tracked"
        )

        self.assertEqual(
            faster_boundary["two_sided_95_median_ci"]["upper"], 1.0
        )
        self.assertEqual(
            slower_boundary["two_sided_95_median_ci"]["lower"], 1.0
        )
        self.assertEqual(faster_boundary["direction"], "inconclusive")
        self.assertEqual(slower_boundary["direction"], "inconclusive")


class WorkloadValidationTests(unittest.TestCase):
    @staticmethod
    def sample(run=1, cycles=10, checks=20, failures=0):
        return {
            "run": run,
            "sim_cycles": cycles,
            "checks": checks,
            "failures": failures,
        }

    def test_rejects_unequal_run_counts_explicitly(self):
        with self.assertRaisesRegex(SystemExit, "run-count mismatch"):
            RUNNER.assert_same_workload(
                {"dpi": [self.sample()], "sv": [self.sample(), self.sample(2)]}
            )

    def test_accepts_all_zero_cycle_counts(self):
        RUNNER.assert_same_workload(
            {"dpi": [self.sample(cycles=0)], "sv": [self.sample(cycles=0)]}
        )

    def test_rejects_one_zero_cycle_count(self):
        with self.assertRaisesRegex(SystemExit, "sim-cycle mismatch"):
            RUNNER.assert_same_workload(
                {"dpi": [self.sample(cycles=10)], "sv": [self.sample(cycles=0)]}
            )


class MetadataTests(unittest.TestCase):
    def test_collects_reproducibility_metadata(self):
        def command_result(command):
            outputs = {
                ("git", "rev-parse", "HEAD"): "abc123",
                ("git", "status", "--porcelain"): " M local-change",
                ("verilator", "--getenv", "VERILATOR_ROOT"): "/verilator",
            }
            return {
                "command": list(command),
                "returncode": 0,
                "output": outputs.get(tuple(command), "tool version"),
            }

        with (
            mock.patch.object(RUNNER, "_metadata_command", side_effect=command_result),
            mock.patch.object(RUNNER.shutil, "which", side_effect=lambda tool: f"/bin/{tool}"),
            mock.patch.dict(os.environ, {}, clear=True),
        ):
            metadata = RUNNER.collect_metadata(
                {"iterations": 10_000}, argv=["run_benchmark.py", "--skip-build"]
            )

        self.assertEqual(metadata["git"], {"commit": "abc123", "dirty": True})
        self.assertIn("platform", metadata["host"])
        self.assertIn("compiler", metadata["python"])
        self.assertEqual(metadata["compiler"]["executable"], "/bin/c++")
        self.assertEqual(metadata["verilator"]["root"], "/verilator")
        self.assertEqual(metadata["command"], ["run_benchmark.py", "--skip-build"])
        self.assertEqual(metadata["config"], {"iterations": 10_000})

        self.assertEqual(
            set(metadata),
            {
                "timestamp_utc",
                "git",
                "host",
                "python",
                "compiler",
                "verilator",
                "command",
                "config",
            },
        )
        self.assertEqual(set(metadata["git"]), {"commit", "dirty"})
        self.assertEqual(
            set(metadata["host"]),
            {"hostname", "platform", "system", "release", "machine", "processor"},
        )
        self.assertEqual(
            set(metadata["python"]),
            {"version", "implementation", "compiler", "executable"},
        )
        self.assertEqual(
            set(metadata["compiler"]),
            {"command", "returncode", "output", "executable", "environment"},
        )
        self.assertEqual(
            set(metadata["verilator"]),
            {
                "command",
                "returncode",
                "output",
                "executable",
                "root",
                "environment",
            },
        )
        timestamp = datetime.fromisoformat(
            metadata["timestamp_utc"].replace("Z", "+00:00")
        )
        self.assertEqual(timestamp.utcoffset(), timedelta(0))


class CriticalComparisonTests(unittest.TestCase):
    @staticmethod
    def runners(ratio_for_run):
        calls = []

        def dpi(run, iters, spawn_mode):
            calls.append(("dpi", run, spawn_mode))
            ratio = 1.0 if run == 0 else ratio_for_run(run)
            return {
                "run": run,
                "process_wall_ms": ratio * 100.0,
                "internal_wall_ms": ratio * 90.0,
                "checks": 20,
                "sim_cycles": 10,
                "failures": 0,
            }

        def sv(run, iters):
            calls.append(("sv", run, None))
            return {
                "run": run,
                "process_wall_ms": 100.0,
                "checks": 20,
                "sim_cycles": 10,
                "failures": 0,
            }

        return dpi, sv, calls

    def test_pairs_are_adjacent_and_alternate_order_after_warmup(self):
        dpi, sv, calls = self.runners(lambda run: 1.0)
        dpi_samples, sv_samples, guard = RUNNER.run_critical_comparison(
            10_000, 15, dpi_sample_runner=dpi, sv_sample_runner=sv
        )
        self.assertEqual(guard["status"], "passed")
        self.assertFalse(guard["extra_batch_collected"])
        self.assertEqual(len(dpi_samples), 15)
        self.assertEqual(len(sv_samples), 15)
        self.assertEqual(calls[:2], [("dpi", 0, "tracked"), ("sv", 0, None)])
        self.assertEqual(
            calls[2:8],
            [
                ("dpi", 1, "tracked"),
                ("sv", 1, None),
                ("sv", 2, None),
                ("dpi", 2, "tracked"),
                ("dpi", 3, "tracked"),
                ("sv", 3, None),
            ],
        )

    def test_collects_one_extra_batch_when_initial_bound_is_high(self):
        dpi, sv, _ = self.runners(
            lambda run: 1.2 if 10 <= run <= 15 else 1.0
        )
        dpi_samples, _, guard = RUNNER.run_critical_comparison(
            10_000, 15, dpi_sample_runner=dpi, sv_sample_runner=sv
        )
        self.assertEqual(len(dpi_samples), 30)
        self.assertTrue(guard["extra_batch_collected"])
        self.assertEqual(guard["status"], "passed")

    def test_passes_with_warning_if_extra_batch_remains_inconclusive(self):
        dpi, sv, _ = self.runners(
            lambda run: 1.0 if (run - 1) % 15 < 9 else 1.2
        )
        _, _, guard = RUNNER.run_critical_comparison(
            10_000, 15, dpi_sample_runner=dpi, sv_sample_runner=sv
        )
        self.assertEqual(guard["status"], "passed_inconclusive")
        self.assertTrue(guard["extra_batch_collected"])
        self.assertIn("warning", guard)

    def test_hard_fails_if_combined_median_crosses_limit(self):
        dpi, sv, _ = self.runners(lambda run: 1.0 if run <= 8 else 1.2)
        with self.assertRaisesRegex(SystemExit, "Stop and consult"):
            RUNNER.run_critical_comparison(
                10_000, 15, dpi_sample_runner=dpi, sv_sample_runner=sv
            )

    def test_hard_fails_first_batch_without_collecting_extra_pairs(self):
        dpi, sv, calls = self.runners(lambda run: 1.2)
        with self.assertRaisesRegex(SystemExit, "Stop and consult"):
            RUNNER.run_critical_comparison(
                10_000, 15, dpi_sample_runner=dpi, sv_sample_runner=sv
            )
        self.assertEqual(len(calls), 2 + 2 * 15)

    def test_parser_defaults_and_minimum(self):
        args = RUNNER._parse_args([])
        self.assertEqual(args.iters, 10_000)
        self.assertEqual(args.comparison_runs, 15)
        with self.assertRaises(SystemExit):
            RUNNER._parse_args(["--comparison-runs", "14"])

    def test_tracked_environment_clears_detached_override(self):
        with mock.patch.dict(os.environ, {"CPPTB_BENCH_DETACHED_SPAWN": "1"}):
            tracked = RUNNER._dpi_environment("tracked")
            detached = RUNNER._dpi_environment("detached")
        self.assertNotIn("CPPTB_BENCH_DETACHED_SPAWN", tracked)
        self.assertEqual(detached["CPPTB_BENCH_DETACHED_SPAWN"], "1")


if __name__ == "__main__":
    unittest.main()
