import hashlib
import io
import json
import math
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock


BENCH_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BENCH_DIR))

import run_benchmark as runner  # noqa: E402
import workload  # noqa: E402


def sample(
    mode,
    pair,
    process_ms,
    kernel="control",
    iterations=1,
    sequence=None,
):
    expected = workload.expected_counts(kernel, iterations).fields()
    order = ["cpp_dpi", "pure_sv"] if pair % 2 else ["pure_sv", "cpp_dpi"]
    order_index = order.index(mode)
    slot = order_index + 1
    if sequence is None:
        sequence = (pair - 1) * 2 + order_index
    return {
        "mode": mode,
        "kernel": kernel,
        **expected,
        "sim_cycles": 7,
        "checksum": workload.expected_checksum(iterations),
        "failures": 0,
        "pair": pair,
        "pair_order": order,
        "slot": slot,
        "sequence_index": sequence,
        "process_wall_ms": process_ms,
    }


def paired_samples(ratios, sv_times=None):
    cpp, sv = [], []
    sv_times = sv_times or [1.0] * len(ratios)
    for pair, (ratio, sv_ms) in enumerate(zip(ratios, sv_times), 1):
        order = ["cpp_dpi", "pure_sv"] if pair % 2 else ["pure_sv", "cpp_dpi"]
        base = (pair - 1) * 2
        cpp.append(
            sample(
                "cpp_dpi",
                pair,
                ratio * sv_ms,
                sequence=base + order.index("cpp_dpi"),
            )
        )
        sv.append(
            sample(
                "pure_sv",
                pair,
                sv_ms,
                sequence=base + order.index("pure_sv"),
            )
        )
    return cpp, sv


def result_output(mode, kernel="control", iterations=1):
    expected = workload.expected_counts(kernel, iterations).fields()
    fields = {
        "mode": mode,
        "kernel": kernel,
        **expected,
        "sim_cycles": 7,
        "checksum": workload.expected_checksum(iterations),
        "failures": 0,
        "internal_wall_ms": 1.0,
    }
    return "AUTHORING_CORE_RESULT " + " ".join(
        f"{name}={fields[name]}" for name in workload.RESULT_FIELDS
    )


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

    def test_confidence_bounds_reject_bad_inputs(self):
        for values in ([], [1.0, math.nan], [1.0, math.inf]):
            with self.assertRaises(ValueError):
                runner.one_sided_upper_median_bound(values)
        with self.assertRaises(ValueError):
            runner.two_sided_median_confidence_interval([1.0], confidence=1.0)

    def test_statistics_include_order_and_independent_diagnostics(self):
        ratios = [1.2 if pair % 2 else 1.0 for pair in range(1, 17)]
        cpp, sv = paired_samples(ratios)
        stats = runner.paired_ratio_statistics(cpp, sv)

        self.assertAlmostEqual(stats["ratio"], 1.1)
        self.assertEqual(
            stats["order_stratified_paired_medians"],
            {"cpp_dpi_first": 1.2, "pure_sv_first": 1.0},
        )
        self.assertAlmostEqual(stats["independent_median_ratio"], 1.1)
        self.assertAlmostEqual(stats["relative_disagreement"], 0.0)
        self.assertAlmostEqual(stats["stratum_gap"], 0.2 / 1.1)

    def test_invalid_process_samples_and_odd_counts(self):
        for bad_cpp, bad_sv in (
            (0.0, 1.0),
            (math.nan, 1.0),
            (1.0, 0.0),
            (1.0, math.inf),
        ):
            cpp, sv = paired_samples([1.0] * 16)
            cpp[0]["process_wall_ms"] = bad_cpp
            sv[0]["process_wall_ms"] = bad_sv
            with self.assertRaises(ValueError):
                runner.paired_ratio_statistics(cpp, sv)
        cpp, sv = paired_samples([1.0] * 17)
        with self.assertRaisesRegex(ValueError, "even"):
            runner.paired_ratio_statistics(cpp, sv)

    def test_pair_order_requires_slots_adjacency_and_matching_ids(self):
        cpp, sv = paired_samples([1.0] * 16)
        runner.validate_pair_order(cpp, sv)

        cpp[0]["pair_order"] = ["pure_sv", "cpp_dpi"]
        with self.assertRaisesRegex(ValueError, "order"):
            runner.validate_pair_order(cpp, sv)
        cpp, sv = paired_samples([1.0] * 16)
        cpp[0]["slot"] = 2
        with self.assertRaisesRegex(ValueError, "slots"):
            runner.validate_pair_order(cpp, sv)
        cpp, sv = paired_samples([1.0] * 16)
        sv[0]["sequence_index"] += 3
        with self.assertRaisesRegex(ValueError, "adjacently"):
            runner.validate_pair_order(cpp, sv)
        cpp, sv = paired_samples([1.0] * 16)
        sv[0]["pair"] = 99
        with self.assertRaisesRegex(ValueError, "pair IDs"):
            runner.validate_pair_order(cpp, sv)

    def test_equivalence_mismatch_is_rejected(self):
        cpp, sv = paired_samples([1.0] * 16)
        sv[0]["sim_cycles"] += 1
        with self.assertRaisesRegex(ValueError, "sim_cycles"):
            runner.validate_pair_order(cpp, sv)


class GuardDecisionTests(unittest.TestCase):
    def guard(self, ratios, sv_times=None, final=True):
        cpp, sv = paired_samples(ratios, sv_times)
        return runner.evaluate_guard(cpp, sv, final=final)

    def test_passes_at_hard_limit_and_fails_strictly_above(self):
        self.assertEqual(self.guard([1.10] * 16)["status"], "passed")
        failed = self.guard([1.100001] * 16)
        self.assertEqual(failed["status"], "hard_failure")
        self.assertEqual(failed["verdict"], "failed")
        self.assertEqual(failed["validity"], "valid")
        self.assertTrue(failed["order_strata_confirm_failure"])
        self.assertTrue(failed["independent_median_confirms_failure"])

    def test_crossing_with_one_low_order_stratum_is_invalid(self):
        ratios = [1.0 if pair % 2 else 1.3 for pair in range(1, 17)]
        result = self.guard(ratios)
        self.assertGreater(result["ratio"], 1.10)
        self.assertEqual(result["status"], "invalid_environment")
        self.assertFalse(result["order_strata_confirm_failure"])

    def test_crossing_with_independent_disagreement_is_invalid(self):
        ratios = [1.06, 1.06, 1.40, 1.40] * 4
        sv_times = [1.0, 1.0, 100.0, 100.0] * 4
        result = self.guard(ratios, sv_times)
        self.assertGreater(result["ratio"], 1.10)
        self.assertGreater(result["relative_disagreement"], 0.05)
        self.assertEqual(result["status"], "invalid_environment")

    def test_validity_boundaries_are_strict_for_strata_and_inclusive_for_agreement(self):
        base = {
            "ratio": 1.2,
            "order_stratified_paired_medians": {
                "cpp_dpi_first": 1.050001,
                "pure_sv_first": 1.050001,
            },
            "relative_disagreement": 0.05,
            "one_sided_95_upper_median_bound": {"bound": 1.2},
        }
        with mock.patch.object(runner, "paired_ratio_statistics", return_value=base):
            result = runner.evaluate_guard([{}] * 16, [{}] * 16)
        self.assertEqual(result["status"], "hard_failure")

        at_stratum_boundary = {
            **base,
            "order_stratified_paired_medians": {
                "cpp_dpi_first": 1.05,
                "pure_sv_first": 1.2,
            },
        }
        with mock.patch.object(
            runner, "paired_ratio_statistics", return_value=at_stratum_boundary
        ):
            result = runner.evaluate_guard([{}] * 16, [{}] * 16)
        self.assertEqual(result["status"], "invalid_environment")

    def test_inconclusive_bound_requests_only_nonfinal_extra_batch(self):
        ratios = [1.0] * 10 + [1.2] * 6
        self.assertEqual(self.guard(ratios, final=False)["status"], "needs_extra_batch")
        final = self.guard(ratios, final=True)
        self.assertEqual(final["status"], "passed_inconclusive")
        self.assertIn("warning", final)


class ComparisonTests(unittest.TestCase):
    @staticmethod
    def fake_runner(ratio_for_pair, calls):
        def run(mode, kernel, pair, iterations):
            calls.append((mode, kernel, pair))
            ratio = 1.0 if pair == 0 else ratio_for_pair(kernel, pair)
            return sample(
                mode,
                pair,
                ratio if mode == "cpp_dpi" else 1.0,
                kernel=kernel,
                iterations=iterations,
            )

        return run

    def test_interleaving_is_balanced_deterministic_and_slotted(self):
        calls = []
        fake = self.fake_runner(lambda _kernel, _pair: 1.0, calls)
        summaries, raw = runner.run_comparison(
            ["control", "event"], 1, 16, sample_runner=fake
        )

        self.assertEqual(len(raw), 68)
        self.assertEqual([entry["sequence_index"] for entry in raw], list(range(68)))
        for kernel in ("control", "event"):
            self.assertEqual(summaries[kernel]["guard"]["status"], "passed")
            first_modes = [
                pair["pair_order"][0] for pair in summaries[kernel]["cpp_dpi"]
            ]
            self.assertEqual(first_modes.count("cpp_dpi"), 8)
            self.assertEqual(first_modes.count("pure_sv"), 8)
        measured = [entry for entry in raw if entry["batch"] == "initial"]
        self.assertEqual(
            [(entry["kernel"], entry["mode"], entry["slot"]) for entry in measured[:8]],
            [
                ("control", "cpp_dpi", 1),
                ("control", "pure_sv", 2),
                ("event", "cpp_dpi", 1),
                ("event", "pure_sv", 2),
                ("event", "pure_sv", 1),
                ("event", "cpp_dpi", 2),
                ("control", "pure_sv", 1),
                ("control", "cpp_dpi", 2),
            ],
        )

    def test_collects_exactly_one_fixed_extra_batch(self):
        calls = []
        fake = self.fake_runner(
            lambda _kernel, pair: 1.2 if 11 <= pair <= 16 else 1.0,
            calls,
        )
        summaries, raw = runner.run_comparison(
            ["control"], 1, 16, sample_runner=fake
        )
        guard = summaries["control"]["guard"]
        self.assertTrue(guard["extra_batch_collected"])
        self.assertEqual(guard["status"], "passed")
        self.assertEqual(len(raw), 2 + 2 * (16 + runner.EXTRA_PAIRS))
        self.assertEqual(len(calls), len(raw))

    def test_final_extra_batch_can_remain_inconclusive(self):
        calls = []
        fake = self.fake_runner(
            lambda _kernel, pair: 1.2 if (pair - 1) % 16 >= 10 else 1.0,
            calls,
        )
        summaries, _ = runner.run_comparison(
            ["control"], 1, 16, sample_runner=fake
        )
        self.assertEqual(summaries["control"]["guard"]["status"], "passed_inconclusive")

    def test_extra_batch_can_produce_a_valid_failure(self):
        calls = []
        fake = self.fake_runner(
            lambda _kernel, pair: 1.0 if pair <= 9 else 1.2,
            calls,
        )
        summaries, raw = runner.run_comparison(
            ["control"], 1, 16, sample_runner=fake
        )
        self.assertEqual(summaries["control"]["guard"]["status"], "hard_failure")
        self.assertEqual(len(raw), 66)

    def test_initial_failure_or_invalid_environment_never_collects_extra(self):
        for ratio_for_pair, expected in (
            (lambda _kernel, _pair: 1.2, "hard_failure"),
            (
                lambda _kernel, pair: 1.0 if pair % 2 else 1.3,
                "invalid_environment",
            ),
        ):
            calls = []
            fake = self.fake_runner(ratio_for_pair, calls)
            summaries, raw = runner.run_comparison(
                ["control"], 1, 16, sample_runner=fake
            )
            self.assertEqual(summaries["control"]["guard"]["status"], expected)
            self.assertFalse(summaries["control"]["guard"]["extra_batch_collected"])
            self.assertEqual(len(raw), 34)

    def test_odd_pair_counts_are_rejected_before_sampling(self):
        called = mock.Mock()
        with self.assertRaisesRegex(ValueError, "even"):
            runner.run_comparison(["control"], 1, 17, sample_runner=called)
        called.assert_not_called()
        with self.assertRaises(SystemExit):
            runner._parse_args(["--pairs", "17"])
        self.assertEqual(runner._parse_args([]).pairs, 16)


class DurabilityTests(unittest.TestCase):
    def test_journal_appends_json_lines_and_fsyncs_each_sample(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "samples.jsonl"
            journal = runner.SampleJournal(path)
            with mock.patch.object(runner.os, "fsync") as fsync:
                journal.append({"sequence_index": 0})
                journal.append({"sequence_index": 1})
            entries = [json.loads(line) for line in path.read_text().splitlines()]
        self.assertEqual(entries, [{"sequence_index": 0}, {"sequence_index": 1}])
        self.assertEqual(fsync.call_count, 2)

    def test_partial_pair_is_preserved_in_memory_and_journal(self):
        raw = []
        calls = 0

        def fail_during_second_pair(mode, kernel, pair, iterations):
            nonlocal calls
            calls += 1
            if calls == 4:
                raise RuntimeError("injected sample failure")
            return sample(mode, pair, 1.0, kernel=kernel, iterations=iterations)

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "samples.jsonl"
            with self.assertRaisesRegex(RuntimeError, "injected"):
                runner.collect_batch(
                    ["control"],
                    1,
                    16,
                    1,
                    fail_during_second_pair,
                    raw,
                    runner.SampleJournal(path),
                )
            journaled = [json.loads(line) for line in path.read_text().splitlines()]
        self.assertEqual(len(raw), 3)
        self.assertEqual(journaled, raw)
        self.assertEqual(raw[-1]["sequence_index"], 2)

    def test_atomic_write_uses_sibling_temporary_and_replace(self):
        real_replace = os.replace
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "latest.json"
            replacements = []

            def replace(source, destination):
                source = Path(source)
                destination = Path(destination)
                replacements.append((source, destination, source.read_text()))
                self.assertEqual(source.parent, target.parent)
                real_replace(source, destination)

            runner.atomic_write_json(target, {"status": "success"}, replace=replace)
            self.assertEqual(json.loads(target.read_text()), {"status": "success"})
            self.assertEqual(replacements[0][1], target)
            self.assertFalse(replacements[0][0].exists())

    def test_all_report_statuses_atomically_write_json_and_markdown(self):
        with tempfile.TemporaryDirectory() as directory:
            result_dir = Path(directory)
            base = {
                "iterations": 1,
                "pairs": 16,
                "preflight": {"status": "skipped"},
                "selected_kernels": ["control"],
                "kernels": {},
                "raw_samples": [],
            }
            for status in ("success", "failed", "invalid_environment", "error"):
                result = {**base, "status": status}
                if status == "error":
                    result["error"] = {"type": "RuntimeError", "message": "boom"}
                calls = []
                real_replace = os.replace

                def replace(source, destination):
                    calls.append((Path(source), Path(destination)))
                    real_replace(source, destination)

                with (
                    mock.patch.object(runner, "RESULT_DIR", result_dir),
                    mock.patch.object(runner.os, "replace", side_effect=replace),
                ):
                    runner._write_results(result)
                self.assertEqual(len(calls), 2)
                self.assertEqual(json.loads((result_dir / "latest.json").read_text())["status"], status)
                self.assertIn(f"`{status}`", (result_dir / "latest.md").read_text())

    def test_binary_hashes_are_recorded_on_samples(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "benchmark"
            binary.write_bytes(b"authoring-core-binary")
            expected = hashlib.sha256(binary.read_bytes()).hexdigest()
            with (
                mock.patch.object(runner, "_binary", return_value=binary),
                mock.patch.object(
                    runner,
                    "run_command",
                    return_value=(result_output("cpp_dpi"), 2.5),
                ),
            ):
                measured = runner.run_sample("cpp_dpi", "control", 1, 1)
        self.assertEqual(measured["binary_sha256"], expected)
        self.assertEqual(measured["binary"], str(binary))
        self.assertIsNone(runner.binary_sha256(Path(directory) / "missing"))

    def test_main_persists_metadata_and_partial_samples_on_error(self):
        with tempfile.TemporaryDirectory() as directory:
            result_dir = Path(directory)
            raw_path = result_dir / "latest.jsonl"

            def fail_comparison(*_args, **kwargs):
                entry = {"mode": "cpp_dpi", "sequence_index": 0}
                kwargs["raw_samples"].append(entry)
                kwargs["journal"].append(entry)
                raise RuntimeError("measurement stopped")

            metadata = {"timestamp_utc": "now", "config": {}}
            with (
                mock.patch.object(runner, "RESULT_DIR", result_dir),
                mock.patch.object(runner, "RAW_SAMPLE_PATH", raw_path),
                mock.patch.object(runner, "collect_metadata", return_value=metadata),
                mock.patch.object(runner, "collect_binary_metadata", return_value={}),
                mock.patch.object(runner, "run_comparison", side_effect=fail_comparison),
            ):
                with redirect_stdout(io.StringIO()):
                    returncode = runner.main(
                        [
                            "--skip-build",
                            "--skip-preflight",
                            "--kernels",
                            "control",
                        ]
                    )
            persisted = json.loads((result_dir / "latest.json").read_text())
            journaled = [json.loads(line) for line in raw_path.read_text().splitlines()]

        self.assertEqual(returncode, 1)
        self.assertEqual(persisted["status"], "error")
        self.assertEqual(persisted["metadata"], metadata)
        self.assertEqual(persisted["raw_samples"], journaled)
        self.assertEqual(persisted["error"]["message"], "measurement stopped")

    def test_main_maps_guard_verdicts_to_exit_statuses(self):
        cases = (
            ([1.0] * 16, "success", 0),
            ([1.2] * 16, "failed", 1),
            (
                [1.0 if pair % 2 else 1.3 for pair in range(1, 17)],
                "invalid_environment",
                1,
            ),
        )
        for ratios, expected_status, expected_returncode in cases:
            cpp, sv = paired_samples(ratios)
            guard = runner.evaluate_guard(cpp, sv, final=True)
            guard["extra_batch_collected"] = False
            summary = {
                "control": {
                    "cpp_dpi": cpp,
                    "pure_sv": sv,
                    "guard": guard,
                }
            }
            with tempfile.TemporaryDirectory() as directory:
                result_dir = Path(directory)
                with (
                    mock.patch.object(runner, "RESULT_DIR", result_dir),
                    mock.patch.object(
                        runner, "RAW_SAMPLE_PATH", result_dir / "latest.jsonl"
                    ),
                    mock.patch.object(
                        runner,
                        "collect_metadata",
                        return_value={"config": {}},
                    ),
                    mock.patch.object(
                        runner, "collect_binary_metadata", return_value={}
                    ),
                    mock.patch.object(
                        runner, "run_comparison", return_value=(summary, [])
                    ),
                    redirect_stdout(io.StringIO()),
                ):
                    returncode = runner.main(
                        [
                            "--skip-build",
                            "--skip-preflight",
                            "--kernels",
                            "control",
                        ]
                    )
                persisted = json.loads((result_dir / "latest.json").read_text())
            self.assertEqual(returncode, expected_returncode)
            self.assertEqual(persisted["status"], expected_status)


if __name__ == "__main__":
    unittest.main()
