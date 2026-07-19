import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from cpptb_codegen.verilator_capabilities import probe_verilator_four_state


@unittest.skipUnless(shutil.which("verilator"), "Verilator is required")
class VerilatorCapabilityProbeTests(unittest.TestCase):
    def test_four_state_semantics_remain_upstream_blocked(self):
        version = subprocess.run(
            ["verilator", "--version"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=True,
        ).stdout.strip()
        with tempfile.TemporaryDirectory() as temp_dir:
            work_dir = Path(temp_dir)
            result = probe_verilator_four_state(
                ("verilator",), version, work_dir
            )

            self.assertFalse(
                result.supported,
                "Verilator's four-state semantic probe now passes. Enable "
                "the CPPTB experimental transport, add end-to-end X/Z "
                "conformance tests, and revise the blocked-feature docs.",
            )
            self.assertIn("SystemVerilog X/Z storage:", result.summary())
            self.assertIn("DPI round trip:", result.summary())

            cached = probe_verilator_four_state(
                ("verilator",), version, work_dir
            )
            self.assertEqual(cached, result)


if __name__ == "__main__":
    unittest.main()
