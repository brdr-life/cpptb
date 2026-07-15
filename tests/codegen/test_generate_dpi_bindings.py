import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from cpptb_codegen.design_ir import (
    DesignIR,
    Internal,
    PackedEnumType,
    PackedEnumValue,
    PackedField,
    PackedIntegralType,
    PackedRange,
    PackedStructType,
    UnpackedRange,
)
from cpptb_codegen.generate_dpi_bindings import (
    CodegenError,
    Port,
    apply_port_transport,
    build_tree,
    compare_designs,
    discover_ports,
    map_ports,
    main as codegen_main,
    render_cpp_binding,
    render_cpp_adapter,
    render_cpp_dut,
    render_sv,
    load_access_plan,
    load_discovered_clocks,
    parse_clock_argument,
    source_manifest,
    time_literal_femtoseconds,
    validate_clock_ports,
    validate_internals,
    validate_transport_ports,
    write_or_check,
)


def manifest():
    return {
        "module": "sample_dut",
        "top_module": "dpi_sample_dut",
        "namespace": "cpptb::sample",
        "root_type": "SampleDut",
        "clock": {"port": "clk"},
        "parameters": {},
        "path_rules": [
            {
                "prefix": "bus_",
                "path": "bus",
                "groups": [{"path": "apb", "members": ["PADDR", "PWDATA"]}],
            }
        ],
        "type_names": {"": "SampleDut", "bus": "BusDut", "bus.apb": "ApbBus"},
        "outputs": {
            "cpp_include": "generated/sample_dut.hpp",
        },
    }


class CodegenTests(unittest.TestCase):
    def test_access_plan_loads_and_deduplicates_port_edges(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "access.json"
            path.write_text(
                '{"schema_version": 1, "accesses": [], '
                '"port_edges": [4, 0, 4]}\n'
            )
            hierarchy, port_edges = load_access_plan(path)

        self.assertEqual(hierarchy, [])
        self.assertEqual(port_edges, [0, 4])

    def test_discovered_clocks_render_as_writable_literal_drivers(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "clocks.json"
            path.write_text(
                '{"schema_version": 1, "clocks": ['
                '{"port": "clk", "period_fs": 10000000, '
                '"phase_fs": 1000000, "primary": true}]}\n'
            )
            clocks = load_discovered_clocks(path)
            config = source_manifest(
                [Path(temp_dir) / "counter.sv"],
                "counter",
                output_dir=Path(temp_dir) / "generated",
                clocks=clocks,
                dynamic_clocks=False,
            )
            ports = map_ports(
                [Port("clk", "input", 1), Port("count", "output", 8)],
                config,
            )
            tree = build_tree(ports)
            header = render_cpp_dut(ports, [], tree, config, "counter.sv")
            binding = render_cpp_binding(
                ports, [], tree, config, "counter.sv"
            )
            wrapper = render_sv(ports, [], config, "counter.sv")

            self.assertEqual(clocks[0]["source"], "registered")
            self.assertIn(
                "StaticPackedSignal<1, true, false", header
            )
            self.assertIn("kRegisteredClockConfigs", binding)
            self.assertIn("10000000ULL", binding)
            self.assertIn("bind_dut_for_clock_discovery", binding)
            self.assertIn("next_edge = $realtime + 1ns + 5ns;", wrapper)
            self.assertIn("task automatic drive_clock_0", wrapper)
            self.assertNotIn("drive_registered_clock_", wrapper)
            self.assertNotIn("dpi_clock_config", wrapper)

    def test_source_cli_infers_top_and_reports_ambiguous_sources(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            source = base / "design.sv"
            output = base / "generated"
            source.write_text("module inferred(input logic clk); endmodule\n")
            with contextlib.redirect_stdout(io.StringIO()):
                result = codegen_main(
                    [
                        str(source),
                        "--clock",
                        "clk=10ns",
                        "--output-dir",
                        str(output),
                    ]
                )
            self.assertEqual(result, 0)
            self.assertTrue((output / "inferred_dut.hpp").is_file())
            self.assertTrue((output / "dpi_inferred.cpp").is_file())

            source.write_text(
                "module inferred(input logic first_clk, "
                "input logic second_clk); endmodule\n"
            )
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors):
                result = codegen_main(
                    [
                        str(source),
                        "--clock",
                        "first_clk=4ns",
                        "--clock",
                        "second_clk=6ns@1ns",
                        "--output-dir",
                        str(output),
                    ]
                )
            self.assertEqual(result, 1)
            self.assertIn("require --primary-clock", errors.getvalue())

            source.write_text(
                "module first(input logic clk); endmodule\n"
                "module second(input logic clk); endmodule\n"
            )
            errors = io.StringIO()
            with contextlib.redirect_stderr(errors):
                result = codegen_main(
                    [str(source), "--output-dir", str(output)]
                )
            self.assertEqual(result, 1)
            self.assertIn("multiple top-level modules", errors.getvalue())
            self.assertIn("--top", errors.getvalue())

    def test_source_defaults_generate_generic_target_unique_dut(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            source = base / "counter.sv"
            source.write_text("module counter(input logic clk); endmodule\n")
            config = source_manifest(
                [source],
                "counter",
                output_dir=base / "generated",
                clocks=[parse_clock_argument("clk=10ns")],
            )
            ports = map_ports([Port("clk", "input", 1)], config)
            tree = build_tree(ports)
            header = render_cpp_dut(
                ports, [], tree, config, "counter.sv"
            )
            adapter = render_cpp_adapter(config, "counter.sv")

            self.assertEqual(config["namespace"], "cpptb::generated::counter")
            self.assertEqual(config["root_type"], "Dut")
            self.assertEqual(config["top_module"], "dpi_counter")
            self.assertEqual(
                config["clocks"][0]["half_period"], "5ns"
            )
            self.assertIn("struct Dut {", header)
            self.assertIn("using CounterDut = Dut;", header)
            self.assertIn("struct DpiAdapter", adapter)
            self.assertIn("cpptb::run_registered_test", adapter)
            self.assertIn("cpptb_counter_dpi_init", adapter)
            self.assertIn("CPP_DPI_COUNTER_RESULT", adapter)

    def test_source_defaults_defer_clock_timing_to_cpp_testbench(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            source = base / "clocked.sv"
            source.write_text(
                "module clocked(input logic clk, input logic enable, "
                "output logic output_clk); endmodule\n"
            )
            config = source_manifest(
                [source], "clocked", output_dir=base / "generated"
            )
            ports = map_ports(
                [
                    Port("clk", "input", 1),
                    Port("enable", "input", 1),
                    Port("output_clk", "output", 1),
                ],
                config,
            )
            tree = build_tree(ports)
            binding = render_cpp_binding(
                ports, [], tree, config, "clocked.sv"
            )
            adapter = render_cpp_adapter(config, "clocked.sv")
            wrapper = render_sv(ports, [], config, "clocked.sv")

            self.assertEqual(config["clocks"], [])
            self.assertIn("kSignalClk", binding)
            self.assertIn("kSignalOutputClk", binding)
            self.assertIn("coro::ClockRegistrar clocks", adapter)
            self.assertIn("CPPTB_DEFINE_NAMED_DPI_CLOCK_API", adapter)
            self.assertIn("task automatic drive_registered_clock_0", wrapper)
            self.assertIn("half_period = cpptb_clocked_dpi_clock_config", wrapper)
            self.assertIn("#(half_period);", wrapper)
            self.assertIn(
                "if (!clock_drivers_active || !registered_clock[SIGNAL_CLK])",
                wrapper,
            )
            self.assertIn("@(output_clk);", wrapper)
            self.assertIn("@(output_clk);", wrapper)
            self.assertNotIn("wait ((status != 0)", wrapper)
            self.assertNotIn("@(clk);", wrapper)
            self.assertNotIn("realtime next_edge", wrapper)

    def test_source_overrides_and_target_namespaces_do_not_collide(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            source = base / "design.sv"
            source.write_text("module design(input logic clk); endmodule\n")
            alpha = source_manifest(
                [source],
                "design",
                output_dir=base / "alpha",
                target="alpha",
                namespace="project::targets::alpha",
                root_type="TypedDut",
            )
            beta = source_manifest(
                [source],
                "design",
                output_dir=base / "beta",
                target="beta",
            )

            self.assertEqual(alpha["namespace"], "project::targets::alpha")
            self.assertEqual(alpha["root_type"], "TypedDut")
            self.assertEqual(alpha["top_module"], "dpi_alpha")
            self.assertEqual(beta["namespace"], "cpptb::generated::beta")
            self.assertNotEqual(alpha["namespace"], beta["namespace"])
            self.assertNotEqual(
                alpha["run"]["step_function"],
                beta["run"]["step_function"],
            )

    def test_clock_cli_requires_an_explicit_even_period(self):
        self.assertEqual(
            parse_clock_argument("clk=10ns")["half_period"], "5ns"
        )
        self.assertEqual(
            parse_clock_argument("fast_clk=1500ps")["half_period"],
            "750ps",
        )
        with self.assertRaisesRegex(CodegenError, "PORT=PERIOD"):
            parse_clock_argument("clk")
        with self.assertRaisesRegex(CodegenError, "whole femtosecond"):
            parse_clock_argument("clk=1fs")
        phased = parse_clock_argument("read_clk=6ns@1ns", primary=False)
        self.assertEqual(phased["half_period"], "3ns")
        self.assertEqual(phased["phase"], "1ns")
        self.assertFalse(phased["primary"])

    def test_v1_manifest_root_naming_remains_unchanged(self):
        config = manifest()
        ports = map_ports([Port("clk", "input", 1)], config)
        header = render_cpp_dut(
            ports, [], build_tree(ports), config, "sample.json"
        )
        self.assertIn("struct SampleDut {", header)
        self.assertNotIn("struct Dut {", header)
        self.assertNotIn("using SampleDut =", header)

    def test_signal_named_count_does_not_collide_with_wrapper_bookkeeping(self):
        config = manifest()
        del config["clock"]
        config["clocks"] = []
        ports = map_ports(
            [Port("enable", "input", 1), Port("count", "output", 8)],
            config,
        )

        header = render_cpp_dut(
            ports, [], build_tree(ports), config, "sample.json"
        )
        wrapper = render_sv(ports, [], config, "sample.json")

        self.assertIn("kSignalCount,", header)
        self.assertIn("kCpptbSignalCount,", header)
        self.assertEqual(wrapper.count("localparam int SIGNAL_COUNT ="), 1)
        self.assertIn("localparam int CPPTB_SIGNAL_COUNT = 2", wrapper)
        self.assertIn(
            "int unsigned edge_interest[0:CPPTB_SIGNAL_COUNT-1]", wrapper
        )

    def test_static_binding_is_opt_in_and_disabled_is_byte_identical(self):
        base = manifest()
        del base["clock"]
        base["clocks"] = []
        ports = map_ports(
            [Port("drive", "input", 1), Port("observe", "output", 1)],
            base,
        )
        tree = build_tree(ports)
        disabled = {**base, "codegen": {"static_binding": False}}

        self.assertEqual(
            render_cpp_dut(ports, [], tree, base, "sample.json"),
            render_cpp_dut(ports, [], tree, disabled, "sample.json"),
        )
        self.assertEqual(
            render_cpp_binding(ports, [], tree, base, "sample.json"),
            render_cpp_binding(ports, [], tree, disabled, "sample.json"),
        )
        self.assertEqual(
            render_sv(ports, [], base, "sample.json"),
            render_sv(ports, [], disabled, "sample.json"),
        )

    def test_generates_mixed_static_packed_and_on_demand_bindings(self):
        config = manifest()
        del config["clock"]
        config["clocks"] = []
        config["codegen"] = {"static_binding": True}
        config.setdefault("run", {})["compact_input_transport"] = False
        ports = map_ports(
            [
                Port("packed_scalar_i", "input", 1),
                Port(
                    "lazy_scalar_i", "input", 1,
                    transport="on_demand"
                ),
                Port("packed64_i", "input", 64, four_state=False),
                Port(
                    "lazy64_i", "input", 64, four_state=False,
                    transport="on_demand"
                ),
                Port(
                    "lazy137_i", "input", 137, four_state=False,
                    transport="on_demand"
                ),
                Port(
                    "packed_array_i", "input", 8,
                    unpacked=(UnpackedRange(3, 1),)
                ),
                Port(
                    "lazy_matrix_i", "input", 65, four_state=False,
                    unpacked=(UnpackedRange(2, 1), UnpackedRange(-1, 1)),
                    transport="on_demand"
                ),
                Port("packed_scalar_o", "output", 1),
                Port(
                    "lazy_scalar_o", "output", 1,
                    transport="on_demand"
                ),
                Port("packed64_o", "output", 64, four_state=False),
                Port(
                    "lazy64_o", "output", 64, four_state=False,
                    transport="on_demand"
                ),
                Port(
                    "lazy137_o", "output", 137, four_state=False,
                    transport="on_demand"
                ),
                Port(
                    "packed_array_o", "output", 8,
                    unpacked=(UnpackedRange(3, 1),)
                ),
                Port(
                    "lazy_matrix_o", "output", 65, four_state=False,
                    unpacked=(UnpackedRange(2, 1), UnpackedRange(-1, 1)),
                    transport="on_demand"
                ),
            ],
            config,
        )
        tree = build_tree(ports)
        header = render_cpp_dut(ports, [], tree, config, "sample.json")
        binding = render_cpp_binding(ports, [], tree, config, "sample.json")
        wrapper = render_sv(ports, [], config, "sample.json")

        self.assertIn('#include "cpptb/dpi_static_binding.hpp"', header)
        self.assertIn(
            "static constexpr bool cpptb_static_binding = true;", header
        )
        self.assertIn(
            "StaticPackedSignal<1, true, true, kSignalPackedScalarI, 0>",
            header,
        )
        self.assertIn(
            "StaticOnDemandSignal<1, true, true, kSignalLazyScalarI>",
            header,
        )
        self.assertIn(
            "StaticPackedSignal<64, true, true, kSignalPacked64I, 1>",
            header,
        )
        self.assertIn(
            "StaticOnDemandSignal<64, true, true, kSignalLazy64I>",
            header,
        )
        self.assertIn(
            "StaticOnDemandSignal<137, false, false, kSignalLazy137O>",
            header,
        )
        self.assertIn(
            "StaticPackedFixedArray<8, true, true, "
            "kSignalPackedArrayI, 3, 0, coro::ArrayDimension<3, 1>>",
            header,
        )
        self.assertIn(
            "StaticOnDemandFixedArray<65, false, false, "
            "kSignalLazyMatrixO, 0, coro::ArrayDimension<2, 1>, "
            "coro::ArrayDimension<-1, 1>>",
            header,
        )

        self.assertIn(
            "StaticPackedSignalSpec<64, true, true, kSignalPacked64I, 1>",
            binding,
        )
        self.assertIn(
            "StaticOnDemandSignalSpec<1, true, true, "
            "kSignalLazyScalarI, on_demand_port_1_get_words, "
            "on_demand_port_1_set_words>",
            binding,
        )
        self.assertIn(
            "StaticOnDemandArraySpec<65, false, false, "
            "kSignalLazyMatrixO, on_demand_port_13_get_words, nullptr, "
            "coro::ArrayDimension<2, 1>, coro::ArrayDimension<-1, 1>>",
            binding,
        )
        self.assertIn("kStaticPackedBindingSpans", binding)
        self.assertIn("validate_static_packed_binding_spans", binding)
        self.assertNotIn(
            "StaticOnDemandSignalSpec<64, true, true, kSignalLazy64I, 3,",
            binding,
        )

        self.assertIn(
            "unsigned long long dpi_sample_dut_port_3_get();", binding
        )
        self.assertIn(
            "void dpi_sample_dut_port_4_get(svBitVecVal* value);", binding
        )
        self.assertIn("localparam int INPUT_WORD_COUNT = 6", wrapper)
        self.assertIn("localparam int OUTPUT_WORD_COUNT = 6", wrapper)
        self.assertNotIn("INPUT_SIGNAL_LAZY64I", wrapper)
        self.assertNotIn("OUTPUT_SIGNAL_LAZY137O", wrapper)

    def test_rejects_non_boolean_static_binding_flag(self):
        config = manifest()
        config["codegen"] = {"static_binding": "yes"}
        ports = map_ports([Port("clk", "input", 1)], config)
        with self.assertRaisesRegex(CodegenError, "must be a boolean"):
            render_cpp_dut(
                ports, [], build_tree(ports), config, "sample.json"
            )

    def test_generates_raw_preserving_packed_enum_and_struct_views(self):
        state_base = PackedIntegralType(
            3, signed=True, four_state=True, ranges=(PackedRange(2, 0),)
        )
        state = PackedEnumType(
            state_base,
            (
                PackedEnumValue("NEGATIVE", -1),
                PackedEnumValue("IDLE", 0),
                PackedEnumValue("RUN", 3),
            ),
            declared_name="state_t",
        )
        inner = PackedStructType(
            5,
            signed=False,
            four_state=True,
            fields=(
                PackedField(
                    "tag", PackedIntegralType(2, False, False), bit_offset=3
                ),
                PackedField(
                    "payload", PackedIntegralType(3, False, True), bit_offset=0
                ),
            ),
            declared_name="inner_t",
        )
        packet = PackedStructType(
            11,
            signed=False,
            four_state=True,
            fields=(
                PackedField(
                    "opcode", PackedIntegralType(3, False, True), bit_offset=8
                ),
                PackedField("state", state, bit_offset=5),
                PackedField("inner", inner, bit_offset=0),
            ),
            declared_name="packet_t",
        )
        config = manifest()
        ports = map_ports(
            [
                Port("clk", "input", 1),
                Port("packet_i", "input", 11, packed_type=packet),
                Port("state_o", "output", 3, signed=True, packed_type=state),
            ],
            config,
        )
        header = render_cpp_dut(
            ports, [], build_tree(ports), config, "sample.json"
        )

        self.assertIn("enum class StateT : std::int64_t", header)
        self.assertIn("Negative = -1", header)
        self.assertIn("class StateTValue", header)
        self.assertIn("from_raw_bits(raw_type bits)", header)
        self.assertIn("class PacketTValue", header)
        self.assertIn("bits_.slice<3>(8)", header)
        self.assertIn("bits_.set_slice<3>(", header)
        self.assertIn("StateTValue state() const", header)
        self.assertIn("PacketTValue& set_state(StateT value)", header)
        self.assertIn("InnerTView<StorageWidth> inner()", header)
        self.assertIn("using signal_value_type = std::uint32_t", header)
        self.assertIn("signal_value_type signal_value() const", header)
        self.assertIn("coro::Signal packet_i;", header)

    def test_maps_flat_ports_to_typed_hierarchy(self):
        ports = map_ports(
            [
                Port("clk", "input", 1),
                Port("bus_PADDR", "input", 12),
                Port("bus_PWDATA", "input", 32),
                Port("bus_ready", "output", 1),
            ],
            manifest(),
        )

        self.assertEqual(ports[1].cpp_path, ("bus", "apb", "PADDR"))
        self.assertEqual(ports[3].cpp_path, ("bus", "ready"))

        tree = build_tree(ports)
        header = render_cpp_dut(ports, [], tree, manifest(), "sample.json")
        binding = render_cpp_binding(ports, [], tree, manifest(), "sample.json")
        wrapper = render_sv(ports, [], manifest(), "sample.json")

        self.assertIn("struct ApbBus", header)
        self.assertIn("ApbBus apb;", header)
        self.assertIn("kSignalBusPaddr", binding)
        self.assertIn("logic [11:0] bus_PADDR;", wrapper)
        self.assertIn(
            "bus_PADDR = out_words[OUTPUT_SIGNAL_BUSPADDR][11:0];", wrapper
        )
        self.assertNotIn("bus_ready = out_words", wrapper)
        self.assertIn("localparam int INPUT_WORD_COUNT = 2", wrapper)
        self.assertIn("localparam int OUTPUT_WORD_COUNT = 2", wrapper)
        self.assertIn("kObservedSignalWordIds", binding)
        self.assertIn("kDrivenSignalWordIds", binding)

    def test_discovers_elaborated_port_widths(self):
        ast = {
            "modulesp": [
                {
                    "type": "MODULE",
                    "name": "sample_dut",
                    "stmtsp": [
                        {
                            "type": "VAR",
                            "varType": "PORT",
                            "name": "data",
                            "direction": "OUTPUT",
                            "dtypep": "(A)",
                        }
                    ],
                }
            ],
            "typeTable": [
                {
                    "type": "BASICDTYPE",
                    "addr": "(A)",
                    "name": "logic",
                    "range": "15:0",
                }
            ],
        }

        self.assertEqual(discover_ports(ast, "sample_dut"), [Port("data", "output", 16)])

    def test_generates_multiple_clock_drivers_and_delay_transport(self):
        config = manifest()
        del config["clock"]
        config["clocks"] = [
            {"port": "write_clk", "half_period": "2ns", "primary": True},
            {"port": "read_clk", "half_period": "3ns", "phase": "1ns"},
        ]
        ports = map_ports(
            [
                Port("write_clk", "input", 1),
                Port("read_clk", "input", 1),
                Port("bus_PADDR", "input", 12),
                Port("bus_ready", "output", 1),
            ],
            config,
        )
        tree = build_tree(ports)

        binding = render_cpp_binding(ports, [], tree, config, "sample.json")
        wrapper = render_sv(ports, [], config, "sample.json")

        self.assertIn("std::array<uint32_t, 2> kClockSignalIds", binding)
        self.assertNotIn("kSignalWriteClk,\n    kSignalReadClk,\n    kSignalBusPaddr", binding)
        self.assertIn("task automatic drive_clock_0", wrapper)
        self.assertIn("task automatic drive_clock_1", wrapper)
        self.assertNotIn("task automatic observe_clock_0", wrapper)
        self.assertNotIn("task automatic observe_clock_1", wrapper)
        self.assertIn("realtime next_edge", wrapper)
        self.assertIn("STEP_FALLING_EDGES", wrapper)
        self.assertIn("STEP_OUTPUTS_CHANGED", wrapper)
        self.assertIn("STEP_NEXT_TICK_TIMER", wrapper)
        self.assertIn("STEP_TIMER_IDLE", wrapper)
        self.assertIn("if ((requests >= 0) &&", wrapper)
        self.assertIn("((phase == PHASE_INIT) ||", wrapper)
        self.assertIn("((requests & STEP_OUTPUTS_CHANGED) != 0)", wrapper)
        self.assertIn("cpptb_dpi_pull_outputs(out_words);", wrapper)
        self.assertIn("input int unsigned in_words[]\n  );", wrapper)
        self.assertNotIn("in_words[],\n      output int unsigned out_words[]", wrapper)
        self.assertEqual(wrapper.count("apply_outputs();"), 3)
        self.assertIn("event phase_outputs_pending;", wrapper)
        self.assertIn("always @(phase_outputs_pending)", wrapper)
        self.assertIn("-> phase_outputs_pending;", wrapper)
        self.assertIn("timeunit 1ps", wrapper)
        self.assertIn("TIMEPRECISION_FS = 1000", wrapper)
        self.assertIn("cpptb_dpi_init(iterations, TIMEPRECISION_FS)", wrapper)
        self.assertNotIn("PHASE_SAMPLE", wrapper)
        self.assertNotIn("PHASE_DRIVE", wrapper)
        self.assertNotIn("sample_delay", wrapper)
        self.assertIn("cpptb_dpi_next_timer_deadline", wrapper)
        self.assertIn("cpptb_dpi_edge_interest", wrapper)
        self.assertIn(
            'import "DPI-C" function longint unsigned '
            "cpptb_dpi_next_timer_deadline();",
            wrapper,
        )
        self.assertNotIn(
            'import "DPI-C" context function longint unsigned '
            "cpptb_dpi_next_timer_deadline();",
            wrapper,
        )
        self.assertIn("longint unsigned timer_deadline;", wrapper)
        self.assertIn("longint unsigned timer_owner_target;", wrapper)
        self.assertIn("event timer_kick;", wrapper)
        self.assertIn("task automatic timer_owner();", wrapper)
        self.assertIn("task automatic update_timer_schedule();", wrapper)
        self.assertIn("timer_deadline = $time + 1;", wrapper)
        self.assertIn("timer_wakeup(timer_deadline, generation);", wrapper)
        self.assertNotIn("#1step;", wrapper)
        self.assertNotIn("service_done", wrapper)
        self.assertIn("if (timer_owner_target == NO_TIMER) begin", wrapper)
        self.assertIn("end else if (deadline < timer_owner_target) begin", wrapper)
        self.assertIn("timer_wakeup(deadline, generation);", wrapper)
        self.assertNotIn("reschedule_timer", wrapper)
        self.assertNotIn("#0", wrapper)
        self.assertNotIn("<=", wrapper)
        self.assertNotIn("disable fork", wrapper)
        launcher = wrapper.index(
            "    if (status == 0) begin\n"
            "      fork\n"
            "        timer_owner();"
        )
        self.assertLess(wrapper.index("service_requests(initial_requests);"), launcher)
        self.assertLess(launcher, wrapper.index("        drive_clock_0();", launcher))
        self.assertLess(launcher, wrapper.index("        drive_clock_1();", launcher))

    def test_generates_persistent_timer_owner_for_clockless_wrapper(self):
        config = manifest()
        del config["clock"]
        config["clocks"] = []
        ports = map_ports(
            [
                Port("data_i", "input", 8),
                Port("data_o", "output", 8),
            ],
            config,
        )

        wrapper = render_sv(ports, [], config, "clockless.json")

        self.assertIn("task automatic timer_owner();", wrapper)
        self.assertIn(
            "    if (status == 0) begin\n"
            "      fork\n"
            "        timer_owner();\n"
            "      join_none\n"
            "    end",
            wrapper,
        )
        self.assertNotIn("task automatic drive_clock_", wrapper)
        self.assertNotIn("task automatic observe_clock_", wrapper)
        self.assertNotIn("reschedule_timer", wrapper)
        self.assertNotIn("#0", wrapper)
        self.assertNotIn("<=", wrapper)
        self.assertNotIn("disable fork", wrapper)

    def test_generates_opt_in_dut_output_edge_observer(self):
        config = manifest()
        config["edge_observers"] = ["bus_ready"]
        ports = map_ports(
            [
                Port("clk", "input", 1),
                Port("bus_PADDR", "input", 12),
                Port("bus_ready", "output", 1),
            ],
            config,
        )
        tree = build_tree(ports)

        binding = render_cpp_binding(ports, [], tree, config, "sample.json")
        wrapper = render_sv(ports, [], config, "sample.json")

        self.assertIn("kEdgeObserverSignalIds", binding)
        self.assertIn("kSignalBusReady", binding)
        self.assertIn("task automatic observe_signal_0", wrapper)
        self.assertIn("@(bus_ready);", wrapper)
        self.assertIn("edge_interest[SIGNAL_BUSREADY]", wrapper)
        observer_start = wrapper.index("  task automatic observe_signal_0();")
        observer_end = wrapper.index("  endtask", observer_start)
        observer = wrapper[observer_start:observer_end]
        self.assertNotIn("wait (", observer)
        self.assertLess(
            observer.index("@(bus_ready);"),
            observer.index("edge_interest[SIGNAL_BUSREADY]"),
        )
        self.assertLess(
            observer.index("edge_interest[SIGNAL_BUSREADY]"),
            observer.index("run_step(PHASE_EDGE"),
        )
        self.assertIn("STEP_EDGE_INTEREST_CHANGED", wrapper)
        self.assertIn("observe_signal_0();", wrapper)
        launcher = wrapper.index(
            "    if (status == 0) begin\n"
            "      fork\n"
            "        timer_owner();"
        )
        self.assertLess(wrapper.index("service_requests(initial_requests);"), launcher)
        self.assertLess(launcher, wrapper.index("        drive_clock_0();", launcher))
        self.assertLess(launcher, wrapper.index("        observe_signal_0();", launcher))

    def test_auto_edge_observers_use_the_discovered_port_set(self):
        config = manifest()
        config["edge_observers"] = []
        config["auto_edge_observers"] = True
        config["edge_observer_signal_ids"] = [0, 2]
        ports = map_ports(
            [
                Port("clk", "input", 1),
                Port("unused", "output", 1),
                Port("event_out", "output", 1),
            ],
            config,
        )
        wrapper = render_sv(ports, [], config, "sample.json")

        self.assertIn("@(event_out);", wrapper)
        self.assertNotIn("@(unused);", wrapper)
        self.assertNotIn("@(clk);", wrapper)

    def test_rejects_invalid_edge_observer_ports(self):
        ports = [
            Port("clk", "input", 1),
            Port("data_i", "input", 1),
            Port("wide_o", "output", 8),
            Port("array_o", "output", 1, unpacked=(UnpackedRange(1, 0),)),
        ]
        cases = [
            ("missing", "was not found"),
            ("clk", "configured clock"),
            ("data_i", "DUT output"),
            ("wide_o", "one bit"),
            ("array_o", "unpacked array"),
        ]
        for name, message in cases:
            with self.subTest(name=name):
                config = manifest()
                config["edge_observers"] = [name]
                with self.assertRaisesRegex(CodegenError, message):
                    render_sv(ports, [], config, "sample.json")

    def test_parses_timeprecision_for_delay_transport(self):
        self.assertEqual(time_literal_femtoseconds("1fs", "precision"), 1)
        self.assertEqual(time_literal_femtoseconds("10 ps", "precision"), 10_000)
        self.assertEqual(
            time_literal_femtoseconds("2ns", "precision"), 2_000_000
        )
        with self.assertRaises(CodegenError):
            time_literal_femtoseconds("0ps", "precision")
        with self.assertRaises(CodegenError):
            time_literal_femtoseconds("0.5ps", "precision")

    def test_supports_generated_testbench_and_dut_clock_sources(self):
        config = manifest()
        del config["clock"]
        config["clocks"] = [
            {
                "port": "core_clk",
                "source": "generated",
                "half_period": "2ns",
                "primary": True,
            },
            {"port": "scan_clk", "source": "testbench"},
            {"port": "gated_clk", "source": "dut"},
        ]
        ports = map_ports(
            [
                Port("core_clk", "input", 1),
                Port("scan_clk", "input", 1),
                Port("gated_clk", "output", 1),
                Port("bus_PADDR", "input", 12),
            ],
            config,
        )
        tree = build_tree(ports)

        validate_clock_ports(config, ports)
        binding = render_cpp_binding(ports, [], tree, config, "sample.json")
        wrapper = render_sv(ports, [], config, "sample.json")

        driven_section = binding.split("kDrivenSignalSpans = {{", 1)[1].split(
            "}};", 1
        )[0]
        self.assertIn("kSignalScanClk,", driven_section)
        self.assertIn("kSignalBusPaddr,", driven_section)
        self.assertNotIn("kSignalCoreClk,", driven_section)
        self.assertNotIn("kSignalGatedClk,", driven_section)
        self.assertIn(
            "scan_clk = out_words[OUTPUT_SIGNAL_SCANCLK][0];", wrapper
        )
        self.assertNotIn("core_clk = out_words[OUTPUT_SIGNAL_CORECLK][0];", wrapper)
        self.assertIn("task automatic drive_clock_0", wrapper)
        self.assertNotIn("task automatic drive_clock_1", wrapper)
        self.assertNotIn("task automatic drive_clock_2", wrapper)
        self.assertNotIn("task automatic observe_clock_0", wrapper)
        self.assertIn("task automatic observe_clock_1", wrapper)
        self.assertIn("task automatic observe_clock_2", wrapper)

    def test_rejects_invalid_clock_source_port_combinations(self):
        ports = [
            Port("input_clk", "input", 1),
            Port("output_clk", "output", 1),
        ]

        config = manifest()
        config["clock"] = {"port": "output_clk", "source": "generated"}
        with self.assertRaisesRegex(CodegenError, "must be a DUT input"):
            validate_clock_ports(config, ports)

        config["clock"] = {"port": "input_clk", "source": "dut"}
        with self.assertRaisesRegex(CodegenError, "must be a DUT output"):
            validate_clock_ports(config, ports)

        config["clock"] = {
            "port": "input_clk",
            "source": "testbench",
            "half_period": "2ns",
        }
        with self.assertRaisesRegex(CodegenError, "cannot define half_period"):
            validate_clock_ports(config, ports)

    def test_supports_a_clockless_dut(self):
        config = manifest()
        del config["clock"]
        config["clocks"] = []
        ports = map_ports(
            [
                Port("bus_PADDR", "input", 12),
                Port("bus_ready", "output", 1),
            ],
            config,
        )
        tree = build_tree(ports)

        validate_clock_ports(config, ports)
        binding = render_cpp_binding(ports, [], tree, config, "sample.json")
        wrapper = render_sv(ports, [], config, "sample.json")

        self.assertIn("std::array<uint32_t, 0> kClockSignalIds", binding)
        self.assertNotIn("drive_clock_", wrapper)
        self.assertNotIn("observe_clock_", wrapper)
        self.assertNotIn("fork\n      join_none", wrapper)

    def test_generates_flattened_words_for_wide_ports(self):
        ast = {
            "modulesp": [
                {
                    "type": "MODULE",
                    "name": "sample_dut",
                    "stmtsp": [
                        {
                            "type": "VAR",
                            "varType": "PORT",
                            "name": "wide",
                            "direction": "INPUT",
                            "dtypep": "(A)",
                        }
                    ],
                }
            ],
            "typeTable": [
                {
                    "type": "BASICDTYPE",
                    "addr": "(A)",
                    "name": "bit",
                    "range": "63:0",
                }
            ],
        }

        ports = discover_ports(ast, "sample_dut")
        ports.extend(
            [
                Port("wide_65", "output", 65, four_state=False),
                Port("wide_137", "input", 137, four_state=False),
            ]
        )
        validate_transport_ports(ports)

        config = manifest()
        del config["clock"]
        config["clocks"] = []
        config["aliases"] = {
            "wide": "wide",
            "wide_65": "wide_65",
            "wide_137": "wide_137",
        }
        mapped = map_ports(ports, config)
        tree = build_tree(mapped)
        header = render_cpp_dut(mapped, [], tree, config, "sample.json")
        binding = render_cpp_binding(mapped, [], tree, config, "sample.json")
        wrapper = render_sv(mapped, [], config, "sample.json")

        self.assertIn("kSignalWide = 0", header)
        self.assertIn("kSignalWide65 = 2", header)
        self.assertIn("kSignalWide137 = 5", header)
        self.assertIn("kCpptbSignalCount = 10", header)
        self.assertIn("coro::DrivenSignal<64> wide;", header)
        self.assertIn("coro::ObservedSignal<65> wide_65;", header)
        self.assertIn("coro::DrivenSignal<137> wide_137;", header)
        self.assertIn("SignalSpec<137, true>", binding)
        self.assertIn("localparam int SIGNAL_WIDE65 = 2", wrapper)
        self.assertIn("localparam int CPPTB_SIGNAL_COUNT = 10", wrapper)
        self.assertIn("bit [136:0] wide_137;", wrapper)
        self.assertNotIn("in_words[INPUT_SIGNAL_WIDE137", wrapper)
        self.assertIn(
            "in_words[INPUT_SIGNAL_WIDE65 + 2] = wide_65[64 +: 1];", wrapper
        )
        self.assertIn(
            "wide_137 = {out_words[OUTPUT_SIGNAL_WIDE137 + 4][8:0], "
            "out_words[OUTPUT_SIGNAL_WIDE137 + 3], "
            "out_words[OUTPUT_SIGNAL_WIDE137 + 2], "
            "out_words[OUTPUT_SIGNAL_WIDE137 + 1], "
            "out_words[OUTPUT_SIGNAL_WIDE137 + 0]};",
            wrapper,
        )
        self.assertIn("localparam int INPUT_WORD_COUNT = 3", wrapper)
        self.assertIn("localparam int OUTPUT_WORD_COUNT = 7", wrapper)
        self.assertIn("kSignalWide65 + 2", binding)
        self.assertIn("kSignalWide137 + 4", binding)

    def test_rejects_wide_four_state_port(self):
        with self.assertRaisesRegex(CodegenError, "requires a two-state bit port"):
            validate_transport_ports([Port("wide", "input", 33)])

    def test_can_generate_sparse_input_transport_diagnostic(self):
        config = manifest()
        del config["clock"]
        config["clocks"] = []
        config.setdefault("run", {})["compact_input_transport"] = False
        mapped = map_ports(
            [Port("drive_i", "input", 32), Port("observe_o", "output", 32)],
            config,
        )
        tree = build_tree(mapped)
        binding = render_cpp_binding(mapped, [], tree, config, "sample.json")
        wrapper = render_sv(mapped, [], config, "sample.json")

        self.assertIn("kCompactInputTransport = false", binding)
        self.assertIn("localparam int INPUT_SIGNAL_OBSERVEO = 1", wrapper)
        self.assertIn("localparam int INPUT_WORD_COUNT = 2", wrapper)
        self.assertIn("localparam int OUTPUT_SIGNAL_DRIVEI = 0", wrapper)
        self.assertIn("localparam int OUTPUT_WORD_COUNT = 1", wrapper)

    def test_generates_range_aware_unpacked_array_transport(self):
        ports = [
            Port(
                "arr_i",
                "input",
                32,
                unpacked=(UnpackedRange(1, 8),),
            ),
            Port(
                "warr_o",
                "output",
                64,
                four_state=False,
                unpacked=(UnpackedRange(3, 0),),
            ),
        ]
        validate_transport_ports(ports)

        config = manifest()
        del config["clock"]
        config["clocks"] = []
        mapped = map_ports(ports, config)
        tree = build_tree(mapped)
        header = render_cpp_dut(mapped, [], tree, config, "sample.json")
        binding = render_cpp_binding(mapped, [], tree, config, "sample.json")
        wrapper = render_sv(mapped, [], config, "sample.json")

        self.assertIn("kSignalArrI = 0", header)
        self.assertIn("kSignalWarrO = 8", header)
        self.assertIn("kCpptbSignalCount = 16", header)
        self.assertIn("coro::DrivenArray<32, 1, 8> arr_i;", header)
        self.assertIn("coro::ObservedArray<64, 3, 0> warr_o;", header)
        self.assertIn("{kSignalArrI, 8}", binding)
        self.assertIn("ArraySpec<32, 1, 8, true>", binding)
        self.assertIn("ArraySpec<64, 3, 0, false>", binding)
        self.assertIn("logic [31:0] arr_i [1:8];", wrapper)
        self.assertIn("bit [63:0] warr_o [3:0];", wrapper)
        self.assertIn("arr_i = '{default: '0};", wrapper)
        self.assertIn(
            "for (int cpptb_arr_i_index = 1; cpptb_arr_i_index <= 8;",
            wrapper,
        )
        self.assertNotIn("in_words[INPUT_SIGNAL_ARRI", wrapper)
        self.assertIn(
            "for (int cpptb_warr_o_index = 0; cpptb_warr_o_index <= 3;",
            wrapper,
        )
        self.assertIn("in_words[INPUT_SIGNAL_WARRO", wrapper)
        self.assertIn("out_words[OUTPUT_SIGNAL_ARRI", wrapper)
        self.assertIn("localparam int INPUT_WORD_COUNT = 8", wrapper)
        self.assertIn("localparam int OUTPUT_WORD_COUNT = 8", wrapper)

    def test_generates_multidimensional_unpacked_transport(self):
        ports = [
            Port(
                "matrix_i",
                "input",
                8,
                unpacked=(UnpackedRange(1, 0), UnpackedRange(2, 4)),
            ),
            Port(
                "cube_o",
                "output",
                65,
                four_state=False,
                unpacked=(
                    UnpackedRange(-1, 0),
                    UnpackedRange(3, 2),
                    UnpackedRange(5, 7),
                ),
            ),
        ]
        validate_transport_ports(ports)

        config = manifest()
        del config["clock"]
        config["clocks"] = []
        mapped = map_ports(ports, config)
        tree = build_tree(mapped)
        header = render_cpp_dut(mapped, [], tree, config, "sample.json")
        binding = render_cpp_binding(mapped, [], tree, config, "sample.json")
        wrapper = render_sv(mapped, [], config, "sample.json")

        self.assertIn("kSignalCubeO = 6", header)
        self.assertIn("kCpptbSignalCount = 42", header)
        self.assertIn(
            "coro::DrivenFixedArray<8, coro::ArrayDimension<1, 0>, "
            "coro::ArrayDimension<2, 4>> matrix_i;",
            header,
        )
        self.assertIn(
            "coro::ObservedFixedArray<65, coro::ArrayDimension<-1, 0>, "
            "coro::ArrayDimension<3, 2>, coro::ArrayDimension<5, 7>> cube_o;",
            header,
        )
        self.assertIn("FixedArraySpec<8, true", binding)
        self.assertIn("ArraySpec<96, 1, 0, true>", binding)
        self.assertIn("FixedArraySpec<65, false", binding)
        self.assertIn("ArraySpec<576, -1, 0, false>", binding)
        self.assertIn("{kSignalMatrixI, 6}", binding)
        self.assertIn("logic [7:0] matrix_i [1:0] [2:4];", wrapper)
        self.assertIn("bit [64:0] cube_o [-1:0] [3:2] [5:7];", wrapper)
        self.assertIn(
            "for (int cpptb_matrix_i_index_0 = 0; "
            "cpptb_matrix_i_index_0 <= 1;",
            wrapper,
        )
        self.assertIn(
            "for (int cpptb_cube_o_index_2 = 5; "
            "cpptb_cube_o_index_2 <= 7;",
            wrapper,
        )
        self.assertIn(
            "((cpptb_matrix_i_index_0 - 0) * 3 + "
            "(cpptb_matrix_i_index_1 - 2)) * 1",
            wrapper,
        )
        self.assertIn(
            "((cpptb_cube_o_index_0 - -1) * 2 + "
            "(cpptb_cube_o_index_1 - 2)) * 3 + "
            "(cpptb_cube_o_index_2 - 5)",
            wrapper,
        )

    def test_generates_pull_based_internal_probe_exports(self):
        config = manifest()
        ports = map_ports([Port("clk", "input", 1)], config)
        internals = [
            Internal(
                "state",
                ("internal", "state"),
                "variable",
                7,
                access="read_write",
                forceable=True,
            ),
            Internal(
                "u_core.observed",
                ("internal", "debug", "observed"),
                "net",
                9,
                forceable=True,
            ),
            Internal(
                "wide_state",
                ("internal", "wide_state"),
                "variable",
                64,
                access="read_write",
                four_state=False,
            ),
            Internal(
                "memory",
                ("internal", "memory"),
                "variable",
                73,
                access="read_write",
                forceable=True,
                four_state=False,
                unpacked=(UnpackedRange(7, 4),),
            ),
        ]
        validate_internals(internals)
        tree = build_tree([*ports, *internals])

        header = render_cpp_dut(ports, internals, tree, config, "sample.json")
        binding = render_cpp_binding(
            ports, internals, tree, config, "sample.json"
        )
        wrapper = render_sv(ports, internals, config, "sample.json")

        self.assertIn("probe::Probe<7, true, true> state;", header)
        self.assertIn("probe::Probe<9, false, true> observed;", header)
        self.assertIn("probe::Probe<64, true> wide_state;", header)
        self.assertIn("probe::MemoryProbe<73, 7, 4, true, true> memory;", header)
        self.assertIn("struct InternalDut", header)
        self.assertIn("InternalDut internal;", header)
        self.assertIn(
            "unsigned int dpi_sample_dut_internal_0_get();", binding
        )
        self.assertIn(
            "void dpi_sample_dut_internal_0_deposit(unsigned int value);",
            binding,
        )
        self.assertIn(
            "void dpi_sample_dut_internal_0_force(unsigned int value);",
            binding,
        )
        self.assertIn("void dpi_sample_dut_internal_0_release();", binding)
        self.assertIn(
            "void dpi_sample_dut_internal_1_force(unsigned int value);",
            binding,
        )
        self.assertIn(
            "unsigned long long dpi_sample_dut_internal_2_get();", binding
        )
        self.assertIn(
            "void dpi_sample_dut_internal_2_deposit(unsigned long long value);",
            binding,
        )
        self.assertIn(
            "void dpi_sample_dut_internal_3_get(int index, svBitVecVal* value);",
            binding,
        )
        self.assertIn(
            "void dpi_sample_dut_internal_3_deposit(int index, "
            "const svBitVecVal* value);",
            binding,
        )
        self.assertIn(
            "void dpi_sample_dut_internal_3_force(int index, "
            "const svBitVecVal* value);",
            binding,
        )
        self.assertIn(
            "void dpi_sample_dut_internal_3_release(int index);", binding
        )
        self.assertIn("make_internal_0()", binding)
        self.assertIn("make_internal_3()", binding)
        self.assertNotIn("unsigned int* value", binding)
        self.assertIn(
            "dpi_sample_dut_internal_0_deposit(value);", binding
        )
        self.assertIn(
            'export "DPI-C" function dpi_sample_dut_internal_0_get;', wrapper
        )
        self.assertIn(
            "function int unsigned dpi_sample_dut_internal_0_get();", wrapper
        )
        self.assertIn("dpi_sample_dut_internal_0_get = i_dut.state;", wrapper)
        self.assertIn(
            "function longint unsigned dpi_sample_dut_internal_2_get();", wrapper
        )
        self.assertIn(
            'export "DPI-C" function dpi_sample_dut_internal_0_deposit;',
            wrapper,
        )
        self.assertIn("i_dut.state = value;", wrapper)
        self.assertIn("i_dut.wide_state = value;", wrapper)
        self.assertIn("4: i_dut.memory[4] = value;", wrapper)
        self.assertIn("4: value = i_dut.memory[4];", wrapper)
        self.assertIn("bit [6:0] internal_0_force_shadow;", wrapper)
        self.assertIn(
            "force i_dut.state = internal_0_force_shadow;", wrapper
        )
        self.assertIn("release i_dut.state;", wrapper)
        self.assertIn("force i_dut.u_core.observed", wrapper)
        self.assertIn("bit [72:0] internal_3_force_shadow [7:4];", wrapper)
        self.assertIn("4: begin", wrapper)
        self.assertIn(
            "force i_dut.memory[4] = internal_3_force_shadow[4];", wrapper
        )
        self.assertIn("7: release i_dut.memory[7];", wrapper)
        self.assertNotIn("SIGNAL_STATE", wrapper)

    def test_rejects_invalid_internal_transport(self):
        with self.assertRaisesRegex(CodegenError, "read_write access requires"):
            validate_internals(
                [
                    Internal(
                        "status",
                        ("internal", "status"),
                        "net",
                        1,
                        access="read_write",
                    )
                ]
            )

        with self.assertRaisesRegex(
            CodegenError, "forceable memories require a variable"
        ):
            validate_internals(
                [
                    Internal(
                        "net_memory",
                        ("internal", "net_memory"),
                        "net",
                        8,
                        forceable=True,
                        unpacked=(UnpackedRange(0, 3),),
                    )
                ]
            )

        with self.assertRaisesRegex(CodegenError, "dispatch limit is 1024"):
            validate_internals(
                [
                    Internal(
                        "large_memory",
                        ("internal", "large_memory"),
                        "variable",
                        8,
                        forceable=True,
                        unpacked=(UnpackedRange(0, 1024),),
                    )
                ]
            )

    def test_frontend_disagreement_reports_unpacked_ranges(self):
        primary = DesignIR(
            "sample_dut",
            (Port("array_i", "input", 73, unpacked=(UnpackedRange(7, 4),)),),
        )
        comparison = DesignIR(
            "sample_dut",
            (Port("array_i", "input", 73, unpacked=(UnpackedRange(4, 7),)),),
        )

        with self.assertRaisesRegex(CodegenError, r"array_i\[73\]\[7:4\]"):
            compare_designs(primary, comparison, "slang", "verilator_json")

    def test_applies_and_validates_port_transport(self):
        design = DesignIR(
            "sample_dut",
            (
                Port("clk", "input", 1),
                Port("data_i", "input", 8),
                Port("event_o", "output", 1),
            ),
        )
        config = manifest()
        config["edge_observers"] = ["event_o"]
        config["port_transport"] = {"data_i": "on_demand"}
        applied = apply_port_transport(design, config)
        self.assertEqual(applied.ports[0].transport, "packed")
        self.assertEqual(applied.ports[1].transport, "on_demand")

        cases = [
            ([], "must be an object"),
            ({"missing": "on_demand"}, "was not found"),
            ({"data_i": "lazy"}, "invalid transport"),
            ({"data_i": []}, "invalid transport"),
            ({"clk": "on_demand"}, "configured clock"),
            ({"event_o": "on_demand"}, "edge observer"),
        ]
        for mapping, message in cases:
            with self.subTest(mapping=mapping):
                config["port_transport"] = mapping
                with self.assertRaisesRegex(CodegenError, message):
                    apply_port_transport(design, config)

    def test_transport_mode_participates_in_frontend_signature(self):
        primary = DesignIR(
            "sample_dut", (Port("data_i", "input", 32),)
        )
        comparison = DesignIR(
            "sample_dut",
            (Port("data_i", "input", 32, transport="on_demand"),),
        )

        with self.assertRaisesRegex(CodegenError, "transport=on_demand"):
            compare_designs(primary, comparison, "slang", "verilator_json")

    def test_explicit_packed_transport_is_byte_identical(self):
        design = DesignIR(
            "sample_dut",
            (Port("clk", "input", 1), Port("data_o", "output", 17)),
        )
        default_config = manifest()
        explicit_config = manifest()
        explicit_config["port_transport"] = {
            "clk": "packed",
            "data_o": "packed",
        }

        outputs = []
        for config in (default_config, explicit_config):
            ports = map_ports(
                list(apply_port_transport(design, config).ports), config
            )
            tree = build_tree(ports)
            outputs.append(
                (
                    render_cpp_dut(ports, [], tree, config, "sample.json"),
                    render_cpp_binding(ports, [], tree, config, "sample.json"),
                    render_sv(ports, [], config, "sample.json"),
                )
            )
        self.assertEqual(outputs[0], outputs[1])

    def test_generates_on_demand_abi_for_all_widths_and_shapes(self):
        config = manifest()
        del config["clock"]
        config["clocks"] = []
        ports = map_ports(
            [
                Port("hot_i", "input", 32),
                Port("hot_o", "output", 32),
                Port("narrow_i", "input", 17, transport="on_demand"),
                Port(
                    "u64_o", "output", 64, four_state=False,
                    transport="on_demand"
                ),
                Port(
                    "wide_i", "input", 137, four_state=False,
                    transport="on_demand"
                ),
                Port(
                    "array_o", "output", 32,
                    unpacked=(UnpackedRange(3, 0),),
                    transport="on_demand",
                ),
                Port(
                    "matrix_i", "input", 65, four_state=False,
                    unpacked=(UnpackedRange(2, 1), UnpackedRange(-1, 1)),
                    transport="on_demand",
                ),
            ],
            config,
        )
        tree = build_tree(ports)
        binding = render_cpp_binding(ports, [], tree, config, "sample.json")
        wrapper = render_sv(ports, [], config, "sample.json")

        self.assertIn("unsigned int dpi_sample_dut_port_2_get();", binding)
        self.assertIn(
            "void dpi_sample_dut_port_2_set(unsigned int value);", binding
        )
        self.assertIn(
            "unsigned long long dpi_sample_dut_port_3_get();", binding
        )
        self.assertIn(
            "void dpi_sample_dut_port_4_get(svBitVecVal* value);", binding
        )
        self.assertIn(
            "unsigned int dpi_sample_dut_port_5_get(int index_0);", binding
        )
        self.assertIn(
            "dpi_sample_dut_port_6_get(int index_0, int index_1, "
            "svBitVecVal* value)",
            binding,
        )
        self.assertIn("remaining % 3", binding)
        self.assertIn("remaining /= 3", binding)
        self.assertIn("+ -1", binding)
        self.assertIn("OnDemandSpec{coro::SignalSpec<17, true>", binding)
        self.assertIn("OnDemandSpec{coro::ArraySpec<288, 2, 1, true>", binding)

        observed_table = binding.split("kObservedSignalWordIds = {", 1)[1].split(
            "};", 1
        )[0]
        driven_table = binding.split("kDrivenSignalWordIds = {", 1)[1].split(
            "};", 1
        )[0]
        self.assertIn("kSignalHotO", observed_table)
        self.assertNotIn("kSignalU64O", observed_table)
        self.assertNotIn("kSignalArrayO", observed_table)
        self.assertIn("kSignalHotI", driven_table)
        self.assertNotIn("kSignalNarrowI", driven_table)
        self.assertNotIn("kSignalWideI", driven_table)
        self.assertNotIn("kSignalMatrixI", driven_table)

        self.assertIn("localparam int CPPTB_SIGNAL_COUNT = 32", wrapper)
        self.assertIn("localparam int INPUT_WORD_COUNT = 1", wrapper)
        self.assertIn("localparam int OUTPUT_WORD_COUNT = 1", wrapper)
        self.assertNotIn("INPUT_SIGNAL_U64O", wrapper)
        self.assertNotIn("OUTPUT_SIGNAL_WIDEI", wrapper)
        self.assertIn(
            'export "DPI-C" function dpi_sample_dut_port_2_get;', wrapper
        )
        self.assertIn(
            "function longint unsigned dpi_sample_dut_port_3_get();", wrapper
        )
        self.assertIn(
            "function void dpi_sample_dut_port_4_get(output bit [136:0] value);",
            wrapper,
        )
        self.assertIn(
            "function int unsigned dpi_sample_dut_port_5_get(input int index_0);",
            wrapper,
        )
        self.assertIn(
            "matrix_i[index_0][index_1] = value;", wrapper
        )
        self.assertNotIn("in_words[INPUT_SIGNAL_U64O", wrapper)
        self.assertNotIn("out_words[OUTPUT_SIGNAL_MATRIXI", wrapper)

    def test_zero_extends_signed_on_demand_scalar_and_array_getters(self):
        config = manifest()
        del config["clock"]
        config["clocks"] = []
        ports = map_ports(
            [
                Port(
                    "signed_narrow_o", "output", 17, signed=True,
                    transport="on_demand"
                ),
                Port(
                    "signed64_o", "output", 64, signed=True,
                    four_state=False, transport="on_demand"
                ),
                Port(
                    "signed_array_o", "output", 8, signed=True,
                    unpacked=(UnpackedRange(3, 0),),
                    transport="on_demand",
                ),
            ],
            config,
        )

        validate_transport_ports(ports)
        wrapper = render_sv(ports, [], config, "sample.json")

        self.assertIn(
            "dpi_sample_dut_port_0_get = $unsigned(signed_narrow_o);",
            wrapper,
        )
        self.assertIn(
            "dpi_sample_dut_port_1_get = $unsigned(signed64_o);", wrapper
        )
        self.assertIn(
            "dpi_sample_dut_port_2_get = "
            "$unsigned(signed_array_o[index_0]);",
            wrapper,
        )

    def test_check_detects_stale_generated_file(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            output = Path(temp_dir) / "generated.hpp"
            output.write_text("old\n")
            with self.assertRaisesRegex(CodegenError, "generated files are stale"):
                write_or_check({output: "new\n"}, check=True)


if __name__ == "__main__":
    unittest.main()
