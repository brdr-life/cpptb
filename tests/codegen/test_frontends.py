import shutil
import tempfile
import unittest
from pathlib import Path

from cpptb_codegen.design_ir import (
    CodegenError,
    PackedEnumType,
    PackedIntegralType,
    PackedRange,
    PackedStructType,
    PackedUnionType,
    UnpackedRange,
)
from cpptb_codegen.generate_dpi_bindings import validate_transport_ports
from cpptb_codegen.frontends.slang import SlangFrontend, infer_top_module
from cpptb_codegen.frontends.verilator_json import VerilatorJsonFrontend, parse_range


class FrontendTests(unittest.TestCase):
    RICH_PACKED_SOURCE = """
typedef enum logic signed [2:0] {
  NEGATIVE = -1,
  IDLE = 0,
  RUN = 3
} state_t;

typedef struct packed {
  bit [1:0] tag;
  logic [4:2] payload;
} inner_t;

typedef struct packed {
  logic [3:1] opcode;
  state_t state;
  inner_t inner;
} packet_t;

module sample(input packet_t packet_i, input state_t state_i);
endmodule
"""

    def test_verilator_range_parser_requires_balanced_brackets(self):
        self.assertEqual(parse_range("[7:4]", "array_i"), UnpackedRange(7, 4))
        self.assertEqual(parse_range("7:4", "array_i"), UnpackedRange(7, 4))
        for malformed in ("[7:4", "7:4]"):
            with self.subTest(malformed=malformed):
                with self.assertRaisesRegex(CodegenError, "unsupported"):
                    parse_range(malformed, "array_i")

    def elaborate(self, source: str, parameters=None, internals=None):
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        base_dir = Path(temp_dir.name)
        (base_dir / "sample.sv").write_text(source)
        manifest = {
            "module": "sample",
            "sources": ["sample.sv"],
            "parameters": parameters or {},
            "internals": internals or [],
            "frontend_options": {"slang": {"standard": "1800-2023"}},
        }
        return SlangFrontend().elaborate(manifest, base_dir), manifest, base_dir

    def test_slang_resolves_internal_variables_nets_and_memories(self):
        design, _, _ = self.elaborate(
            """
module sample(input logic clk);
  logic [6:0] state;
  wire [8:0] observed = {2'b0, state};
  bit [72:0] memory [7:4];
endmodule
""",
            internals=[
                {"path": "state", "access": "read_write", "force": True},
                {"path": "observed", "name": "debug.observed", "force": True},
                {"path": "memory", "access": "read_write", "force": True},
            ],
        )

        state, observed, memory = design.internals
        self.assertEqual(state.cpp_path, ("internal", "state"))
        self.assertEqual(state.symbol_kind, "variable")
        self.assertEqual(state.width, 7)
        self.assertTrue(state.writable)
        self.assertTrue(state.forceable)
        self.assertEqual(observed.cpp_path, ("internal", "debug", "observed"))
        self.assertEqual(observed.symbol_kind, "net")
        self.assertFalse(observed.writable)
        self.assertTrue(observed.forceable)
        self.assertEqual(memory.width, 73)
        self.assertEqual(memory.unpacked, (UnpackedRange(7, 4),))
        self.assertTrue(memory.forceable)

    def test_slang_catalogs_complete_elaborated_hierarchy_without_manifest(self):
        fixture_dir = Path(__file__).parent / "fixtures"
        design = SlangFrontend().elaborate(
            {
                "module": "hierarchy_catalog",
                "sources": ["hierarchy_catalog.sv"],
                "frontend_options": {
                    "slang": {"standard": "1800-2023"}
                },
            },
            fixture_dir,
        )

        scopes = {scope.hdl_path: scope for scope in design.hierarchy.scopes}
        self.assertEqual(scopes["block1"].symbol_kind, "instance")
        self.assertEqual(scopes["lanes"].symbol_kind, "generate_array")
        self.assertEqual(scopes["lanes[0]"].symbol_kind, "generate_block")
        self.assertEqual(
            scopes["lanes[1].block2"].cpp_path,
            ("lanes[1]", "block2"),
        )

        signals = {
            signal.hdl_path: signal for signal in design.hierarchy.signals
        }
        self.assertNotIn("clk", signals)
        self.assertEqual(signals["block1.storage"].symbol_kind, "variable")
        self.assertTrue(signals["block1.storage"].depositable)
        self.assertEqual(signals["block1.inverted"].symbol_kind, "net")
        self.assertFalse(signals["block1.inverted"].depositable)
        self.assertEqual(
            signals["block1.memory"].unpacked,
            (UnpackedRange(2, 5),),
        )
        self.assertEqual(
            signals["block1.matrix"].unpacked,
            (UnpackedRange(1, 0), UnpackedRange(4, 2)),
        )
        self.assertIsInstance(
            signals["block1.state"].packed_type, PackedEnumType
        )
        self.assertIsInstance(
            signals["block1.packet"].packed_type, PackedStructType
        )
        self.assertIn("lanes[1].block2.storage", signals)

        parameters = {
            parameter.hdl_path: parameter
            for parameter in design.hierarchy.parameters
        }
        self.assertEqual(parameters["block1.WIDTH"].value, 8)
        self.assertEqual(parameters["block1.DOUBLE_WIDTH"].value, 16)
        self.assertTrue(parameters["block1.DOUBLE_WIDTH"].local)
        self.assertEqual(parameters["lanes[1].index"].value, 1)

    def test_slang_rejects_invalid_internal_configuration(self):
        source = """
module sample(input logic clk);
  wire observed = clk;
endmodule
"""
        with self.assertRaisesRegex(CodegenError, "was not found"):
            self.elaborate(source, internals=[{"path": "missing"}])
        with self.assertRaisesRegex(CodegenError, "requires a variable"):
            self.elaborate(
                source,
                internals=[{"path": "observed", "access": "read_write"}],
            )
        with self.assertRaisesRegex(CodegenError, "unsupported access"):
            self.elaborate(
                source, internals=[{"path": "observed", "access": "force"}]
            )
        with self.assertRaisesRegex(CodegenError, "force must be a boolean"):
            self.elaborate(
                source, internals=[{"path": "observed", "force": "yes"}]
            )

    def test_verilator_frontend_rejects_internal_resolution(self):
        with self.assertRaisesRegex(
            CodegenError, "cannot resolve configured internals"
        ):
            VerilatorJsonFrontend().elaborate(
                {"module": "sample", "sources": [], "internals": [{"path": "x"}]},
                Path("."),
            )

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
            (
                ("data_i", "input", 12, True, True, ()),
                ("data_o", "output", 12, False, False, ()),
            ),
        )
        self.assertTrue(design.ports[0].signed)
        self.assertTrue(design.ports[0].four_state)
        self.assertFalse(design.ports[1].four_state)

    def test_slang_preserves_enum_values_and_struct_field_offsets(self):
        design, _, _ = self.elaborate(self.RICH_PACKED_SOURCE)
        packet = design.ports[0].packed_type
        state = design.ports[1].packed_type

        self.assertIsInstance(packet, PackedStructType)
        self.assertEqual(packet.width, 11)
        self.assertEqual(
            [(field.name, field.bit_offset, field.width) for field in packet.fields],
            [("opcode", 8, 3), ("state", 5, 3), ("inner", 0, 5)],
        )
        opcode = packet.fields[0].data_type
        self.assertIsInstance(opcode, PackedIntegralType)
        self.assertEqual(opcode.ranges, (PackedRange(3, 1),))

        nested_state = packet.fields[1].data_type
        self.assertIsInstance(nested_state, PackedEnumType)
        self.assertEqual(
            [(value.name, value.value) for value in nested_state.values],
            [("NEGATIVE", -1), ("IDLE", 0), ("RUN", 3)],
        )
        self.assertTrue(nested_state.signed)
        self.assertEqual(nested_state.base.ranges, (PackedRange(2, 0),))
        self.assertEqual(
            nested_state.structural_signature(), state.structural_signature()
        )

        inner = packet.fields[2].data_type
        self.assertIsInstance(inner, PackedStructType)
        self.assertEqual(
            [(field.name, field.bit_offset, field.width) for field in inner.fields],
            [("tag", 3, 2), ("payload", 0, 3)],
        )

    def test_slang_represents_union_for_explicit_transport_rejection(self):
        design, manifest, base_dir = self.elaborate(
            """
typedef union packed {
  logic [7:0] raw;
  struct packed { logic [2:0] low; logic [4:0] high; } fields;
} overlay_t;
module sample(input overlay_t overlay_i);
endmodule
"""
        )
        overlay = design.ports[0]
        self.assertEqual(overlay.type_kind, "packed_union")
        self.assertIsInstance(overlay.packed_type, PackedUnionType)
        self.assertEqual(
            [
                (field.name, field.bit_offset, field.width)
                for field in overlay.packed_type.fields
            ],
            [("raw", 0, 8), ("fields", 0, 8)],
        )
        with self.assertRaisesRegex(CodegenError, "packed_union"):
            validate_transport_ports([overlay])
        if shutil.which("verilator"):
            verilator_design = VerilatorJsonFrontend().elaborate(manifest, base_dir)
            self.assertEqual(
                design.transport_signature(),
                verilator_design.transport_signature(),
            )

    def test_slang_reports_source_diagnostics(self):
        with self.assertRaisesRegex(CodegenError, "Slang could not elaborate"):
            self.elaborate("module sample(input logic broken; endmodule")

    def test_slang_infers_one_top_and_diagnoses_ambiguity(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base_dir = Path(temp_dir)
            source = base_dir / "design.sv"
            source.write_text(
                "module leaf(input logic value); endmodule\n"
                "module selected(input logic value); leaf child(value); endmodule\n"
            )
            inference = {
                "sources": ["design.sv"],
                "frontend_options": {
                    "slang": {"standard": "1800-2023"}
                },
            }
            self.assertEqual(
                infer_top_module(inference, base_dir), "selected"
            )

            source.write_text(
                "module first(input logic value); endmodule\n"
                "module second(input logic value); endmodule\n"
            )
            with self.assertRaisesRegex(
                CodegenError, "multiple top-level modules.*first, second.*--top"
            ):
                infer_top_module(inference, base_dir)

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

    @unittest.skipUnless(shutil.which("verilator"), "Verilator is not installed")
    def test_frontends_agree_on_wide_two_state_port(self):
        design, manifest, base_dir = self.elaborate(
            """
module sample (
  input bit [136:0] data_i,
  output bit [64:0] data_o
);
  assign data_o = data_i[64:0];
endmodule
"""
        )
        verilator_design = VerilatorJsonFrontend().elaborate(manifest, base_dir)

        self.assertFalse(design.ports[0].four_state)
        self.assertFalse(design.ports[1].four_state)
        self.assertEqual(
            design.transport_signature(), verilator_design.transport_signature()
        )

    @unittest.skipUnless(shutil.which("verilator"), "Verilator is not installed")
    def test_frontends_agree_on_fixed_unpacked_ranges(self):
        design, manifest, base_dir = self.elaborate(
            """
module sample (
  input bit [63:0] descending_i [7:4],
  output logic [15:0] ascending_o [0:3],
  input logic [7:0] nonzero_i [4:11]
);
endmodule
"""
        )
        verilator_design = VerilatorJsonFrontend().elaborate(manifest, base_dir)

        self.assertEqual(
            design.transport_signature(), verilator_design.transport_signature()
        )
        self.assertEqual(design.ports[0].unpacked, (UnpackedRange(7, 4),))
        self.assertEqual(design.ports[1].unpacked, (UnpackedRange(0, 3),))
        self.assertEqual(design.ports[2].unpacked, (UnpackedRange(4, 11),))
        validate_transport_ports(list(design.ports))

    @unittest.skipUnless(shutil.which("verilator"), "Verilator is not installed")
    def test_frontends_agree_on_enum_and_nested_struct_layout(self):
        design, manifest, base_dir = self.elaborate(self.RICH_PACKED_SOURCE)
        verilator_design = VerilatorJsonFrontend().elaborate(manifest, base_dir)

        self.assertEqual(
            design.transport_signature(), verilator_design.transport_signature()
        )
        self.assertEqual(
            design.ports[0].packed_type.structural_signature(),
            verilator_design.ports[0].packed_type.structural_signature(),
        )

    @unittest.skipUnless(shutil.which("verilator"), "Verilator is not installed")
    def test_frontends_agree_on_multidimensional_unpacked_ports(self):
        design, manifest, base_dir = self.elaborate(
            """
module sample(input logic [7:0] matrix_i [1:0] [4:2] [-1:1]);
endmodule
"""
        )
        manifest["codegen"] = {"static_binding": True}
        verilator_design = VerilatorJsonFrontend().elaborate(manifest, base_dir)
        self.assertEqual(
            design.transport_signature(), verilator_design.transport_signature()
        )
        self.assertEqual(
            design.ports[0].unpacked,
            (UnpackedRange(1, 0), UnpackedRange(4, 2), UnpackedRange(-1, 1)),
        )
        validate_transport_ports(list(design.ports))


if __name__ == "__main__":
    unittest.main()
