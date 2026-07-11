import shutil
import tempfile
import unittest
from pathlib import Path

from cpptb.codegen.design_ir import CodegenError
from cpptb.codegen.frontends.slang import SlangFrontend
from cpptb.codegen.frontends.verilator_json import VerilatorJsonFrontend


class FrontendTests(unittest.TestCase):
    def elaborate(self, source: str, parameters=None):
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        base_dir = Path(temp_dir.name)
        (base_dir / "sample.sv").write_text(source)
        manifest = {
            "module": "sample",
            "sources": ["sample.sv"],
            "parameters": parameters or {},
            "frontend_options": {"slang": {"standard": "1800-2023"}},
        }
        return SlangFrontend().elaborate(manifest, base_dir), manifest, base_dir

    def test_slang_returns_resolved_typed_ports(self):
        design, _, _ = self.elaborate(
            """
module sample #(parameter int WIDTH = 4) (
  input logic signed [WIDTH-1:0] data_i,
  output bit [WIDTH-1:0] data_o
);
endmodule
""",
            {"WIDTH": 12},
        )

        self.assertEqual(
            design.transport_signature(),
            (("data_i", "input", 12), ("data_o", "output", 12)),
        )
        self.assertTrue(design.ports[0].signed)
        self.assertTrue(design.ports[0].four_state)
        self.assertFalse(design.ports[1].four_state)

    def test_slang_reports_source_diagnostics(self):
        with self.assertRaisesRegex(CodegenError, "Slang could not elaborate"):
            self.elaborate("module sample(input logic broken; endmodule")

    @unittest.skipUnless(shutil.which("verilator"), "Verilator is not installed")
    def test_slang_matches_verilator_transport_contract(self):
        design, manifest, base_dir = self.elaborate(
            """
module sample #(parameter int WIDTH = 4) (
  input logic [WIDTH-1:0] data_i,
  output logic [WIDTH-1:0] data_o
);
  assign data_o = data_i;
endmodule
""",
            {"WIDTH": 9},
        )
        verilator_design = VerilatorJsonFrontend().elaborate(manifest, base_dir)

        self.assertEqual(
            design.transport_signature(), verilator_design.transport_signature()
        )


if __name__ == "__main__":
    unittest.main()
