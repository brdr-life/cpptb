"""Version-comparison contract for tools/check_environment.py.

The parsing is easy to get subtly wrong: Verilator zero-pads its minor field,
so a naive float or string comparison ranks 5.046 above 5.50 or below 5.9.
"""

import importlib.util
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
_SPEC = importlib.util.spec_from_file_location(
    "cpptb_check_environment", REPO / "tools" / "check_environment.py"
)
check_environment = importlib.util.module_from_spec(_SPEC)
# @dataclass resolves annotations through sys.modules[cls.__module__], so the
# module has to be registered before its body runs.
sys.modules[_SPEC.name] = check_environment
_SPEC.loader.exec_module(check_environment)

_version = check_environment._version
_at_least = check_environment._at_least
_show = check_environment._show


class VersionParsingTests(unittest.TestCase):
    def test_parses_verilator_banner_and_keeps_spelling(self):
        parsed = _version("Verilator 5.050 2026-07-01 rev v5.050")
        self.assertIsNotNone(parsed)
        fields, literal = parsed
        self.assertEqual(fields, (5, 50))
        # The report must echo the spelling upstream publishes.
        self.assertEqual(literal, "5.050")

    def test_parses_common_tool_banners(self):
        cases = {
            "cmake version 3.31.6": (3, 31, 6),
            "Python 3.12.3": (3, 12, 3),
            "uv 0.11.32 (x86_64-unknown-linux-gnu)": (0, 11, 32),
            "4.8.12.0": (4, 8, 12, 0),
        }
        for text, expected in cases.items():
            with self.subTest(text=text):
                self.assertEqual(_version(text)[0], expected)

    def test_returns_none_without_a_version(self):
        self.assertIsNone(_version("command not found"))


class ComparisonTests(unittest.TestCase):
    def test_zero_padded_minor_compares_numerically(self):
        # 5.046 is the floor; 5.050 satisfies it, 5.044 does not.
        floor = check_environment.MIN_VERILATOR
        self.assertTrue(_at_least(_version("5.050")[0], floor))
        self.assertTrue(_at_least(_version("5.046")[0], floor))
        self.assertFalse(_at_least(_version("5.044")[0], floor))
        self.assertFalse(_at_least(_version("5.020")[0], floor))

    def test_shorter_and_longer_tuples_compare(self):
        self.assertTrue(_at_least((3, 20), (3, 20)))
        self.assertTrue(_at_least((3, 20, 1), (3, 20)))
        self.assertFalse(_at_least((3, 19, 9), (3, 20)))
        # A trailing zero must not make an equal version look newer or older.
        self.assertTrue(_at_least((4, 15, 5, 0), (4, 15, 5)))

    def test_distro_z3_is_rejected_against_the_floor(self):
        floor = check_environment.MIN_Z3
        self.assertFalse(_at_least(_version("4.8.12.0")[0], floor))
        self.assertFalse(_at_least(_version("4.15.4")[0], floor))
        self.assertTrue(_at_least(_version("4.15.5")[0], floor))
        self.assertTrue(_at_least(_version("4.16.0")[0], floor))

    def test_minimums_render_the_way_upstream_spells_them(self):
        self.assertEqual(_show(check_environment.MIN_VERILATOR), "5.046")
        self.assertEqual(_show(check_environment.MIN_Z3), "4.15.5")
        self.assertEqual(_show(check_environment.MIN_CMAKE), "3.20")


class ProbeTests(unittest.TestCase):
    def test_cxx20_probe_passes_on_a_supported_toolchain(self):
        # The suite itself only builds under a C++20 compiler, so this must
        # agree; a false negative here would send users chasing a real
        # compiler that is already fine.
        self.assertEqual(check_environment.check_cxx20().status,
                         check_environment.OK)

    def test_collect_reports_every_component_once(self):
        results = check_environment.collect()
        names = [item.name for item in results]
        self.assertEqual(len(names), len(set(names)))
        self.assertIn("Verilator", names)
        self.assertTrue(any(item.required for item in results))
        self.assertTrue(any(not item.required for item in results))


if __name__ == "__main__":
    unittest.main()
