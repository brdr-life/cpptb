import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which("c++") and shutil.which("verilator"),
                     "C++ and Verilator are required")
class PortForceDiagnosticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.verilator_root = subprocess.run(
            ["verilator", "--getenv", "VERILATOR_ROOT"],
            check=True,
            text=True,
            capture_output=True,
        ).stdout.strip()

    def compile_misuse(self, operation):
        source = f"""
#include "cpptb/dpi_static_binding.hpp"

using ClockPort = cpptb::dpi::StaticPackedSignal<1, true, false, 0, 0>;

void misuse(ClockPort clk) {{
    clk.{operation};
}}
"""
        with tempfile.TemporaryDirectory() as temp_dir:
            source_path = Path(temp_dir) / "port_force_misuse.cpp"
            source_path.write_text(source, encoding="utf-8")
            return subprocess.run(
                [
                    "c++",
                    "-std=c++20",
                    "-fsyntax-only",
                    f"-I{REPO / 'include'}",
                    f"-I{Path(self.verilator_root) / 'include' / 'vltstd'}",
                    str(source_path),
                ],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )

    def test_force_on_scheduler_owned_clock_has_actionable_error(self):
        result = self.compile_misuse("force(0)")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "force() is not supported on ordinary DUT ports",
            result.stdout,
        )
        self.assertIn(
            "Scheduler-owned clocks must remain controlled by "
            "TestContext::start_clock()",
            result.stdout,
        )
        self.assertIn(
            "coherent clock pause/override is not yet supported",
            result.stdout,
        )

    def test_release_on_scheduler_owned_clock_has_actionable_error(self):
        result = self.compile_misuse("release()")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "release() is not supported on ordinary DUT ports",
            result.stdout,
        )
        self.assertIn(
            "Scheduler-owned clocks must remain controlled by "
            "TestContext::start_clock()",
            result.stdout,
        )


if __name__ == "__main__":
    unittest.main()
