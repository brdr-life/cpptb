import importlib.util
import json
import math
import os
import tempfile
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
            {
                "run": run,
                "process_wall_ms": process_wall_ms,
                "pair_order": (
                    ["cpp_dpi", "pure_sv"]
                    if run % 2
                    else ["pure_sv", "cpp_dpi"]
                ),
            }
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
        result = self.guard([1.10] * RUNNER.MIN_CRITICAL_COMPARISON_PAIRS)
        self.assertEqual(result["status"], "passed")
        self.assertAlmostEqual(result["ratio"], 1.10)

    def test_hard_fails_only_above_median_limit(self):
        result = self.guard([1.1001] * RUNNER.MIN_CRITICAL_COMPARISON_PAIRS)
        self.assertEqual(result["status"], "hard_failure")
        self.assertEqual(result["verdict"], "failed")
        self.assertEqual(result["validity"], "valid")

    def test_passing_median_with_high_bound_is_inconclusive_not_failure(self):
        ratios = [1.0] * 10 + [1.2] * 6
        result = self.guard(ratios)
        self.assertEqual(result["status"], "passed_inconclusive")
        self.assertIn("one-sided 95%", result["warning"])

    def test_nonfinal_high_bound_requests_extra_batch(self):
        ratios = [1.0] * 10 + [1.2] * 6
        result = self.guard(ratios, final=False)
        self.assertEqual(result["status"], "needs_extra_batch")

    def test_rejects_fewer_than_minimum_pairs(self):
        result = self.guard([1.0] * 14)
        self.assertEqual(result["status"], "invalid_environment")
        self.assertIn("at least 16", result["error"])

    def test_rejects_odd_pair_count(self):
        result = self.guard([1.0] * 17)
        self.assertEqual(result["status"], "invalid_environment")
        self.assertIn("even count", result["error"])

    def test_rejects_zero_pure_sv_time(self):
        dpi = self.summary([1.0] * RUNNER.MIN_CRITICAL_COMPARISON_PAIRS)
        pure_sv = self.summary([1.0] * RUNNER.MIN_CRITICAL_COMPARISON_PAIRS)
        pure_sv["samples"][4]["process_wall_ms"] = 0.0
        result = RUNNER.check_dpi_vs_pure_sv(dpi, pure_sv)
        self.assertEqual(result["status"], "invalid_environment")
        self.assertIn("greater than zero", result["error"])

    def test_uses_paired_ratios_instead_of_independent_medians(self):
        dpi_times = [100.0, 220.0, 300.0, 110.0] * 4
        sv_times = [100.0, 200.0, 100.0, 100.0] * 4
        dpi = self.summary([value / 100.0 for value in dpi_times])
        sv = self.summary([value / 100.0 for value in sv_times])
        result = RUNNER.check_dpi_vs_pure_sv(dpi, sv)
        self.assertAlmostEqual(result["ratio"], 1.10)
        self.assertAlmostEqual(result["cpp_dpi_process_wall_ms"], 165.0)
        self.assertAlmostEqual(result["pure_sv_process_wall_ms"], 100.0)
        self.assertAlmostEqual(result["independent_median_ratio"], 1.65)
        self.assertIn("dpi_first_paired_median", result)
        self.assertIn("sv_first_paired_median", result)
        self.assertIn("order_stratum_gap", result)

    def test_rejects_mismatched_run_ids(self):
        dpi = self.summary([1.0] * RUNNER.MIN_CRITICAL_COMPARISON_PAIRS)
        pure_sv = self.summary([1.0] * RUNNER.MIN_CRITICAL_COMPARISON_PAIRS)
        pure_sv["samples"][-1]["run"] = 99
        result = RUNNER.check_dpi_vs_pure_sv(dpi, pure_sv)
        self.assertEqual(result["status"], "invalid_environment")
        self.assertIn("matching run IDs", result["error"])

    def test_rejects_duplicate_run_ids(self):
        dpi = self.summary([1.0] * RUNNER.MIN_CRITICAL_COMPARISON_PAIRS)
        pure_sv = self.summary([1.0] * RUNNER.MIN_CRITICAL_COMPARISON_PAIRS)
        dpi["samples"][-1]["run"] = 1
        result = RUNNER.check_dpi_vs_pure_sv(dpi, pure_sv)
        self.assertEqual(result["status"], "invalid_environment")
        self.assertIn("duplicate run IDs", result["error"])

    def test_threshold_crossing_with_weak_stratum_is_invalid(self):
        ratios = [1.20 if run % 2 else 1.02 for run in range(1, 17)]
        result = self.guard(ratios)
        self.assertAlmostEqual(result["ratio"], 1.11)
        self.assertEqual(result["status"], "invalid_environment")
        self.assertEqual(result["verdict"], "invalid_environment")

    def test_threshold_crossing_with_independent_disagreement_is_invalid(self):
        dpi = self.summary([1.0] * 16, label="cpp_dpi")
        sv = self.summary([1.0] * 16, label="pure_sv")
        for index in range(16):
            ratio = 1.06 if index < 8 else 1.20
            denominator = 1000.0 if index < 8 else 1.0
            dpi["samples"][index]["process_wall_ms"] = ratio * denominator
            sv["samples"][index]["process_wall_ms"] = denominator
        dpi["process_wall_ms_median"] = RUNNER.median(
            [sample["process_wall_ms"] for sample in dpi["samples"]]
        )
        sv["process_wall_ms_median"] = RUNNER.median(
            [sample["process_wall_ms"] for sample in sv["samples"]]
        )
        result = RUNNER.check_dpi_vs_pure_sv(dpi, sv)
        self.assertGreater(result["ratio"], 1.10)
        self.assertGreater(result["independent_paired_relative_disagreement"], 0.05)
        self.assertEqual(result["status"], "invalid_environment")


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


class ArtifactDurabilityTests(unittest.TestCase):
    def test_atomic_writer_uses_mockable_replace_with_sibling_temp(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "latest.json"
            calls = []

            def replace(source, target):
                calls.append((Path(source), Path(target)))
                os.replace(source, target)

            RUNNER.atomic_write_text(destination, "evidence\n", replace=replace)

            self.assertEqual(destination.read_text(), "evidence\n")
            self.assertEqual(calls[0][1], destination)
            self.assertEqual(calls[0][0].parent, destination.parent)
            self.assertFalse(calls[0][0].exists())

    def test_jsonl_journal_flushes_and_fsyncs_each_sample(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "samples.jsonl"
            with mock.patch.object(RUNNER.os, "fsync") as fsync:
                journal = RUNNER.SampleJournal(path, truncate=True)
                journal.append(
                    {
                        "event": "sample",
                        "label": "cpp_dpi",
                        "sample": {
                            "sequence_index": 1,
                            "slot": 1,
                            "pair_order": ["cpp_dpi", "pure_sv"],
                            "requested_pair_count": 16,
                            "measured_pair_count": 1,
                            "binary_sha256": "a" * 64,
                        },
                    }
                )

            entry = json.loads(path.read_text())
            self.assertEqual(set(entry), {"event", "label", "sample"})
            self.assertEqual(entry["event"], "sample")
            self.assertEqual(entry["sample"]["slot"], 1)
            self.assertGreaterEqual(fsync.call_count, 2)

    def test_binary_sha256_hashes_file_contents(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "binary"
            path.write_bytes(b"peripheral-suite")
            self.assertEqual(
                RUNNER.binary_sha256(path),
                "5810b141fa1169406c7f902e57825845a04df0afb90cebc2dc8a90c9ade27864",
            )
            self.assertIsNone(RUNNER.binary_sha256(path.with_name("missing")))

    def test_hard_failure_and_invalid_environment_artifacts_keep_raw_samples(self):
        for status, ratios in (
            ("hard_failure", [1.2] * 16),
            (
                "invalid_environment",
                [1.2 if run % 2 else 1.02 for run in range(1, 17)],
            ),
        ):
            dpi = PerformanceGuardTests.summary(ratios, label="cpp_dpi")
            sv = PerformanceGuardTests.summary([1.0] * 16, label="pure_sv")
            guard = RUNNER.check_dpi_vs_pure_sv(dpi, sv)
            payload = {
                "status": guard["status"],
                "verdict": guard["verdict"],
                "requested_pairs": 16,
                "measured_pairs": 16,
                "cpp_dpi": dpi,
                "pure_sv": sv,
                "performance_guard": guard,
            }
            with tempfile.TemporaryDirectory() as directory:
                json_path = Path(directory) / "latest.json"
                md_path = Path(directory) / "latest.md"
                RUNNER.persist_diagnostic_result(payload, json_path, md_path)
                persisted = json.loads(json_path.read_text())
            self.assertEqual(persisted["status"], status)
            self.assertEqual(len(persisted["cpp_dpi"]["samples"]), 16)
            self.assertEqual(len(persisted["pure_sv"]["samples"]), 16)


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
            10_000, 16, dpi_sample_runner=dpi, sv_sample_runner=sv
        )
        self.assertEqual(guard["status"], "passed")
        self.assertFalse(guard["extra_batch_collected"])
        self.assertEqual(len(dpi_samples), 16)
        self.assertEqual(len(sv_samples), 16)
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
        self.assertEqual(
            {sample["slot"] for sample in dpi_samples}, {1, 2}
        )
        self.assertEqual(dpi_samples[0]["sequence_index"], 1)
        self.assertEqual(dpi_samples[0]["requested_pair_count"], 16)
        self.assertIn("binary_sha256", dpi_samples[0])
        self.assertEqual(len(guard["order_strata"]["cpp_dpi_first"]), 8)
        self.assertEqual(len(guard["order_strata"]["pure_sv_first"]), 8)

    def test_collects_one_extra_batch_when_initial_bound_is_high(self):
        dpi, sv, _ = self.runners(
            lambda run: 1.2 if 11 <= run <= 16 else 1.0
        )
        dpi_samples, _, guard = RUNNER.run_critical_comparison(
            10_000, 16, dpi_sample_runner=dpi, sv_sample_runner=sv
        )
        self.assertEqual(len(dpi_samples), 32)
        self.assertTrue(guard["extra_batch_collected"])
        self.assertEqual(guard["status"], "passed")

    def test_passes_with_warning_if_extra_batch_remains_inconclusive(self):
        dpi, sv, _ = self.runners(
            lambda run: 1.0 if (run - 1) % 16 < 10 else 1.2
        )
        _, _, guard = RUNNER.run_critical_comparison(
            10_000, 16, dpi_sample_runner=dpi, sv_sample_runner=sv
        )
        self.assertEqual(guard["status"], "passed_inconclusive")
        self.assertTrue(guard["extra_batch_collected"])
        self.assertIn("warning", guard)

    def test_hard_fails_if_combined_median_crosses_limit(self):
        dpi, sv, _ = self.runners(lambda run: 1.0 if run <= 8 else 1.2)
        dpi_samples, _, guard = RUNNER.run_critical_comparison(
            10_000, 16, dpi_sample_runner=dpi, sv_sample_runner=sv
        )
        self.assertEqual(len(dpi_samples), 32)
        self.assertEqual(guard["status"], "hard_failure")
        self.assertEqual(guard["verdict"], "failed")

    def test_hard_fails_first_batch_without_collecting_extra_pairs(self):
        dpi, sv, calls = self.runners(lambda run: 1.2)
        dpi_samples, sv_samples, guard = RUNNER.run_critical_comparison(
            10_000, 16, dpi_sample_runner=dpi, sv_sample_runner=sv
        )
        self.assertEqual(guard["status"], "hard_failure")
        self.assertEqual(len(dpi_samples), 16)
        self.assertEqual(len(sv_samples), 16)
        self.assertEqual(len(calls), 2 + 2 * 16)

    def test_real_collection_journal_has_one_durable_record_per_sample(self):
        dpi, sv, _ = self.runners(lambda run: 1.0)
        with tempfile.TemporaryDirectory() as directory:
            journal = RUNNER.SampleJournal(
                Path(directory) / "latest.jsonl", truncate=True
            )
            RUNNER.run_critical_comparison(
                10_000,
                16,
                dpi_sample_runner=dpi,
                sv_sample_runner=sv,
                journal=journal,
            )
            entries = [json.loads(line) for line in journal.path.read_text().splitlines()]

        self.assertEqual(len(entries), 34)
        self.assertTrue(all(entry["event"] == "sample" for entry in entries))
        required = {
            "binary_sha256",
            "sequence_index",
            "slot",
            "pair_order",
            "requested_pair_count",
            "measured_pair_count",
        }
        self.assertTrue(all(required <= set(entry["sample"]) for entry in entries))

    def test_workload_error_returns_collected_raw_evidence(self):
        dpi, sv, _ = self.runners(lambda run: 1.0)

        def failing_sv(run, iters):
            sample = sv(run, iters)
            if run == 8:
                sample["failures"] = 1
            return sample

        dpi_samples, sv_samples, guard = RUNNER.run_critical_comparison(
            10_000, 16, dpi_sample_runner=dpi, sv_sample_runner=failing_sv
        )
        self.assertEqual(guard["status"], "invalid_environment")
        self.assertIn("benchmark failures", guard["error"])
        self.assertEqual(len(dpi_samples), 16)
        self.assertEqual(len(sv_samples), 16)

    def test_parser_defaults_and_minimum(self):
        args = RUNNER._parse_args([])
        self.assertEqual(args.iters, 10_000)
        self.assertEqual(args.comparison_runs, 16)
        with self.assertRaises(SystemExit):
            RUNNER._parse_args(["--comparison-runs", "15"])
        with self.assertRaises(SystemExit):
            RUNNER._parse_args(["--comparison-runs", "17"])

    def test_tracked_environment_clears_detached_override(self):
        with mock.patch.dict(os.environ, {"CPPTB_BENCH_DETACHED_SPAWN": "1"}):
            tracked = RUNNER._dpi_environment("tracked")
            detached = RUNNER._dpi_environment("detached")
        self.assertNotIn("CPPTB_BENCH_DETACHED_SPAWN", tracked)
        self.assertEqual(detached["CPPTB_BENCH_DETACHED_SPAWN"], "1")


if __name__ == "__main__":
    unittest.main()
