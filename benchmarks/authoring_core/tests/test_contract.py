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
        self.assertIn(
            "tools/codegen/cpptb_codegen/rggen_codegen.py", makefile
        )
        authoring_template = makefile.split(
            "define AUTHORING_CORE_DPI_template", 1
        )[1].split("endef", 1)[0]
        self.assertIn("$(CPPTB_CODEGEN_SOURCES)", authoring_template)
        kernel_line = next(
            line for line in makefile.splitlines()
            if line.startswith("AUTHORING_CORE_KERNELS :=")
        )
        self.assertEqual(tuple(kernel_line.split(":=", 1)[1].split()), workload.KERNELS)
        generated_rule = makefile.split(
            "$(AUTHORING_CORE_DPI_GENERATED):", 1
        )[1].split("authoring-core-dpi-codegen:", 1)[0]
        self.assertIn("@test -f $@", generated_rule)
        self.assertNotIn("$(CPPTB_CODEGEN)", generated_rule)
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
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,queue_sync,28))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,test_lifecycle,29))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,dynamic_spawn,30))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,dynamic_monitor,34))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,process_pipeline,41))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,random_stimulus,36))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,constrained_packet,37))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,constraint_extensions,38))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,coverage_sampling,39))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,apb_component,40))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,memory_model,42))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,memory_model_direct,43))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,register_prediction_validity,44))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,register_backdoor,45))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,register_hierarchy,46))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,register_memory,50))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,register_sequences,51))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,register_coverage,52))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,register_maps,53))",
            makefile,
        )
        self.assertIn(
            "$(eval $(call AUTHORING_CORE_DPI_template,register_user_effects,54))",
            makefile,
        )
        # The default is shared with the other suites so every measured binary
        # is optimized identically; this suite can still be overridden alone.
        self.assertIn("CPPTB_BENCH_OPT_FAST ?= -O3", makefile)
        self.assertIn(
            "AUTHORING_CORE_OPT_FAST ?= $(CPPTB_BENCH_OPT_FAST)", makefile
        )
        # Four: the pure-SV testbench, force-direct, the direct-timing build,
        # and the C++ VPI binary, which is a measured mode and so must be built
        # like the modes it is compared against.
        self.assertEqual(
            makefile.count('-MAKEFLAGS "OPT_FAST=$(AUTHORING_CORE_OPT_FAST)"'),
            4,
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
        self.assertEqual(integrated.queue_receive, 1)
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

    def test_random_stimulus_has_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("random_stimulus", 5)
        self.assertEqual(counts.transactions, 5)
        self.assertEqual(counts.checks, 7)
        self.assertEqual(counts.random_stimulus, 5)
        self.assertEqual(
            [
                field
                for field in workload.FEATURE_FIELDS
                if getattr(counts, field) != 0
            ],
            ["random_stimulus"],
        )

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("auto& random = random_test.random();", cpp)
        self.assertIn("random.randint<uint32_t>", cpp)
        self.assertIn("random.weighted_choice(random_masks)", cpp)
        self.assertIn("random.randbits<65>()", cpp)
        self.assertIn("random.shuffle(order)", cpp)
        self.assertIn("function automatic logic [63:0] random_next_u64", sv)
        self.assertIn('"random_stimulus": run_random_stimulus();', sv)

        self.assertEqual(
            workload.expected_checksum(5, kernel="random_stimulus"),
            0xA0CE3058,
        )

    def test_constrained_packet_has_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("constrained_packet", 5)
        self.assertEqual(counts.transactions, 5)
        self.assertEqual(counts.checks, 7)
        self.assertEqual(counts.constrained_packet, 5)
        self.assertEqual(
            [
                field
                for field in workload.FEATURE_FIELDS
                if getattr(counts, field) != 0
            ],
            ["constrained_packet"],
        )

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("class PacketStimulus final : public Randomized", cpp)
        self.assertIn("constrained_test.randomize(packet);", cpp)
        self.assertIn("length % uint16_t{4} == uint16_t{0}", cpp)
        self.assertIn(
            "function automatic logic [31:0] constrained_packet_payload", sv
        )
        self.assertIn('"constrained_packet": run_constrained_packet();', sv)

        self.assertEqual(
            workload.expected_checksum(5, kernel="constrained_packet"),
            0xD9EDB167,
        )

    def test_constraint_extensions_have_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("constraint_extensions", 5)
        self.assertEqual(counts.transactions, 5)
        self.assertEqual(counts.checks, 7)
        self.assertEqual(counts.constraint_extensions, 5)
        self.assertEqual(
            [
                field
                for field in workload.FEATURE_FIELDS
                if getattr(counts, field) != 0
            ],
            ["constraint_extensions"],
        )

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("class ExtendedPacketStimulus final : public Randomized", cpp)
        self.assertIn("inside(opcode, {1, 3, 5})", cpp)
        self.assertIn("dist(length", cpp)
        self.assertIn("RandArray<uint8_t, 2>", cpp)
        self.assertIn("RandBits<65>", cpp)
        self.assertIn("legacy_opcode.disable()", cpp)
        self.assertIn(
            "function automatic logic [31:0] constraint_extensions_payload", sv
        )
        self.assertIn(
            '"constraint_extensions": run_constraint_extensions();', sv
        )
        self.assertEqual(
            workload.expected_checksum(5, kernel="constraint_extensions"),
            0x38D6D2B5,
        )

    def test_coverage_sampling_has_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("coverage_sampling", 5)
        self.assertEqual(counts.transactions, 5)
        self.assertEqual(counts.checks, 12)
        self.assertEqual(counts.coverage_sampling, 5)
        self.assertEqual(
            [
                field
                for field in workload.FEATURE_FIELDS
                if getattr(counts, field) != 0
            ],
            ["coverage_sampling"],
        )

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn('Covergroup<CoverageTransaction>', cpp)
        self.assertIn('transition_bin("read_to_write"', cpp)
        self.assertIn('functional_coverage.cross("opcode_x_length"', cpp)
        self.assertIn("task automatic coverage_sample", sv)
        self.assertIn('"coverage_sampling": run_coverage_sampling();', sv)
        self.assertEqual(
            workload.expected_checksum(5, kernel="coverage_sampling"),
            workload.expected_checksum(5),
        )

    def test_apb_component_has_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("apb_component", 5)
        self.assertEqual(counts.transactions, 10)
        self.assertEqual(counts.checks, 29)
        self.assertEqual(counts.apb_component, 10)
        self.assertEqual(
            [
                field
                for field in workload.FEATURE_FIELDS
                if getattr(counts, field) != 0
            ],
            ["apb_component"],
        )

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("ApbMaster master{bus}", cpp)
        self.assertIn("ApbMonitor monitor{test, bus}", cpp)
        self.assertIn("ApbProtocolChecker checker{test, bus}", cpp)
        self.assertIn("task automatic run_apb_monitor", sv)
        self.assertIn("task automatic run_apb_checker", sv)
        self.assertIn('"apb_component": run_apb_component();', sv)

    def test_transaction_recording_has_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("transaction_recording", 5)
        self.assertEqual(counts.transactions, 10)
        self.assertEqual(counts.checks, 34)
        self.assertEqual(counts.analysis_write, 20)
        self.assertEqual(counts.analysis_delivery, 30)
        self.assertEqual(
            [
                field
                for field in workload.FEATURE_FIELDS
                if getattr(counts, field) != 0
            ],
            ["analysis_write", "analysis_delivery"],
        )

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("InMemoryTransactionSink trace", cpp)
        self.assertIn("MeasuredAnalysisSubscriber measured_stream", cpp)
        self.assertIn("recorder.connect(measured_trace)", cpp)
        self.assertIn("task automatic record_apb_transaction", sv)
        self.assertIn("analysis_write_count++", sv)
        self.assertNotIn("apb_transaction_t transaction_trace[$]", sv)
        self.assertIn('"transaction_recording": run_transaction_recording();', sv)

    def test_memory_model_has_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("memory_model", 5)
        self.assertEqual(counts.transactions, 10)
        self.assertEqual(counts.checks, 29)
        self.assertEqual(counts.memory_model, 10)
        self.assertEqual(
            [
                field
                for field in workload.FEATURE_FIELDS
                if getattr(counts, field) != 0
            ],
            ["memory_model"],
        )

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("SparseMemory memory", cpp)
        self.assertIn("make_memory_predictor<Transaction>", cpp)
        self.assertIn("memory_model_bytes", sv)
        self.assertIn('"memory_model": run_memory_model();', sv)
        self.assertEqual(
            workload.expected_checksum(5, kernel="memory_model"),
            workload.expected_checksum(5, kernel="apb_component"),
        )

    def test_direct_memory_model_has_bus_free_cpp_and_sv_twins(self):
        counts = workload.expected_counts("memory_model_direct", 5)
        self.assertEqual(counts.transactions, 10)
        self.assertEqual(counts.checks, 17)
        self.assertEqual(counts.memory_model_direct, 10)
        self.assertEqual(
            [
                field
                for field in workload.FEATURE_FIELDS
                if getattr(counts, field) != 0
            ],
            ["memory_model_direct"],
        )

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("Task<void> run_memory_model_direct", cpp)
        self.assertIn("memory.write_word", cpp)
        self.assertIn("memory.read_word<uint32_t>", cpp)
        self.assertIn("task automatic run_memory_model_direct", sv)
        self.assertIn(
            '"memory_model_direct": run_memory_model_direct();', sv
        )
        self.assertEqual(
            workload.expected_checksum(5, kernel="memory_model_direct"),
            workload.expected_checksum(5, kernel="memory_model"),
        )

    def test_register_prediction_validity_has_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("register_prediction_validity", 5)
        self.assertEqual(counts.transactions, 0)
        self.assertEqual(counts.checks, 29)
        self.assertEqual(counts.register_prediction_validity, 5)
        self.assertEqual(
            [
                field
                for field in workload.FEATURE_FIELDS
                if getattr(counts, field) != 0
            ],
            ["register_prediction_validity"],
        )

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("Task<void> run_register_prediction_validity", cpp)
        self.assertIn("model.mirrored_valid_mask()", cpp)
        self.assertIn("observed.connect(predictor)", cpp)
        self.assertIn("task automatic run_register_prediction_validity", sv)
        self.assertIn(
            '"register_prediction_validity": run_register_prediction_validity();',
            sv,
        )

    def test_register_backdoor_has_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("register_backdoor", 5)
        self.assertEqual(counts.transactions, 0)
        self.assertEqual(counts.checks, 7)
        self.assertEqual(counts.register_backdoor, 5)
        self.assertEqual(
            [
                field
                for field in workload.FEATURE_FIELDS
                if getattr(counts, field) != 0
            ],
            ["register_backdoor"],
        )

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("Task<void> run_register_backdoor", cpp)
        self.assertIn('cpptb_signal<"pending_data">', cpp)
        self.assertIn("model.poke(value)", cpp)
        self.assertIn("task automatic run_register_backdoor", sv)
        self.assertIn('"register_backdoor": run_register_backdoor();', sv)

    def test_register_hierarchy_has_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("register_hierarchy", 5)
        self.assertEqual(counts.transactions, 0)
        self.assertEqual(counts.checks, 27)
        self.assertEqual(counts.register_hierarchy, 5)
        self.assertEqual(
            [
                field
                for field in workload.FEATURE_FIELDS
                if getattr(counts, field) != 0
            ],
            ["register_hierarchy"],
        )

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("Task<void> run_register_hierarchy", cpp)
        self.assertIn("RegisterViewArray lanes", cpp)
        self.assertIn("lanes.for_each", cpp)
        self.assertIn("task automatic run_register_hierarchy", sv)
        self.assertIn('"register_hierarchy": run_register_hierarchy();', sv)

    def test_register_split_has_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("register_split", 5)
        self.assertEqual(counts.transactions, 20)
        self.assertEqual(counts.checks, 12)
        self.assertEqual(counts.register_split, 5)
        self.assertEqual(
            workload.expected_checksum(5, kernel="register_split"),
            0x811C9DC5,
        )
        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("Task<void> run_register_split", cpp)
        self.assertIn(".width = 32", cpp)
        self.assertIn(".access_width = 16", cpp)
        self.assertIn("task automatic run_register_split", sv)
        self.assertIn('"register_split": run_register_split();', sv)

    def test_register_wide_has_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("register_wide", 5)
        self.assertEqual(counts.transactions, 100)
        self.assertEqual(counts.checks, 32)
        self.assertEqual(counts.register_wide, 5)
        self.assertEqual(
            workload.expected_checksum(5, kernel="register_wide"),
            0x811C9DC5,
        )
        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("Task<void> run_register_wide", cpp)
        self.assertIn("WideRegisterHandle<128", cpp)
        self.assertIn("WideRegisterMemoryHandle<128", cpp)
        self.assertIn("WideRegisterPredictor predictor", cpp)
        self.assertIn("model.poke(value)", cpp)
        self.assertIn("task automatic run_register_wide", sv)
        self.assertIn("logic [127:0] predicted", sv)
        self.assertIn('"register_wide": run_register_wide();', sv)

    def test_register_enum_has_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("register_enum", 5)
        self.assertEqual(counts.transactions, 10)
        self.assertEqual(counts.checks, 12)
        self.assertEqual(counts.register_enum, 5)
        self.assertEqual(
            workload.expected_checksum(5, kernel="register_enum"),
            0x811C9DC5,
        )
        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("Task<void> run_register_enum", cpp)
        self.assertIn("RegisterEnumFieldHandle<BenchmarkMode", cpp)
        self.assertIn("task automatic run_register_enum", sv)
        self.assertIn('"register_enum": run_register_enum();', sv)

    def test_register_memory_has_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("register_memory", 5)
        self.assertEqual(counts.transactions, 0)
        self.assertEqual(counts.checks, 32)
        self.assertEqual(
            workload.expected_checksum(5, kernel="register_memory"),
            0x7AF17C29,
        )
        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        generated_header = (
            BENCH_DIR / "testbenches/cpp_dpi/generated/authoring_core_dut.hpp"
        ).read_text(encoding="utf-8")
        generated_wrapper = (
            BENCH_DIR / "testbenches/cpp_dpi/generated/dpi_authoring_core.sv"
        ).read_text(encoding="utf-8")
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("Task<void> run_register_memory", cpp)
        self.assertIn("RegisterMemoryHandle memory", cpp)
        self.assertIn("AccessPath::Backdoor", cpp)
        self.assertIn("memory.write_offset", cpp)
        self.assertIn("memory.read_absolute", cpp)
        self.assertIn("memory.base_address()", cpp)
        self.assertIn("dut_.memory.get_into", cpp)
        self.assertIn("dut_.memory.deposit", cpp)
        self.assertIn("get_block4", generated_header)
        self.assertIn("deposit_block4", generated_header)
        self.assertIn("get_block4", generated_wrapper)
        self.assertIn("deposit_block4", generated_wrapper)
        self.assertIn("task automatic run_register_memory", sv)
        self.assertIn("selected_index = byte_offset / 4", sv)
        self.assertIn("absolute_address - 64'h0000_0000_0000_4100", sv)
        self.assertIn('"register_memory": run_register_memory();', sv)

    def test_register_sequences_have_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("register_sequences", 5)
        self.assertEqual(counts.transactions, 105)
        self.assertEqual(counts.checks, 232)
        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("Task<void> run_register_sequences", cpp)
        self.assertIn("register_reset_check(test, model)", cpp)
        self.assertIn("register_access_check(test, model)", cpp)
        self.assertIn("register_bit_bash(test, model)", cpp)
        self.assertIn("task automatic run_register_sequences", sv)
        self.assertIn('"register_sequences": run_register_sequences();', sv)
        self.assertIn("frontdoor bit-bash value", sv)
        self.assertIn("backdoor bit-bash value", sv)

    def test_register_coverage_has_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("register_coverage", 5)
        self.assertEqual(counts.transactions, 50)
        self.assertEqual(counts.checks, 12)
        self.assertEqual(counts.coverage_sampling, 5)
        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("Task<void> run_register_coverage", cpp)
        self.assertIn("RegisterAccessCoverage coverage", cpp)
        self.assertIn("coverage.sample_memory", cpp)
        self.assertIn("task automatic run_register_coverage", sv)
        self.assertIn('"register_coverage": run_register_coverage();', sv)
        self.assertIn("memory_backdoor_writes", sv)

    def test_register_maps_have_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("register_maps", 5)
        self.assertEqual(counts.transactions, 50)
        self.assertEqual(counts.checks, 42)
        self.assertEqual(
            workload.expected_checksum(5, kernel="register_maps"),
            0x79653529,
        )
        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("Task<void> run_register_maps", cpp)
        self.assertIn("RegisterAddressMap primary", cpp)
        self.assertIn("RegisterAddressMap alias", cpp)
        self.assertIn("BenchmarkRegisterFrontdoor custom", cpp)
        self.assertIn("memory.read_into(index, readback, alias)", cpp)
        self.assertIn("task automatic run_register_maps", sv)
        self.assertIn('"register_maps": run_register_maps();', sv)
        self.assertIn("custom frontdoor read", sv)

    def test_register_user_effects_have_exact_cpp_and_sv_twins(self):
        counts = workload.expected_counts("register_user_effects", 5)
        self.assertEqual(counts.transactions, 10)
        self.assertEqual(counts.checks, 22)
        self.assertEqual(
            workload.expected_checksum(5, kernel="register_user_effects"),
            0xC4BA90E2,
        )
        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("Task<void> run_register_user_effects", cpp)
        self.assertIn("BenchmarkUserEffectPolicy", cpp)
        self.assertIn("RegisterWriteEffect::User", cpp)
        self.assertIn("RegisterReadEffect::User", cpp)
        self.assertIn("task automatic run_register_user_effects", sv)
        self.assertIn(
            '"register_user_effects": run_register_user_effects();', sv
        )
        self.assertIn("written_value = mirrored ^ desired_value", sv)

    def test_queue_sync_has_exact_isolated_counts_and_twin(self):
        counts = workload.expected_counts("queue_sync", 5)
        self.assertEqual(counts.transactions, 5)
        self.assertEqual(counts.checks, 12)
        self.assertEqual(counts.queue_put, 5)
        self.assertEqual(counts.queue_get, 5)
        self.assertEqual(counts.lock_acquire, 5)
        self.assertEqual(counts.semaphore_acquire, 5)
        enabled = [
            field
            for field in workload.FEATURE_FIELDS
            if getattr(counts, field) != 0
        ]
        self.assertEqual(
            enabled,
            ["queue_put", "queue_get", "lock_acquire", "semaphore_acquire"],
        )

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("Queue<uint32_t> queue{1};", cpp)
        self.assertIn("Semaphore credits{2};", cpp)
        self.assertIn("co_await lock.acquire();", cpp)
        self.assertIn("bounded_queue = new(1);", sv)
        self.assertIn("queue_credits = new(2);", sv)
        self.assertIn("authored_lock.get(1);", sv)

    def test_lifecycle_has_exact_systemverilog_twin(self):
        counts = workload.expected_counts("test_lifecycle", 5)
        self.assertEqual(counts.transactions, 0)
        self.assertEqual(counts.checks, 15)
        self.assertEqual(counts.spawned_processes, 1)
        self.assertEqual(counts.test_lifecycle, 5)
        self.assertEqual(
            [
                field
                for field in workload.FEATURE_FIELDS
                if getattr(counts, field) != 0
            ],
            ["test_lifecycle"],
        )

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("test.expect(\"stimulus is nonzero\"", cpp)
        self.assertIn("test.expect_eq(\"stimulus identity\"", cpp)
        self.assertIn("test.spawn(lifecycle_process", cpp)
        self.assertIn("co_await process;", cpp)
        self.assertIn("task automatic lifecycle_process", sv)
        self.assertIn("task automatic run_test_lifecycle();", sv)
        self.assertIn('kernel != "test_lifecycle"', sv)
        self.assertIn("kernel=test_lifecycle iterations=%0d", sv)

    def test_dynamic_process_kernels_have_exact_systemverilog_twins(self):
        for kernel in (
            "dynamic_task",
            "dynamic_spawn_scheduler",
            "dynamic_spawn",
            "dynamic_spawn_suspending",
        ):
            with self.subTest(kernel=kernel):
                counts = workload.expected_counts(kernel, 5)
                self.assertEqual(counts.transactions, 0)
                self.assertEqual(counts.checks, 5)
                self.assertEqual(counts.dynamic_spawn, 5)
                expected_processes = 5
                if kernel == "dynamic_task":
                    expected_processes = 0
                elif kernel == "dynamic_spawn_suspending":
                    expected_processes = 10
                self.assertEqual(counts.spawned_processes, expected_processes)
                self.assertEqual(
                    [
                        field
                        for field in workload.FEATURE_FIELDS
                        if getattr(counts, field) != 0
                    ],
                    ["dynamic_spawn"],
                )

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("test.spawn(dynamic_spawn_child", cpp)
        self.assertIn("co_await child;", cpp)
        self.assertIn("co_await dynamic_task_child", cpp)
        self.assertIn("context.scheduler.spawn(\n            dynamic_scheduler_child", cpp)
        self.assertIn("test.spawn(dynamic_suspending_child", cpp)
        self.assertIn("test.spawn(dynamic_suspending_release", cpp)
        self.assertIn("task automatic dynamic_spawn_child", sv)
        self.assertIn("dynamic_spawn_child(value, i);", sv)
        self.assertIn("task automatic report_dynamic_process();", sv)
        self.assertIn("kernel=%s iterations=%0d", sv)
        self.assertIn("task automatic run_dynamic_task();", sv)
        self.assertIn("task automatic run_dynamic_spawn_scheduler();", sv)
        self.assertIn("task automatic run_dynamic_spawn();", sv)
        self.assertIn("task automatic run_dynamic_spawn_suspending();", sv)
        self.assertIn("dynamic_suspending_child(value, i);", sv)
        self.assertIn("dynamic_suspending_release();", sv)

    def test_dynamic_monitor_has_exact_systemverilog_twin(self):
        counts = workload.expected_counts("dynamic_monitor", 5)
        self.assertEqual(counts.transactions, 5)
        self.assertEqual(counts.checks, 8)
        self.assertEqual(counts.spawned_processes, 2)
        self.assertEqual(counts.queue_put, 5)
        self.assertEqual(counts.queue_get, 5)

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        self.assertIn("test.spawn(response_monitor", cpp)
        self.assertIn("test.spawn(response_edge_watcher", cpp)
        self.assertIn("Queue<uint32_t> observed{8};", cpp)
        self.assertIn("monitor.cancel();", cpp)
        self.assertIn("watcher.cancel();", cpp)
        self.assertIn("dynamic_monitor_queue = new(8);", sv)
        self.assertIn("fork : dynamic_monitor_processes", sv)
        self.assertIn("disable dynamic_monitor_processes;", sv)
        self.assertIn('"dynamic_monitor": run_dynamic_monitor();', sv)

    def test_process_pipeline_has_exact_finite_process_twin(self):
        counts = workload.expected_counts("process_pipeline", 5)
        self.assertEqual(counts.transactions, 5)
        self.assertEqual(counts.checks, 9)
        self.assertEqual(counts.spawned_processes, 3)
        self.assertEqual(counts.queue_put, 10)
        self.assertEqual(counts.queue_get, 10)
        self.assertEqual(counts.dynamic_spawn, 0)

        cpp = (BENCH_DIR / "testbenches/cpp_dpi/testbench.cpp").read_text(
            encoding="utf-8"
        )
        sv = (
            BENCH_DIR / "testbenches/systemverilog/authoring_core_sv_tb.sv"
        ).read_text(encoding="utf-8")
        for task in (
            "process_pipeline_driver",
            "process_pipeline_worker",
            "process_pipeline_scoreboard",
        ):
            self.assertIn(f"test.spawn(\n        {task}", cpp)
            self.assertIn(f"task automatic {task}();", sv)
        self.assertIn("Queue<uint32_t> expected_values{8};", cpp)
        self.assertIn("Queue<uint32_t> observed_values{8};", cpp)
        self.assertIn("co_await driver;", cpp)
        self.assertIn("co_await worker;", cpp)
        self.assertIn("co_await scoreboard;", cpp)
        self.assertIn("process_expected_queue = new(8);", sv)
        self.assertIn("process_observed_queue = new(8);", sv)
        self.assertIn('"process_pipeline": run_process_pipeline();', sv)

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
        for field in workload.RESULT_FIELDS:
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
            runner.parse_result(complete.replace(" queue_receive=0", ""))
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
