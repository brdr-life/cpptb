import json
import sys
import unittest
from pathlib import Path


BENCH_DIR = Path(__file__).resolve().parents[1]
REPO = BENCH_DIR.parents[1]
sys.path.insert(0, str(BENCH_DIR))

import run_benchmark as runner  # noqa: E402
import run_timing_backend_experiments as timing_experiments  # noqa: E402
import workload  # noqa: E402


def result_line(kernel="control", iterations=1, **overrides):
    expected = workload.expected_counts(kernel, iterations).fields()
    fields = {
        "mode": "cpp_dpi",
        "kernel": kernel,
        **expected,
        "sim_cycles": 7,
        "checksum": workload.expected_checksum(iterations, kernel=kernel),
        "failures": 0,
        "internal_wall_ms": 1.25,
    }
    fields.update(overrides)
    return "AUTHORING_CORE_RESULT " + " ".join(
        f"{key}={fields[key]}" for key in workload.RESULT_FIELDS
    )


class ContractTests(unittest.TestCase):
    def test_timing_backend_experiment_matrix_is_complete(self):
        self.assertEqual(
            tuple(timing_experiments.BACKENDS),
            (
                "direct",
                "portable-vpi",
                "sv-dpi-inline",
                "sv-dpi-nba",
                "sv-dpi-calendar",
            ),
        )

        makefile = (REPO / "Makefile").read_text(encoding="utf-8")
        self.assertIn("authoring-core-timing-experiments-build:", makefile)
        self.assertIn("-DCPPTB_SV_DPI_TIMING", makefile)
        self.assertIn("-DCPPTB_SV_DPI_NBA_TIMING", makefile)
        self.assertIn("-DCPPTB_SV_DPI_CALENDAR_TIMING", makefile)

    def test_makefile_builds_every_authoring_kernel(self):
        makefile = (REPO / "Makefile").read_text(encoding="utf-8")
        kernel_line = next(
            line for line in makefile.splitlines()
            if line.startswith("AUTHORING_CORE_KERNELS :=")
        )
        self.assertEqual(tuple(kernel_line.split(":=", 1)[1].split()), workload.KERNELS)
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,task_timeout,8))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,wide_echo_137,10))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,fixed_mac,12))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,array_index,13))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,array_wide,14))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,mem_rw,15))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,hier_probe,16))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,mem_backdoor,17))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,mem_probe_read,18))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,mem_probe_deposit,19))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,mem_probe_read_deposit,20))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,signal_edge,21))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,array_multidim,22))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,force_release,23))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,force_direct,25))",
            makefile,
        )
        self.assertIn("AUTHORING_CORE_OPT_FAST ?= -O3", makefile)
        self.assertEqual(
            makefile.count('-MAKEFLAGS "OPT_FAST=$(AUTHORING_CORE_OPT_FAST)"'),
            3,
        )
        self.assertEqual(
            makefile.count('-MAKEFLAGS "OPT_FAST=$$(AUTHORING_CORE_OPT_FAST)"'),
            2,
        )

    def test_boundary_counts_one_iteration(self):
        control = workload.expected_counts("control", 1)
        self.assertEqual(control.transactions, 1)
        self.assertEqual(control.checks, 3)
        self.assertEqual(control.timeout_hits, 0)

        integrated = workload.expected_counts("all", 1)
        self.assertEqual(integrated.checks, 30)
        self.assertEqual(integrated.task_value, 1)
        self.assertEqual(integrated.clock_cycles, 1)
        self.assertEqual(integrated.timeouts, 1)
        self.assertEqual(integrated.timeout_hits, 0)
        self.assertEqual(integrated.task_timeouts, 1)
        self.assertEqual(integrated.task_timeout_hits, 0)
        self.assertEqual(integrated.event_set, 1)
        self.assertEqual(integrated.channel_receive, 1)
        self.assertEqual(integrated.wide64, 1)
        self.assertEqual(integrated.wide_echo_137, 1)
        self.assertEqual(integrated.wide_slice, 1)
        self.assertEqual(integrated.fixed_mac, 1)
        self.assertEqual(integrated.array_index, 1)
        self.assertEqual(integrated.array_wide, 1)
        self.assertEqual(integrated.mem_rw, 1)
        self.assertEqual(integrated.hier_probe_reads, 2)
        self.assertEqual(integrated.hier_probe_deposits, 1)
        self.assertEqual(integrated.mem_backdoor_reads, 1)
        self.assertEqual(integrated.mem_backdoor_deposits, 1)

    def test_boundary_counts_even_and_odd(self):
        self.assertEqual(workload.expected_counts("timeout", 2).timeout_hits, 1)
        self.assertEqual(workload.expected_counts("timeout", 3).timeout_hits, 1)
        self.assertEqual(workload.expected_counts("timeout", 4).timeout_hits, 2)
        self.assertEqual(
            workload.expected_counts("task_timeout", 2).task_timeout_hits, 1
        )
        self.assertEqual(
            workload.expected_counts("task_timeout", 3).task_timeout_hits, 1
        )
        self.assertEqual(
            workload.expected_counts("task_timeout", 4).task_timeout_hits, 2
        )
        self.assertEqual(workload.expected_counts("all", 3).checks, 86)

    def test_wide_and_fixed_kernels_have_isolated_counts(self):
        for kernel in ("wide64", "wide_echo_137", "wide_slice", "fixed_mac"):
            with self.subTest(kernel=kernel):
                counts = workload.expected_counts(kernel, 5)
                self.assertEqual(counts.transactions, 5)
                self.assertEqual(counts.checks, 12)
                self.assertEqual(getattr(counts, kernel), 5)
                enabled = [
                    field
                    for field in workload.FEATURE_FIELDS
                    if getattr(counts, field) != 0
                ]
                self.assertEqual(enabled, [kernel])

    def test_array_and_memory_kernels_have_isolated_counts(self):
        expected_checks = {
            "array_index": 47,
            "array_wide": 27,
            "mem_rw": 12,
        }
        for kernel, checks in expected_checks.items():
            with self.subTest(kernel=kernel):
                counts = workload.expected_counts(kernel, 5)
                self.assertEqual(counts.transactions, 5)
                self.assertEqual(counts.checks, checks)
                self.assertEqual(getattr(counts, kernel), 5)
                enabled = [
                    field
                    for field in workload.FEATURE_FIELDS
                    if getattr(counts, field) != 0
                ]
                self.assertEqual(enabled, [kernel])

    def test_task_timeout_has_exact_isolated_feature_counts(self):
        counts = workload.expected_counts("task_timeout", 5)
        self.assertEqual(counts.transactions, 5)
        self.assertEqual(counts.checks, 12)
        self.assertEqual(counts.task_timeouts, 5)
        self.assertEqual(counts.task_timeout_hits, 2)
        self.assertEqual(counts.timeouts, 0)
        self.assertEqual(counts.task_value, 0)

    def test_probe_kernels_have_exact_isolated_counts(self):
        for kernel, enabled_fields in (
            ("hier_probe", ["hier_probe_reads", "hier_probe_deposits"]),
            ("mem_backdoor", ["mem_backdoor_reads", "mem_backdoor_deposits"]),
        ):
            with self.subTest(kernel=kernel):
                counts = workload.expected_counts(kernel, 5)
                self.assertEqual(counts.transactions, 5)
                self.assertEqual(counts.checks, 17)
                enabled = [
                    field
                    for field in workload.FEATURE_FIELDS
                    if getattr(counts, field) != 0
                ]
                self.assertEqual(enabled, enabled_fields)

    def test_probe_attribution_kernels_have_exact_counts(self):
        expected = {
            "mem_probe_read": (5, 0, 17),
            "mem_probe_deposit": (0, 5, 12),
            "mem_probe_read_deposit": (5, 5, 17),
        }
        for kernel, (reads, deposits, checks) in expected.items():
            with self.subTest(kernel=kernel):
                counts = workload.expected_counts(kernel, 5)
                self.assertEqual(counts.transactions, 5)
                self.assertEqual(counts.probe_diag_reads, reads)
                self.assertEqual(counts.probe_diag_deposits, deposits)
                self.assertEqual(counts.checks, checks)

    def test_signal_edge_has_exact_isolated_counts(self):
        counts = workload.expected_counts("signal_edge", 5)
        self.assertEqual(counts.transactions, 5)
        self.assertEqual(counts.checks, 7)
        self.assertEqual(counts.signal_edges, 5)
        self.assertEqual(
            [
                field
                for field in workload.FEATURE_FIELDS
                if getattr(counts, field) != 0
            ],
            ["signal_edges"],
        )
        self.assertEqual(workload.expected_counts("all", 5).signal_edges, 0)

    def test_signal_edge_observer_and_exact_waits_are_registered(self):
        manifest = json.loads(
            (BENCH_DIR / "testbenches/cpp_dpi/authoring_core.dpi.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(manifest["edge_observers"], ["rsp_valid"])
        self.assertEqual(manifest["clocks"], [])
        self.assertTrue(manifest["auto_edge_observers"])
        self.assertTrue(manifest["run"]["dynamic_clocks"])

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(encoding="utf-8")
        sv = (BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv").read_text(
            encoding="utf-8"
        )
        self.assertIn("co_await RisingEdge{context.dut.rsp_valid};", cpp)
        self.assertIn("clocks.start(dut.clk, 2_ns);", cpp)
        self.assertIn("task automatic run_signal_edge();", sv)
        self.assertIn("@(posedge rsp_valid);", sv)
        self.assertIn("#1ns clk = ~clk;", sv)
        self.assertIn("if (clk) sim_cycles++;", sv)

    def test_array_multidim_has_exact_isolated_counts(self):
        counts = workload.expected_counts("array_multidim", 5)
        self.assertEqual(counts.transactions, 5)
        self.assertEqual(counts.checks, 37)
        self.assertEqual(counts.array_multidim, 5)
        self.assertEqual(
            [
                field
                for field in workload.FEATURE_FIELDS
                if getattr(counts, field) != 0
            ],
            ["array_multidim"],
        )
        self.assertEqual(workload.expected_counts("all", 5).array_multidim, 0)

    def test_array_multidim_authors_exact_rank_two_accesses(self):
        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(encoding="utf-8")
        sv = (BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv").read_text(
            encoding="utf-8"
        )
        rtl = (BENCH_DIR / "rtl/authoring_core_dut.sv").read_text(
            encoding="utf-8"
        )
        self.assertIn("bit [64:0] array_multidim_i [2:1][-1:1]", rtl)
        self.assertIn(
            "context.dut.array_multidim_i.at(row).at(column).set(", cpp
        )
        self.assertIn(
            "context.dut.array_multidim_o.at(row).at(column).get()", cpp
        )
        self.assertIn("array_multidim_i[row][column] =", sv)
        self.assertIn("check65(array_multidim_o[row][column]", sv)

    def test_force_release_has_exact_isolated_contract(self):
        counts = workload.expected_counts("force_release", 5)
        self.assertEqual(counts.transactions, 5)
        self.assertEqual(counts.checks, 17)
        self.assertEqual(counts.force_release, 5)
        self.assertEqual(
            [
                field
                for field in workload.FEATURE_FIELDS
                if getattr(counts, field) != 0
            ],
            ["force_release"],
        )
        self.assertEqual(workload.expected_counts("all", 5).force_release, 0)

    def test_force_release_uses_dedicated_net_and_exact_delays(self):
        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(encoding="utf-8")
        sv = (BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv").read_text(
            encoding="utf-8"
        )
        rtl = (BENCH_DIR / "rtl/authoring_core_dut.sv").read_text(
            encoding="utf-8"
        )
        self.assertIn("input  bit [31:0] force_source_i", rtl)
        self.assertIn("wire [31:0] force_target = force_source_i", rtl)
        self.assertIn("assign force_fanout_o = force_target;", rtl)
        self.assertIn("context.dut.force_target.force(forced);", cpp)
        self.assertIn("context.dut.force_target.release();", cpp)
        self.assertIn("force i_dut.force_target = value;", sv)
        self.assertIn("release i_dut.force_target;", sv)

        cpp_feature = cpp[
            cpp.index("Task<void> force_release_feature"):
            cpp.index("Task<void> packed_view_feature")
        ]
        sv_feature = sv[
            sv.index("task automatic force_release_feature"):
            sv.index("task automatic packed_view_feature")
        ]
        self.assertEqual(cpp_feature.count("co_await Delay{1_ps};"), 2)
        self.assertEqual(sv_feature.count("#1ps;"), 2)

    def test_force_direct_is_an_exact_zero_time_twin(self):
        counts = workload.expected_counts("force_direct", 5)
        self.assertEqual(counts.transactions, 0)
        self.assertEqual(counts.checks, 5)
        self.assertEqual(counts.force_release, 5)
        self.assertEqual(
            workload.expected_checksum(5, kernel="force_direct"),
            0x811C9DC5,
        )

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/force_direct_sv_tb.sv"
        ).read_text(encoding="utf-8")
        cpp_feature = cpp[
            cpp.index("Task<void> run_force_direct"):
            cpp.index("Task<void> run_timing_phases")
        ]
        self.assertIn("force_target.force(forced);", cpp_feature)
        self.assertIn("force_target.get(), forced", cpp_feature)
        self.assertIn("force_target.release();", cpp_feature)
        self.assertNotIn("co_await", cpp_feature)
        self.assertIn("force i_dut.force_target = value;", sv)
        self.assertIn("if (i_dut.force_target != value)", sv)
        self.assertIn("release i_dut.force_target;", sv)
        self.assertNotIn("#1", sv)

    def test_packed_view_has_exact_isolated_counts_and_twin(self):
        counts = workload.expected_counts("packed_view", 5)
        self.assertEqual(counts.transactions, 5)
        self.assertEqual(counts.checks, 27)
        self.assertEqual(counts.packed_view, 5)
        self.assertEqual(workload.expected_counts("all", 5).packed_view, 0)

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(encoding="utf-8")
        sv = (BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv").read_text(
            encoding="utf-8"
        )
        self.assertIn("PacketTValue::from_signal_value", cpp)
        self.assertIn("packet.view().inner()", cpp)
        self.assertIn("set_state(StateT::StateRun)", cpp)
        self.assertIn("task automatic packed_view_feature", sv)
        self.assertIn("value.state = STATE_RUN;", sv)

    def test_hier_data_has_exact_isolated_counts_and_twin(self):
        counts = workload.expected_counts("hier_data", 5)
        self.assertEqual(counts.transactions, 5)
        self.assertEqual(counts.checks, 17)
        self.assertEqual(counts.hier_data_reads, 10)
        self.assertEqual(counts.hier_data_deposits, 10)
        self.assertEqual(workload.expected_counts("all", 5).hier_data_reads, 0)

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("context.dut.hierarchy_wide.deposit(wide);", cpp)
        self.assertIn("context.dut.hierarchy_logic.deposit_logic(logic);", cpp)
        self.assertIn("context.dut.hierarchy_logic.get_logic()", cpp)
        self.assertIn("i_dut.hierarchy_wide = wide;", sv)
        self.assertIn("i_dut.hierarchy_logic = logic_value;", sv)

    def test_force_direct_result_keeps_complete_feature_schema(self):
        source = (
            BENCH_DIR / "testbenches/systemverilog/force_direct_sv_tb.sv"
        ).read_text(encoding="utf-8")
        for field in workload.FEATURE_FIELDS:
            self.assertIn(f"{field}=", source)

    def test_expected_checksum_is_stable(self):
        self.assertEqual(workload.expected_checksum(1), 1_407_418_725)
        self.assertEqual(workload.expected_checksum(3), 932_421_457)

    def test_parser_and_contract_accept_complete_result(self):
        result = runner.parse_result(
            result_line("all", 3), "cpp_dpi", "all", 3
        )
        runner.validate_contract(result)

    def test_parser_rejects_missing_duplicate_and_nonfinite_fields(self):
        complete = result_line()
        with self.assertRaisesRegex(ValueError, "missing fields"):
            runner.parse_result(complete.replace(" channel_receive=0", ""))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            runner.parse_result(complete + " checks=3")
        with self.assertRaisesRegex(ValueError, "finite"):
            runner.parse_result(complete + " internal_wall_ms=nan")
        with self.assertRaisesRegex(ValueError, "non-negative"):
            runner.parse_result(complete.replace("failures=0", "failures=-1"))

    def test_contract_rejects_count_checksum_and_failure_mismatches(self):
        for override, message in (
            ({"transactions": 2}, "transactions"),
            ({"checksum": 0}, "checksum"),
            ({"failures": 1}, "reported"),
        ):
            result = runner.parse_result(result_line(**override))
            with self.assertRaisesRegex(ValueError, message):
                runner.validate_contract(result)

    def test_invalid_contract_arguments(self):
        with self.assertRaises(ValueError):
            workload.expected_counts("missing", 1)
        with self.assertRaises(ValueError):
            workload.expected_counts("control", 0)
        with self.assertRaises(ValueError):
            workload.expected_checksum(0)


if __name__ == "__main__":
    unittest.main()
