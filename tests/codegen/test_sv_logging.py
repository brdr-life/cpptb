import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class SystemVerilogLoggingTests(unittest.TestCase):
    def test_off_level_never_crosses_dpi_boundary(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            work = Path(temp_dir)
            top = work / "sv_logging_off_test.sv"
            bridge = work / "reject_log_bridge.cpp"
            object_dir = work / "obj"
            top.write_text(
                """
`include "cpptb/sv/cpptb_log.svh"

module sv_logging_off_test;
  int unsigned message_factories = 0;

  function automatic string message_factory();
    ++message_factories;
    return "disabled message";
  endfunction

  initial begin
    cpptb_log_pkg::configure();
    `cpptb_info(message_factory(), "disabled")
    if (message_factories != 0) begin
      $fatal(1, "disabled logging evaluated its message factory");
    end
    $display("CPPTB_SV_LOGGING_OFF_PASS dpi_calls=0 factories=0");
    $finish;
  end
endmodule
""".lstrip(),
                encoding="utf-8",
            )
            bridge.write_text(
                """
#include <cstdlib>

extern "C" unsigned int cpptb_sv_log_minimum_level() noexcept {
    return 5;
}

extern "C" void cpptb_sv_log(unsigned int, const char*, const char*,
                              const char*, unsigned int, const char*,
                              unsigned long long) noexcept {
    std::abort();
}
""".lstrip(),
                encoding="utf-8",
            )

            build = subprocess.run(
                [
                    "verilator",
                    "--binary",
                    "--timing",
                    "-Wno-fatal",
                    "-Wno-TIMESCALEMOD",
                    f"-I{ROOT / 'include'}",
                    "--Mdir",
                    str(object_dir),
                    "--top-module",
                    "sv_logging_off_test",
                    str(ROOT / "include/cpptb/sv/cpptb_log_pkg.sv"),
                    str(top),
                    str(bridge),
                ],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(
                build.returncode,
                0,
                msg=f"{build.stdout}\n{build.stderr}",
            )

            completed = subprocess.run(
                [str(object_dir / "Vsv_logging_off_test")],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(
                completed.returncode,
                0,
                msg=f"{completed.stdout}\n{completed.stderr}",
            )
            self.assertIn(
                "CPPTB_SV_LOGGING_OFF_PASS dpi_calls=0 factories=0",
                completed.stdout,
            )


if __name__ == "__main__":
    unittest.main()
