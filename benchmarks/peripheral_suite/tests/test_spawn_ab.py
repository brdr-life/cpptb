import importlib.util
import unittest
from pathlib import Path
from unittest import mock


TOOL_PATH = Path(__file__).resolve().parents[1] / "tools" / "run_spawn_ab.py"
SPEC = importlib.util.spec_from_file_location("spawn_ab", TOOL_PATH)
SPAWN_AB = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SPAWN_AB)


class SpawnABTests(unittest.TestCase):
    @staticmethod
    def sample_runner(calls):
        def run(run_id, iters, spawn_mode):
            calls.append((run_id, iters, spawn_mode))
            multiplier = 0.9 if spawn_mode == "detached" else 1.0
            return {
                "run": run_id,
                "spawn_mode": spawn_mode,
                "process_wall_ms": 100.0 * multiplier,
                "internal_wall_ms": 90.0 * multiplier,
                "checks": 20,
                "sim_cycles": 10,
                "failures": 0,
            }

        return run

    def test_warms_then_collects_adjacent_alternating_pairs(self):
        calls = []
        tracked, detached = SPAWN_AB.collect_spawn_pairs(
            10_000, 15, sample_runner=self.sample_runner(calls)
        )
        self.assertEqual(
            calls[:8],
            [
                (0, 10_000, "tracked"),
                (0, 10_000, "detached"),
                (1, 10_000, "tracked"),
                (1, 10_000, "detached"),
                (2, 10_000, "detached"),
                (2, 10_000, "tracked"),
                (3, 10_000, "tracked"),
                (3, 10_000, "detached"),
            ],
        )
        self.assertEqual(len(tracked), 15)
        self.assertEqual(len(detached), 15)
        self.assertEqual(tracked[0]["pair_order"], ["tracked", "detached"])
        self.assertEqual(tracked[1]["pair_order"], ["detached", "tracked"])

    def test_result_stores_raw_samples_and_uncertainty(self):
        calls = []
        tracked, detached = SPAWN_AB.collect_spawn_pairs(
            10_000, 15, sample_runner=self.sample_runner(calls)
        )
        with mock.patch.object(
            SPAWN_AB.benchmark,
            "collect_metadata",
            return_value={"git": {"commit": "abc", "dirty": False}},
        ):
            result = SPAWN_AB.make_result(
                10_000, 15, tracked, detached, argv=["run_spawn_ab.py"]
            )

        self.assertEqual(result["tracked"]["samples"], tracked)
        self.assertEqual(result["detached"]["samples"], detached)
        self.assertAlmostEqual(result["comparison"]["ratio"], 0.9)
        self.assertEqual(result["comparison"]["direction"], "detached_faster")
        self.assertIn(
            "one_sided_95_upper_median_bound", result["comparison"]
        )
        self.assertIn("two_sided_95_median_ci", result["comparison"])
        self.assertIn("uncertainty", result["comparison"])

    def test_rejects_too_few_pairs(self):
        with self.assertRaisesRegex(SystemExit, "at least 15"):
            SPAWN_AB.collect_spawn_pairs(10_000, 14, sample_runner=lambda *args: {})

    def test_defaults_are_reproducible(self):
        args = SPAWN_AB._parse_args([])
        self.assertEqual(args.iters, 10_000)
        self.assertEqual(args.runs, 15)
        self.assertEqual(args.output, SPAWN_AB.DEFAULT_OUTPUT)


if __name__ == "__main__":
    unittest.main()
