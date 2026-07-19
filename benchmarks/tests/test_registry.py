from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from benchmarks import registry
from benchmarks.authoring_core.workload import KERNELS


EXPECTED_NAMES = (
    *KERNELS,
    "dpi_counter",
    "dpi_multiclock",
    "dpi_timer_only",
    "dpi_fifo_scoreboard",
    "dpi_component_fifo",
    "dpi_apb_regfile",
    "dpi_watchdog_timeout",
    "dpi_fault_injection",
    "dpi_rich_data",
    "peripheral_suite",
)

EXAMPLE_NAMES = (
    "dpi_counter",
    "dpi_multiclock",
    "dpi_timer_only",
    "dpi_fifo_scoreboard",
    "dpi_component_fifo",
    "dpi_apb_regfile",
    "dpi_watchdog_timeout",
    "dpi_fault_injection",
    "dpi_rich_data",
)


class RegistryTests(unittest.TestCase):
    def test_names_are_unique_and_ordered(self) -> None:
        names = tuple(entry.name for entry in registry.BENCHMARKS)
        self.assertEqual(names, EXPECTED_NAMES)
        self.assertEqual(len(names), len(set(names)))

    def test_entries_have_complete_metadata_and_known_policies(self) -> None:
        policies = set(registry.GatePolicy)
        for entry in registry.BENCHMARKS:
            with self.subTest(entry=entry.name):
                self.assertTrue(entry.label)
                self.assertTrue(entry.build_targets)
                self.assertTrue(entry.binaries)
                self.assertTrue(entry.runner.commands)
                self.assertGreater(entry.default_iterations, 0)
                self.assertIn(entry.gate_policy, policies)
                self.assertEqual(
                    entry.binary_paths, tuple(binary.path for binary in entry.binaries)
                )

        for name in EXAMPLE_NAMES:
            with self.subTest(example=name):
                entry = registry.get_benchmark(name)
                self.assertEqual(
                    entry.gate_policy,
                    registry.GatePolicy.EQUIVALENCE_ONLY,
                )
                self.assertEqual(
                    tuple(binary.adapter for binary in entry.binaries),
                    ("cpp_dpi", "pure_sv"),
                )
                self.assertIsNone(entry.runner.iterations_environment)
        self.assertEqual(
            registry.get_benchmark("peripheral_suite").gate_policy,
            registry.GatePolicy.DIAGNOSTIC,
        )
        self.assertEqual(
            registry.get_benchmark("peripheral_suite").runner.semantic_build_targets,
            ("peripheral-suite-dpi-build", "peripheral-suite-sv-build"),
        )
        authoring = registry.list_benchmarks(
            category=registry.Category.AUTHORING_FEATURE
        )
        self.assertEqual(
            tuple(
                entry.name
                for entry in authoring
                if entry.gate_policy is registry.GatePolicy.WAIVED_HARD_1_10
            ),
            ("force_direct",),
        )
        self.assertEqual(
            tuple(
                entry.name
                for entry in authoring
                if entry.gate_policy is registry.GatePolicy.DIAGNOSTIC
            ),
            ("dynamic_spawn", "dynamic_task", "register_user_effects"),
        )
        self.assertTrue(
            all(
                entry.gate_policy is registry.GatePolicy.HARD_1_10
                for entry in authoring
                if entry.name not in {
                    "force_direct",
                    "dynamic_spawn",
                    "dynamic_task",
                    "register_user_effects",
                }
            )
        )
        self.assertEqual(
            registry.get_benchmark("test_lifecycle").default_iterations,
            5_000_000,
        )
        self.assertEqual(
            registry.get_benchmark("register_user_effects").default_iterations,
            10_000_000,
        )
        for name in (
            "dynamic_task",
            "dynamic_spawn_scheduler",
            "dynamic_spawn",
            "dynamic_spawn_suspending",
        ):
            with self.subTest(name=name):
                self.assertEqual(
                    registry.get_benchmark(name).default_iterations,
                    5_000_000,
                )
        self.assertEqual(
            registry.get_benchmark("dynamic_monitor").default_iterations,
            100_000,
        )
        self.assertEqual(
            registry.get_benchmark("process_pipeline").gate_policy,
            registry.GatePolicy.HARD_1_10,
        )
        self.assertEqual(
            registry.get_benchmark("process_pipeline").default_iterations,
            100_000,
        )
        waiver = registry.get_benchmark("force_direct").waiver
        self.assertIsNotNone(waiver)
        self.assertGreater(waiver.max_ratio, 1.10)
        self.assertTrue(waiver.approved_on)
        self.assertTrue(waiver.rationale)

    def test_lookup_and_filtered_listing(self) -> None:
        control = registry.get_benchmark("control")
        self.assertIs(control, registry.BENCHMARKS[0])
        self.assertEqual(
            tuple(
                entry.name
                for entry in registry.list_benchmarks(category="integration")
            ),
            (
                "dpi_counter",
                "dpi_multiclock",
                "dpi_timer_only",
                "dpi_fifo_scoreboard",
                "dpi_component_fifo",
                "dpi_apb_regfile",
                "dpi_watchdog_timeout",
                "dpi_fault_injection",
                "dpi_rich_data",
                "peripheral_suite",
            ),
        )
        self.assertEqual(
            tuple(
                entry.name
                for entry in registry.list_benchmarks(gate_policy="equivalence_only")
            ),
            EXAMPLE_NAMES,
        )
        with self.assertRaises(KeyError):
            registry.get_benchmark("missing")

    def test_authoring_build_targets_select_one_dpi_binary(self) -> None:
        for entry in registry.list_benchmarks(
            category=registry.Category.AUTHORING_FEATURE
        ):
            with self.subTest(entry=entry.name):
                self.assertEqual(entry.build_targets[0], entry.binaries[0].path)
                expected_sv_target = (
                    "authoring-core-force-direct-sv-build"
                    if entry.name == "force_direct"
                    else "authoring-core-sv-build"
                )
                self.assertEqual(entry.build_targets[1], expected_sv_target)

    def test_repository_contract_is_consistent(self) -> None:
        self.assertEqual(registry.consistency_errors(), ())
        self.assertIsNone(registry.check_consistency())

    def test_mismatch_detection_covers_all_sources(self) -> None:
        makefile = registry.DEFAULT_MAKEFILE.read_text()
        errors = registry.consistency_errors(
            workload_kernels=(*KERNELS[:-1], "different"),
            makefile_text=makefile.replace(
                "AUTHORING_CORE_KERNELS := control",
                "AUTHORING_CORE_KERNELS := changed",
            ).replace(
                "AUTHORING_CORE_DPI_template,control,0",
                "AUTHORING_CORE_DPI_template,control,99",
            ),
        )
        self.assertEqual(len(errors), 3)
        self.assertIn("workload.KERNELS", errors[0])
        self.assertIn("AUTHORING_CORE_KERNELS", errors[1])
        self.assertIn("template IDs", errors[2])
        with self.assertRaises(registry.RegistryConsistencyError):
            registry.check_consistency(
                workload_kernels=("control",), makefile_text=makefile
            )

    def test_import_does_not_execute_subprocess(self) -> None:
        with mock.patch.object(subprocess, "run", side_effect=AssertionError), mock.patch.object(
            subprocess, "check_output", side_effect=AssertionError
        ), mock.patch.object(subprocess, "Popen", side_effect=AssertionError):
            sys.modules.pop("benchmarks.registry", None)
            __import__("benchmarks.registry")


if __name__ == "__main__":
    unittest.main()
