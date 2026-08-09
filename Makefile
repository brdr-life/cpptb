BUILD_DIR := build
.DEFAULT_GOAL := help

OBJ_DIR := $(BUILD_DIR)/obj
MOJOTB_BUILD_DIR := $(BUILD_DIR)/mojotb
MOJOTB_OBJ_DIR := $(MOJOTB_BUILD_DIR)/obj
CPPTB_BUILD_DIR := $(BUILD_DIR)/cpptb
CPPTB_OBJ_DIR := $(CPPTB_BUILD_DIR)/obj
CPPTB_PUBLIC_HEADERS := $(sort $(shell find include/cpptb include/cpptb_vc \
	-type f -name '*.hpp'))
CPPTB_SV_LOGGING_ASSETS := \
	include/cpptb/sv/cpptb_log_pkg.sv \
	include/cpptb/sv/cpptb_log.svh \
	include/cpptb/sv/cpptb_sv_log_bridge.cpp
CPPTB_CORO_RUNTIME_TEST := $(CPPTB_BUILD_DIR)/coro_runtime_test
CPPTB_PACKED_VALUE_TEST := $(CPPTB_BUILD_DIR)/packed_value_test
CPPTB_RANDOM_TEST := $(CPPTB_BUILD_DIR)/random_test
CPPTB_RANDOMIZED_TEST := $(CPPTB_BUILD_DIR)/randomized_test
CPPTB_Z3_RANDOM_TEST := $(CPPTB_BUILD_DIR)/z3_random_backend_test
CPPTB_COVERAGE_TEST := $(CPPTB_BUILD_DIR)/coverage_test
CPPTB_TEST_API_TEST := $(CPPTB_BUILD_DIR)/test_api_test
CPPTB_COMPONENTS_TEST := $(CPPTB_BUILD_DIR)/components_test
CPPTB_TRANSACTION_RECORDING_TEST := $(CPPTB_BUILD_DIR)/transaction_recording_test
CPPTB_MEMORY_MODEL_TEST := $(CPPTB_BUILD_DIR)/memory_model_test
CPPTB_REGISTER_MODEL_TEST := $(CPPTB_BUILD_DIR)/register_model_test
CPPTB_REGISTER_SEQUENCES_TEST := $(CPPTB_BUILD_DIR)/register_sequences_test
CPPTB_REGISTER_COVERAGE_TEST := $(CPPTB_BUILD_DIR)/register_coverage_test
CPPTB_HIERARCHY_TEST := $(CPPTB_BUILD_DIR)/hierarchy_test
CPPTB_APB_EVENT_OBJ_DIR := $(CPPTB_BUILD_DIR)/apb_event_obj
CPPTB_CONFORMANCE_DIR := tests/conformance/runtime
CPPTB_CONFORMANCE_MANIFEST := $(CPPTB_CONFORMANCE_DIR)/scheduler_conformance.dpi.json
CPPTB_CONFORMANCE_RUNNER := $(CPPTB_CONFORMANCE_DIR)/run_conformance.py
CPPTB_CONFORMANCE_BINARY := $(CPPTB_BUILD_DIR)/conformance_obj/Vdpi_scheduler_conformance
CPPTB_BENCH_BUILD_DIR := $(BUILD_DIR)/experiments/cocotb_cpp_comparison
DOCS_BUILD_DIR := $(BUILD_DIR)/docs
SPHINX_DOCS_DIR := $(DOCS_BUILD_DIR)/sphinx
ZENSICAL_DOCS_DIR := $(DOCS_BUILD_DIR)/zensical
PERIPHERAL_SUITE_BUILD_DIR := $(BUILD_DIR)/benchmarks/peripheral_suite
PERIPHERAL_SUITE_OPT_FAST ?= $(CPPTB_BENCH_OPT_FAST)
PERIPHERAL_SUITE_VPI_OBJ_DIR := $(PERIPHERAL_SUITE_BUILD_DIR)/cpp_vpi_obj
PERIPHERAL_SUITE_SV_OBJ_DIR := $(PERIPHERAL_SUITE_BUILD_DIR)/pure_sv_obj
PERIPHERAL_SUITE_DPI_OBJ_DIR := $(PERIPHERAL_SUITE_BUILD_DIR)/cpp_dpi_obj
PERIPHERAL_SUITE_DPI_MANIFEST := benchmarks/peripheral_suite/testbenches/cpp_dpi/peripheral_suite.dpi.json
CPPTB_CODEGEN_ENTRY := tools/codegen/cpptb_codegen/generate_dpi_bindings.py
PERIPHERAL_SUITE_DPI_GENERATOR := $(CPPTB_CODEGEN_ENTRY)
AUTHORING_CORE_DIR := benchmarks/authoring_core
AUTHORING_CORE_BUILD_DIR := $(BUILD_DIR)/benchmarks/authoring_core
AUTHORING_CORE_SV_OBJ_DIR := $(AUTHORING_CORE_BUILD_DIR)/pure_sv_obj
AUTHORING_CORE_TIMING_EXPERIMENT_DIR := $(AUTHORING_CORE_BUILD_DIR)/timing_backends
AUTHORING_CORE_TIMING_INLINE_DIR := $(AUTHORING_CORE_TIMING_EXPERIMENT_DIR)/sv_dpi_inline
AUTHORING_CORE_TIMING_NBA_DIR := $(AUTHORING_CORE_TIMING_EXPERIMENT_DIR)/sv_dpi_nba
AUTHORING_CORE_TIMING_CALENDAR_DIR := $(AUTHORING_CORE_TIMING_EXPERIMENT_DIR)/sv_dpi_calendar
AUTHORING_CORE_TIMING_VPI_DIR := $(AUTHORING_CORE_TIMING_EXPERIMENT_DIR)/portable_vpi
AUTHORING_CORE_DPI_MANIFEST := $(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/authoring_core.dpi.json
AUTHORING_CORE_DPI_GENERATOR := $(CPPTB_CODEGEN_ENTRY)
AUTHORING_CORE_BUILD_PROVENANCE := $(AUTHORING_CORE_DIR)/build_provenance.py
AUTHORING_CORE_DPI_CODEGEN_STAMP := $(AUTHORING_CORE_BUILD_DIR)/cpp_dpi.codegen.stamp
AUTHORING_CORE_CLOCK_CONFIG := $(AUTHORING_CORE_BUILD_DIR)/cpp_dpi.clocks.json
AUTHORING_CORE_ACCESS_CONFIG := $(AUTHORING_CORE_BUILD_DIR)/cpp_dpi.access.json
AUTHORING_CORE_CLOCK_DISCOVERY := $(AUTHORING_CORE_BUILD_DIR)/discover_clocks
AUTHORING_CORE_CLOCK_DISCOVERY_CPP := $(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/framework/clock_discovery.cpp
AUTHORING_CORE_DPI_GENERATED := \
	$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/generated/authoring_core_dut.hpp \
	$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/generated/authoring_core_binding.hpp \
	$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/generated/dpi_authoring_core.sv
AUTHORING_CORE_RTL := $(AUTHORING_CORE_DIR)/rtl/authoring_core_dut.sv
AUTHORING_CORE_CPP := \
	$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/framework/authoring_core.hpp \
	$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/framework/dpi_transport.cpp \
	$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/testbench.cpp
AUTHORING_CORE_KERNELS := control task_value clock_cycles timeout task_timeout wait_until event queue queue_sync all wide64 wide_echo_137 wide_slice fixed_mac array_index array_wide mem_rw hier_probe mem_backdoor mem_probe_read mem_probe_deposit mem_probe_read_deposit signal_edge array_multidim force_release packed_view force_direct hier_data timing_phases timing_phases_deferred test_lifecycle dynamic_spawn dynamic_task dynamic_spawn_scheduler dynamic_spawn_suspending dynamic_monitor process_pipeline analysis_fanout random_stimulus constrained_packet constraint_extensions coverage_sampling coverage_native apb_component transaction_recording memory_model memory_model_direct register_prediction_validity register_backdoor register_hierarchy register_split register_wide register_enum register_memory register_sequences register_coverage register_maps register_user_effects structured_logging structured_log_history mixed_logging
AUTHORING_CORE_KERNEL ?= control
# Every measured benchmark binary is compiled the same way. Verilator applies
# -Os by default to its generated model and no optimization at all to testbench
# sources, which penalises the coroutine-heavy C++ DPI side against its
# pure-SystemVerilog twin and distorts the ratio the guard checks. Override
# CPPTB_BENCH_OPT_FAST to sweep all suites, or one suite's variable for one.
CPPTB_BENCH_OPT_FAST ?= -O3
# Exported so the cocotb runners, which drive Verilator themselves, build their
# model with the same optimization as the other three modes.
export CPPTB_BENCH_OPT_FAST
AUTHORING_CORE_OPT_FAST ?= $(CPPTB_BENCH_OPT_FAST)
AUTHORING_CORE_CONVERGE_LIMIT ?= 50000000
AUTHORING_CORE_EXTRA_CFLAGS ?=
AUTHORING_CORE_EXTRA_LDFLAGS ?=
FRAMEWORK_COMPARISON_DIR := benchmarks/framework_comparison
FRAMEWORK_COMPARISON_BUILD_DIR := $(BUILD_DIR)/benchmarks/framework_comparison
FRAMEWORK_COMPARISON_VPI_OBJ_DIR := $(FRAMEWORK_COMPARISON_BUILD_DIR)/cpp_vpi_obj
FRAMEWORK_COMPARISON_VPI_BINARY := $(FRAMEWORK_COMPARISON_VPI_OBJ_DIR)/Vauthoring_core_vpi_top
FRAMEWORK_COMPARISON_VPI_TOP := $(FRAMEWORK_COMPARISON_DIR)/testbenches/cpp_vpi/authoring_core_vpi_top.sv
FRAMEWORK_COMPARISON_VPI_HOST := $(FRAMEWORK_COMPARISON_DIR)/testbenches/cpp_vpi/authoring_core_vpi_host.cpp
FRAMEWORK_COMPARISON_COCOTB_RUNNER := $(FRAMEWORK_COMPARISON_DIR)/testbenches/cocotb/run_cocotb.py
HEAVY_SUITE_DIR := $(FRAMEWORK_COMPARISON_DIR)/heavy_suite
HEAVY_SUITE_BUILD_DIR := $(FRAMEWORK_COMPARISON_BUILD_DIR)/heavy_suite
HEAVY_SUITE_OPT_FAST ?= $(CPPTB_BENCH_OPT_FAST)
HEAVY_SUITE_RTL := $(HEAVY_SUITE_DIR)/rtl/heavy_benchmark_dut.sv
HEAVY_SUITE_DPI_MANIFEST := $(HEAVY_SUITE_DIR)/testbenches/cpp_dpi/heavy_benchmark.dpi.json
HEAVY_SUITE_DPI_CODEGEN_STAMP := $(HEAVY_SUITE_BUILD_DIR)/cpp_dpi.codegen.stamp
HEAVY_SUITE_CLOCK_CONFIG := $(HEAVY_SUITE_BUILD_DIR)/clocks.json
HEAVY_SUITE_CLOCK_DISCOVERY := $(HEAVY_SUITE_BUILD_DIR)/discover_clocks
HEAVY_SUITE_CLOCK_DISCOVERY_CPP := $(HEAVY_SUITE_DIR)/testbenches/cpp_dpi/framework/clock_discovery.cpp
HEAVY_SUITE_DPI_GENERATED := \
	$(HEAVY_SUITE_DIR)/testbenches/cpp_dpi/generated/heavy_benchmark_dut.hpp \
	$(HEAVY_SUITE_DIR)/testbenches/cpp_dpi/generated/heavy_benchmark_binding.hpp \
	$(HEAVY_SUITE_DIR)/testbenches/cpp_dpi/generated/dpi_heavy_benchmark.sv
HEAVY_SUITE_DPI_CPP := \
	$(HEAVY_SUITE_DIR)/testbenches/cpp_dpi/framework/heavy_benchmark.hpp \
	$(HEAVY_SUITE_DIR)/testbenches/cpp_dpi/framework/dpi_transport.cpp \
	$(HEAVY_SUITE_DIR)/testbenches/cpp_dpi/testbench.cpp
HEAVY_SUITE_WORKLOADS := streaming_fir packet_crc32 matrix4x4
HEAVY_SUITE_SV_BINARY := $(HEAVY_SUITE_BUILD_DIR)/pure_sv_obj/Vheavy_benchmark_sv_tb
HEAVY_SUITE_VPI_BINARY := $(HEAVY_SUITE_BUILD_DIR)/cpp_vpi_obj/Vheavy_benchmark_vpi_top
HEAVY_SUITE_COCOTB_RUNNER := $(HEAVY_SUITE_DIR)/testbenches/cocotb/run_cocotb.py
OPEN_CORES_DIR := $(FRAMEWORK_COMPARISON_DIR)/open_cores
OPEN_CORES_BUILD_DIR := $(FRAMEWORK_COMPARISON_BUILD_DIR)/open_cores
OPEN_CORES_OPT_FAST ?= $(CPPTB_BENCH_OPT_FAST)
OPEN_CORES_DPI_MANIFEST := $(OPEN_CORES_DIR)/testbenches/cpp_dpi/open_cores.dpi.json
OPEN_CORES_DPI_CODEGEN_STAMP := $(OPEN_CORES_BUILD_DIR)/cpp_dpi.codegen.stamp
OPEN_CORES_CLOCK_CONFIG := $(OPEN_CORES_BUILD_DIR)/clocks.json
OPEN_CORES_CLOCK_DISCOVERY := $(OPEN_CORES_BUILD_DIR)/discover_clocks
OPEN_CORES_CLOCK_DISCOVERY_CPP := $(OPEN_CORES_DIR)/testbenches/cpp_dpi/framework/clock_discovery.cpp
OPEN_CORES_DPI_GENERATED := \
	$(OPEN_CORES_DIR)/testbenches/cpp_dpi/generated/open_cores_dut.hpp \
	$(OPEN_CORES_DIR)/testbenches/cpp_dpi/generated/open_cores_binding.hpp \
	$(OPEN_CORES_DIR)/testbenches/cpp_dpi/generated/dpi_open_cores_benchmark.sv
OPEN_CORES_DPI_CPP := \
	$(OPEN_CORES_DIR)/testbenches/cpp_dpi/framework/open_cores_benchmark.hpp \
	$(OPEN_CORES_DIR)/testbenches/cpp_dpi/framework/dpi_transport.cpp \
	$(OPEN_CORES_DIR)/testbenches/cpp_dpi/testbench.cpp
OPEN_CORES_RTL := \
	$(OPEN_CORES_DIR)/third_party/picorv32/picorv32.v \
	$(OPEN_CORES_DIR)/third_party/secworks_aes/aes_sbox.v \
	$(OPEN_CORES_DIR)/third_party/secworks_aes/aes_inv_sbox.v \
	$(OPEN_CORES_DIR)/third_party/secworks_aes/aes_key_mem.v \
	$(OPEN_CORES_DIR)/third_party/secworks_aes/aes_encipher_block.v \
	$(OPEN_CORES_DIR)/third_party/secworks_aes/aes_decipher_block.v \
	$(OPEN_CORES_DIR)/third_party/secworks_aes/aes_core.v \
	$(OPEN_CORES_DIR)/third_party/secworks_aes/aes.v \
	$(OPEN_CORES_DIR)/third_party/verilog_ethernet/lfsr.v \
	$(OPEN_CORES_DIR)/third_party/verilog_ethernet/axis_eth_fcs.v \
	$(OPEN_CORES_DIR)/rtl/open_cores_benchmark_dut.sv
OPEN_CORES_WORKLOADS := picorv32_firmware secworks_aes128 ethernet_fcs64
OPEN_CORES_COCOTB_RUNNER := $(OPEN_CORES_DIR)/testbenches/cocotb/run_cocotb.py
OPEN_CORES_COCOTB_WORKLOAD ?= picorv32_firmware
OPEN_CORES_COCOTB_ITERS ?= 100
COCOTB_BENCH_PYTHON ?= 3.12
# z3::get_full_version(), used by include/cpptb/z3_random_backend.hpp, first
# shipped in Z3 4.15.5. Keep in sync with tools/check_environment.py.
CPPTB_MIN_Z3_VERSION := 4.15.5
# `make z3-toolchain` writes a z3.pc for the portable z3-solver wheel here.
# Prepending is a no-op until that file exists, and it deliberately outranks a
# distribution Z3, which is usually older than CPPTB_MIN_Z3_VERSION.
CPPTB_Z3_DIR := $(abspath $(BUILD_DIR)/z3)
CPPTB_Z3_PKGCONFIG_DIR := $(abspath $(BUILD_DIR)/pkgconfig)
# Pinned so CI and developers resolve the same solver. 4.15.5 first exposes
# z3::get_full_version(), but 4.15.5-4.15.7 published no Linux wheels, and
# 4.15.8/4.16.0 ship macosx_15_0 wheels only, so they cannot install on macOS 13
# or 14. 5.0.0.0 is back to a macosx_13_0 floor and keeps the same manylinux
# coverage, which makes it the widest-reaching pin rather than merely the newest.
CPPTB_Z3_WHEEL_SPEC := z3-solver==5.0.0.0
export PKG_CONFIG_PATH := $(CPPTB_Z3_PKGCONFIG_DIR):$(PKG_CONFIG_PATH)
FEATURE ?=
FEATURE_REGRESSION_RUNNER := python3 benchmarks/run_regression.py
UV_CACHE_DIR ?= $(BUILD_DIR)/uv-cache
CODEGEN_PYTHON := UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --frozen python
PEAKRDL_PYTHON := UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --frozen --extra peakrdl python
PEAKRDL := UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --frozen --extra peakrdl peakrdl
DOCS_RUN := UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --frozen --group docs
CPPTB_CODEGEN := UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --frozen cpptb-codegen
CPPTB := UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --frozen cpptb
CPPTB_CODEGEN_SOURCES := \
	$(CPPTB_CODEGEN_ENTRY) \
	tools/codegen/cpptb_codegen/__init__.py \
	tools/codegen/cpptb_codegen/__main__.py \
	tools/codegen/cpptb_codegen/build.py \
	tools/codegen/cpptb_codegen/cli.py \
	tools/codegen/cpptb_codegen/design_ir.py \
	tools/codegen/cpptb_codegen/peakrdl_plugin.py \
	tools/codegen/cpptb_codegen/project.py \
	tools/codegen/cpptb_codegen/register_codegen.py \
	tools/codegen/cpptb_codegen/rggen_codegen.py \
	tools/codegen/cpptb_codegen/runner.py \
	tools/codegen/cpptb_codegen/verilator_capabilities.py \
	tools/codegen/cpptb_codegen/frontends/__init__.py \
	tools/codegen/cpptb_codegen/frontends/slang.py \
	tools/codegen/cpptb_codegen/frontends/verilator_json.py \
	pyproject.toml uv.lock
PERIPHERAL_SUITE_DPI_GENERATED := \
	benchmarks/peripheral_suite/testbenches/cpp_dpi/generated/peripheral_suite_dut.hpp \
	benchmarks/peripheral_suite/testbenches/cpp_dpi/generated/peripheral_suite_binding.hpp \
	benchmarks/peripheral_suite/testbenches/cpp_dpi/generated/dpi_peripheral_suite.sv
PERIPHERAL_SUITE_DPI_CODEGEN_STAMP := $(PERIPHERAL_SUITE_BUILD_DIR)/cpp_dpi.codegen.stamp
CPPTB_APB_EVENT_RTL := \
	experiments/cpp_vpi/rggen_apb_event/rtl/apb_event_service_unit_regs_core_pkg.sv \
	experiments/cpp_vpi/rggen_apb_event/rtl/apb_event_service_unit_regs_core.sv \
	experiments/cpp_vpi/rggen_apb_event/rtl/apb_event_sleep_unit_regs_core_pkg.sv \
	experiments/cpp_vpi/rggen_apb_event/rtl/apb_event_sleep_unit_regs_core.sv \
	experiments/cpp_vpi/rggen_apb_event/rtl/apb_event_unit_peakrdl.sv \
	experiments/cpp_vpi/rggen_apb_event/rtl/vpi_apb_event_unit.sv
PERIPHERAL_SUITE_CORE_RTL := \
	benchmarks/peripheral_suite/rtl/apb_timer_regs_core_pkg.sv \
	benchmarks/peripheral_suite/rtl/apb_timer_regs_core.sv \
	benchmarks/peripheral_suite/rtl/timer_peakrdl.sv \
	benchmarks/peripheral_suite/rtl/apb_timer_peakrdl.sv \
	benchmarks/peripheral_suite/rtl/apb_spi_master_regs_core_pkg.sv \
	benchmarks/peripheral_suite/rtl/apb_spi_master_regs_core.sv \
	benchmarks/peripheral_suite/rtl/spi_master_apb_if_peakrdl.sv \
	benchmarks/peripheral_suite/rtl/apb_i2c_regs_core_pkg.sv \
	benchmarks/peripheral_suite/rtl/apb_i2c_regs_core.sv \
	benchmarks/peripheral_suite/rtl/i2c_master_bit_ctrl.sv \
	benchmarks/peripheral_suite/rtl/i2c_master_byte_ctrl.sv \
	benchmarks/peripheral_suite/rtl/apb_i2c_peakrdl.sv \
	benchmarks/peripheral_suite/rtl/peripheral_suite_dut.sv
PERIPHERAL_SUITE_VPI_RTL := \
	$(PERIPHERAL_SUITE_CORE_RTL) \
	benchmarks/peripheral_suite/testbenches/cpp_vpi/vpi_peripheral_suite.sv
PERIPHERAL_SUITE_SV_RTL := \
	$(PERIPHERAL_SUITE_CORE_RTL) \
	benchmarks/peripheral_suite/testbenches/systemverilog/peripheral_suite_sv_tb.sv
PERIPHERAL_SUITE_DPI_RTL := \
	$(PERIPHERAL_SUITE_CORE_RTL) \
	benchmarks/peripheral_suite/testbenches/cpp_dpi/generated/dpi_peripheral_suite.sv
VERILATOR_ROOT := $(shell verilator --getenv VERILATOR_ROOT)
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SDKROOT := $(shell xcrun --show-sdk-path)
VERILATOR_PLATFORM_CXXFLAGS := -I$(SDKROOT)/usr/include/c++/v1
CPPTB_SHLIB_EXT := dylib
CPPTB_SHLIB_FLAG := -dynamiclib
else
VERILATOR_PLATFORM_CXXFLAGS :=
CPPTB_SHLIB_EXT := so
CPPTB_SHLIB_FLAG := -shared
endif
ifeq ($(origin CXX),default)
CXX := c++
endif
export CXX

CPPTB_EXAMPLE_PHONY_TARGETS :=
CPPTB_EXAMPLE_TEST_TARGETS :=
CPPTB_EXAMPLE_FRONTEND_CHECK_TARGETS :=

# $(1): target slug, $(2): variable prefix, $(3): directory name,
# $(4): RTL/top basename, $(5): generated DPI top, $(6): pure-SV top,
# $(7): fixed semantic benchmark test, $(8): optional `slang-only` frontend mode.
define CPPTB_EXAMPLE_template
CPPTB_$(2)_DIR := examples/$(3)
CPPTB_$(2)_BUILD_DIR := $$(CPPTB_BUILD_DIR)/$(3)
CPPTB_$(2)_GENERATED_DIR := $$(CPPTB_$(2)_BUILD_DIR)/generated
CPPTB_$(2)_OBJ_DIR := $$(CPPTB_$(2)_BUILD_DIR)/obj
CPPTB_$(2)_SV_OBJ_DIR := $$(CPPTB_$(2)_BUILD_DIR)/systemverilog_obj
CPPTB_$(2)_RESULT_DIR := $$(CPPTB_$(2)_BUILD_DIR)/results
CPPTB_$(2)_SV_TB := $$(CPPTB_$(2)_DIR)/systemverilog/$(6).sv
CPPTB_$(2)_DEFAULT_TEST := $(7)
CPPTB_$(2)_FRONTEND_CHECK_ARGS := $$(if $$(filter slang-only,$(8)),--rebuild,--compare-frontend verilator_json)
CPPTB_$(2)_GENERATED := \
	$$(CPPTB_$(2)_GENERATED_DIR)/$(4)_dut.hpp \
	$$(CPPTB_$(2)_GENERATED_DIR)/dut.hpp \
	$$(CPPTB_$(2)_GENERATED_DIR)/$(4)_binding.hpp \
	$$(CPPTB_$(2)_GENERATED_DIR)/$(5).sv \
	$$(CPPTB_$(2)_GENERATED_DIR)/$(5).cpp
CPPTB_$(2)_PROJECT_ARGS = \
	--project $$(CPPTB_$(2)_DIR) \
	--build-dir $$(abspath $$(BUILD_DIR)) \
	--build-name $(3) \
	--top $(4) \
	--target $(4)

CPPTB_EXAMPLE_PHONY_TARGETS += cpp-dpi-$(1)-codegen \
	cpp-dpi-$(1)-codegen-check cpp-dpi-$(1)-frontend-check \
	cpp-dpi-$(1)-build cpp-dpi-$(1)-run \
	cpp-dpi-$(1)-sv-build cpp-dpi-$(1)-sv-run
CPPTB_EXAMPLE_TEST_TARGETS += cpp-dpi-$(1)-run cpp-dpi-$(1)-sv-run
CPPTB_EXAMPLE_FRONTEND_CHECK_TARGETS += cpp-dpi-$(1)-frontend-check

cpp-dpi-$(1)-codegen:
	$$(CPPTB) build $$(CPPTB_$(2)_PROJECT_ARGS)

cpp-dpi-$(1)-codegen-check:
	$$(CPPTB) build $$(CPPTB_$(2)_PROJECT_ARGS) --rebuild

cpp-dpi-$(1)-frontend-check:
	$$(CPPTB) build $$(CPPTB_$(2)_PROJECT_ARGS) \
		$$(CPPTB_$(2)_FRONTEND_CHECK_ARGS)

cpp-dpi-$(1)-build:
	$$(CPPTB) build $$(CPPTB_$(2)_PROJECT_ARGS)

cpp-dpi-$(1)-run: cpp-dpi-$(1)-build
	CPPTB_TEST="$$(CPPTB_$(2)_DEFAULT_TEST)" \
		$$(CPPTB_$(2)_OBJ_DIR)/V$(5)

$$(CPPTB_$(2)_SV_OBJ_DIR)/V$(6): \
		$$(CPPTB_$(2)_DIR)/$(4).sv $$(CPPTB_$(2)_SV_TB)
	mkdir -p $$(CPPTB_$(2)_SV_OBJ_DIR)
	verilator --binary --timing --no-sched-zero-delay \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-UNUSEDSIGNAL -Wno-BLKANDNBLK \
		-Wno-MULTIDRIVEN \
		--Mdir $$(CPPTB_$(2)_SV_OBJ_DIR) \
		--top-module $(6) \
		$$(CPPTB_$(2)_DIR)/$(4).sv \
		$$(CPPTB_$(2)_SV_TB)

cpp-dpi-$(1)-sv-build: $$(CPPTB_$(2)_SV_OBJ_DIR)/V$(6)

cpp-dpi-$(1)-sv-run: $$(CPPTB_$(2)_SV_OBJ_DIR)/V$(6)
	$$(CPPTB_$(2)_SV_OBJ_DIR)/V$(6)
endef

$(eval $(call CPPTB_EXAMPLE_template,counter,COUNTER,counter,counter,dpi_counter,counter_sv_tb,counter_sequence))
CPPTB_EXAMPLE_TEST_TARGETS += cpp-dpi-counter-suite-test
$(eval $(call CPPTB_EXAMPLE_template,multiclock,MULTICLOCK,multiclock,dual_clock_mailbox,dpi_dual_clock_mailbox,dual_clock_mailbox_sv_tb,multiclock_test))
$(eval $(call CPPTB_EXAMPLE_template,timer-only,TIMER_ONLY,timer_only,timer_only_probe,dpi_timer_only_probe,timer_only_probe_sv_tb,timer_only_test))
CPPTB_TIMER_ONLY_DEADLOCK_SV_TB := \
	examples/timer_only/systemverilog/timer_only_deadlock_sv_tb.sv
CPPTB_TIMER_ONLY_DEADLOCK_SV_OBJ_DIR := \
	$(CPPTB_TIMER_ONLY_BUILD_DIR)/systemverilog_deadlock_obj
CPPTB_TIMER_ONLY_DEADLOCK_SV_BINARY := \
	$(CPPTB_TIMER_ONLY_DEADLOCK_SV_OBJ_DIR)/Vtimer_only_deadlock_sv_tb
CPPTB_EXAMPLE_PHONY_TARGETS += cpp-dpi-timer-only-deadlock-test
CPPTB_EXAMPLE_TEST_TARGETS += cpp-dpi-timer-only-deadlock-test

$(CPPTB_TIMER_ONLY_DEADLOCK_SV_BINARY): $(CPPTB_TIMER_ONLY_DEADLOCK_SV_TB)
	mkdir -p $(CPPTB_TIMER_ONLY_DEADLOCK_SV_OBJ_DIR)
	verilator --binary --timing --no-sched-zero-delay \
		--Mdir $(CPPTB_TIMER_ONLY_DEADLOCK_SV_OBJ_DIR) \
		--top-module timer_only_deadlock_sv_tb \
		$(CPPTB_TIMER_ONLY_DEADLOCK_SV_TB)

cpp-dpi-timer-only-deadlock-test: cpp-dpi-timer-only-build \
		$(CPPTB_TIMER_ONLY_DEADLOCK_SV_BINARY)
	mkdir -p $(CPPTB_TIMER_ONLY_RESULT_DIR)
	python3 tests/integration/check_expected_failure.py \
		--contains "scheduler deadlock detected" \
		--contains "Event timer_only.response_ready" \
		--result-json $(CPPTB_TIMER_ONLY_RESULT_DIR)/deadlock-result.json \
		--result-status timed_out --result-wait-status deadlocked -- \
		env CPPTB_TEST=timer_only_deadlock \
		CPPTB_RESULT_FILE=$(CPPTB_TIMER_ONLY_RESULT_DIR)/deadlock-result.json \
		$(CPPTB_TIMER_ONLY_OBJ_DIR)/Vdpi_timer_only_probe
	python3 tests/integration/check_expected_failure.py \
		--contains "pure-SV deadlock" \
		--contains "Event timer_only.response_ready" -- \
		$(CPPTB_TIMER_ONLY_DEADLOCK_SV_BINARY)
$(eval $(call CPPTB_EXAMPLE_template,fifo-scoreboard,FIFO_SCOREBOARD,fifo_scoreboard,stream_fifo,dpi_stream_fifo,stream_fifo_sv_tb,fifo_test))
$(eval $(call CPPTB_EXAMPLE_template,component-fifo,COMPONENT_FIFO,component_fifo,component_fifo,dpi_component_fifo,component_fifo_sv_tb,component_fifo_test))
$(eval $(call CPPTB_EXAMPLE_template,apb-regfile,APB_REGFILE,apb_regfile,apb_regfile,dpi_apb_regfile,apb_regfile_sv_tb,component_apb_test))
CPPTB_EXAMPLE_PHONY_TARGETS += cpp-dpi-apb-regfile-suite-test
CPPTB_EXAMPLE_TEST_TARGETS += cpp-dpi-apb-regfile-suite-test
$(eval $(call CPPTB_EXAMPLE_template,apb-trace,APB_TRACE,apb_trace,apb_trace,dpi_apb_trace,apb_trace_sv_tb,transaction_recording_test))
$(eval $(call CPPTB_EXAMPLE_template,ipxact-regfile,IPXACT_REGFILE,ipxact_regfile,ipxact_regfile,dpi_ipxact_regfile,ipxact_regfile_sv_tb,ipxact_register_model_test))
CPPTB_IPXACT_REGFILE_MODEL := $(CPPTB_IPXACT_REGFILE_GENERATED_DIR)/ipxact_regs.hpp
CPPTB_IPXACT_REGFILE_MODEL_TEST := $(CPPTB_IPXACT_REGFILE_BUILD_DIR)/model_contract
CPPTB_EXAMPLE_PHONY_TARGETS += cpptb-ipxact-regfile-model-test
CPPTB_EXAMPLE_TEST_TARGETS += cpptb-ipxact-regfile-model-test

$(CPPTB_IPXACT_REGFILE_MODEL): \
		examples/ipxact_regfile/component.xml $(CPPTB_CODEGEN_SOURCES)
	mkdir -p $(CPPTB_IPXACT_REGFILE_GENERATED_DIR)
	$(PEAKRDL) cpptb $< -o $@ \
		--namespace ipxact_regs --rename peripheral

cpp-dpi-ipxact-regfile-codegen cpp-dpi-ipxact-regfile-codegen-check \
		cpp-dpi-ipxact-regfile-frontend-check cpp-dpi-ipxact-regfile-build: \
		$(CPPTB_IPXACT_REGFILE_MODEL)

$(CPPTB_IPXACT_REGFILE_MODEL_TEST): \
		examples/ipxact_regfile/model_contract.cpp \
		$(CPPTB_IPXACT_REGFILE_MODEL) $(CPPTB_PUBLIC_HEADERS)
	$(CXX) -std=c++20 -O2 -Iinclude -I. \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		-I$(CPPTB_IPXACT_REGFILE_GENERATED_DIR) \
		examples/ipxact_regfile/model_contract.cpp -o $@

cpptb-ipxact-regfile-model-test: $(CPPTB_IPXACT_REGFILE_MODEL_TEST)
	$(CPPTB_IPXACT_REGFILE_MODEL_TEST)
$(eval $(call CPPTB_EXAMPLE_template,watchdog-timeout,WATCHDOG_TIMEOUT,watchdog_timeout,stalling_responder,dpi_stalling_responder,stalling_responder_sv_tb,watchdog_sequence))
$(eval $(call CPPTB_EXAMPLE_template,fault-injection,FAULT_INJECTION,fault_injection,fault_injection,dpi_fault_injection,fault_injection_sv_tb,fault_injection_sequence))
$(eval $(call CPPTB_EXAMPLE_template,rich-data,RICH_DATA,rich_data,rich_data,dpi_rich_data,rich_data_sv_tb,rich_data_sequence))
$(eval $(call CPPTB_EXAMPLE_template,interfaces,INTERFACES,interfaces,stream_interfaces,dpi_stream_interfaces,stream_interfaces_sv_tb,interface_test,slang-only))
$(eval $(call CPPTB_EXAMPLE_template,mixed-logging,MIXED_LOGGING,mixed_logging,mixed_logging,dpi_mixed_logging,mixed_logging_sv_tb,mixed_language_logging))
CPPTB_EXAMPLE_PHONY_TARGETS += cpp-dpi-mixed-logging-output-test
CPPTB_EXAMPLE_TEST_TARGETS += cpp-dpi-mixed-logging-output-test

cpp-dpi-mixed-logging-output-test: cpp-dpi-mixed-logging-build
	python3 examples/mixed_logging/check_output.py \
		$(CPPTB_MIXED_LOGGING_OBJ_DIR)/Vdpi_mixed_logging

.PHONY: help all doctor z3-toolchain test unit-test python-test codegen-test conformance-test examples-test ground-truth-test secworks-aes-regmodel-equivalence secworks-aes-regmodel-benchmark docs-build docs-check docs-diagram-check docs-sphinx-build docs-sphinx-serve docs-zensical-build docs-zensical-serve run vpi-run cpp-vpi-run cpp-coro-runtime-test cpptb-packed-value-test cpptb-random-test cpptb-randomized-test cpptb-z3-random-test cpptb-coverage-test cpptb-test-api-test cpptb-components-test cpptb-transaction-recording-test cpptb-memory-model-test cpptb-register-model-test cpptb-register-sequences-test cpptb-register-coverage-test cpptb-hierarchy-test cpptb-peakrdl-test cpp-dpi-counter-suite-test cpp-apb-event-run cpp-apb-event-bench-build cpp-apb-event-bench-run cpptb-codegen-test cpptb-codegen-frontend-check cpptb-conformance-codegen cpptb-conformance-codegen-check cpptb-conformance-frontend-check cpptb-conformance-build cpptb-conformance-run cpptb-conformance-vpi-run deferred-writes-test wave-equivalence-test wave-eq-counter wave-eq-fifo_scoreboard wave-eq-apb_regfile wave-eq-multiclock wave-eq-multiclock-read backend-equivalence-test backend-eq-counter-vcd backend-eq-counter-fst backend-eq-fifo_scoreboard-vcd backend-eq-apb_regfile-vcd backend-eq-multiclock-vcd $(CPPTB_EXAMPLE_PHONY_TARGETS) peripheral-suite-build peripheral-suite-run peripheral-suite-sv-build peripheral-suite-sv-run peripheral-suite-dpi-codegen peripheral-suite-dpi-codegen-check peripheral-suite-dpi-build peripheral-suite-dpi-run authoring-core-dpi-codegen authoring-core-dpi-codegen-check authoring-core-dpi-build authoring-core-dpi-run authoring-core-sv-build authoring-core-sv-run authoring-core-build authoring-core-benchmark authoring-core-timing-experiments-build authoring-core-force-direct-sv-build framework-comparison-vpi-build framework-comparison-vpi-run framework-comparison-cocotb-build framework-comparison-build framework-comparison-benchmark feature-list feature-test feature-benchmark feature-regression registry-check clean

help:
	@printf '%s\n' \
		'cpptb development targets:' \
		'  make doctor                  Check the local toolchain before building' \
		'  make z3-toolchain            Install a portable Z3 for the optional adapter' \
		'  make test                    Run all unit, codegen, conformance, and example tests' \
		'  make feature-list            List semantic/performance benchmark features' \
		'  make feature-test FEATURE=…  Run one semantic comparison' \
		'  make feature-benchmark FEATURE=…  Benchmark one C++/SV pair' \
		'  make feature-regression      Run the complete serial regression' \
		'  make secworks-aes-regmodel-equivalence  Check generated RegModel against upstream AES' \
		'  make secworks-aes-regmodel-benchmark    Benchmark the matched AES workload' \
		'  make authoring-core-timing-experiments-build  Build timing backends' \
		'  make docs-build              Build both documentation variants' \
		'  make docs-sphinx-serve       Preview Sphinx at localhost:8001' \
		'  make docs-zensical-serve     Preview Zensical at localhost:8002' \
		'  make clean                   Remove generated build output'

all: test

# Deliberately uses bare python3 and no uv: this must work on a fresh checkout
# before any dependency has been installed.
doctor:
	@python3 tools/check_environment.py

# One portable way to get a new enough Z3 on both macOS and Linux, for hosts
# whose packaged Z3 predates $(CPPTB_MIN_Z3_VERSION). The wheel goes into its
# own directory, not the project virtualenv: `uv sync --frozen` prunes anything
# outside the lockfile, which would leave linked binaries with a dangling rpath.
z3-toolchain:
	UV_CACHE_DIR=$(UV_CACHE_DIR) uv pip install --target $(CPPTB_Z3_DIR) \
		'$(CPPTB_Z3_WHEEL_SPEC)'
	python3 tools/z3_pkgconfig.py --site-dir $(CPPTB_Z3_DIR) \
		--output-dir $(CPPTB_Z3_PKGCONFIG_DIR)

test: unit-test python-test codegen-test conformance-test deferred-writes-test wave-equivalence-test backend-equivalence-test examples-test ground-truth-test registry-check

unit-test: cpp-coro-runtime-test cpptb-packed-value-test cpptb-random-test cpptb-randomized-test cpptb-z3-random-test cpptb-coverage-test cpptb-test-api-test \
	cpptb-components-test cpptb-transaction-recording-test cpptb-memory-model-test cpptb-register-model-test cpptb-register-sequences-test cpptb-register-coverage-test \
	cpptb-hierarchy-test

python-test:
	$(CODEGEN_PYTHON) -m unittest discover -s tests/tools
	$(CODEGEN_PYTHON) -m unittest discover -s benchmarks/tests
	$(CODEGEN_PYTHON) -m unittest discover -s benchmarks/authoring_core/tests
	$(CODEGEN_PYTHON) -m unittest discover -s benchmarks/framework_comparison/tests
	$(CODEGEN_PYTHON) -m unittest discover -s benchmarks/framework_comparison/heavy_suite/tests
	$(CODEGEN_PYTHON) -m unittest discover -s benchmarks/framework_comparison/open_cores/tests
	$(CODEGEN_PYTHON) -m unittest discover -s benchmarks/regmodel_ground_truth/secworks_aes/tests
	$(CODEGEN_PYTHON) -m unittest discover -s benchmarks/peripheral_suite/tests

codegen-test: cpptb-codegen-test cpptb-peakrdl-test cpptb-codegen-frontend-check

# Both supported timing backends, contract-checked on every run. The
# direct binary is the make-built default; the vpi variant builds via
# the runner so the bridge link itself stays covered.
conformance-test: cpptb-conformance-run cpptb-conformance-vpi-run

# The deferred-write contract -- the cocotb write model -- pinned on both
# supported timing backends against the same testbench.
# Pure SV and cpptb dump the same run of the same design, and a
# cycle-sampled comparator proves the design saw the identical state
# trajectory. Four pairs across different design classes keep the claim
# honest: a plain sequential counter, a ready/valid FIFO with
# backpressure, a register file behind the APB components, and a
# dual-clock mailbox compared on both domains. Each twin builds with
# --trace into its own object directory so the untraced twin stays warm;
# the cpptb side is an ordinary --wave vcd build of the example under a
# wave_eq_* build name so the regular example builds stay warm too.
WAVE_EQ_DIR := $(BUILD_DIR)/wave-equivalence

# $(1) example dir  $(2) cpptb top  $(3) test name  $(4) twin top
# $(5) twin sources $(6) clock      $(7) min cycles
define WAVE_EQ_template
wave-eq-$(1):
	mkdir -p $(WAVE_EQ_DIR)/$(1)
	verilator --binary --timing --no-sched-zero-delay --trace \
		+define+CPPTB_TWIN_WAVE \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-UNUSEDSIGNAL -Wno-BLKANDNBLK \
		-Wno-MULTIDRIVEN \
		--Mdir $(WAVE_EQ_DIR)/$(1)/twin_obj \
		--top-module $(4) \
		$(5)
	cd $(WAVE_EQ_DIR)/$(1) && $$(abspath $(WAVE_EQ_DIR)/$(1)/twin_obj)/V$(4)
	$$(CPPTB) test --project examples/$(1) \
		--build-dir $$(abspath $$(BUILD_DIR)) --build-name wave_eq_$(1) \
		--top $(2) --target $(2) --wave vcd
	python3 tools/wave_compare.py \
		--a build/cpptb/wave_eq_$(1)/results/$(3).vcd \
		--a-scope dpi_$(2).i_dut \
		--b $(WAVE_EQ_DIR)/$(1)/twin.vcd \
		--b-scope $(4).i_dut \
		--clock-signal $(6) --min-cycles $(7)
endef

$(eval $(call WAVE_EQ_template,counter,counter,counter_sequence,counter_sv_tb,examples/counter/counter.sv examples/counter/systemverilog/counter_sv_tb.sv,clk,9))
$(eval $(call WAVE_EQ_template,fifo_scoreboard,stream_fifo,fifo_test,stream_fifo_sv_tb,examples/fifo_scoreboard/stream_fifo.sv examples/fifo_scoreboard/systemverilog/stream_fifo_sv_tb.sv,clk,30))
$(eval $(call WAVE_EQ_template,apb_regfile,apb_regfile,component_apb_test,apb_regfile_sv_tb,examples/apb_regfile/apb_regfile.sv examples/apb_regfile/systemverilog/apb_regfile_sv_tb.sv,clk,60))
$(eval $(call WAVE_EQ_template,multiclock,dual_clock_mailbox,multiclock_test,dual_clock_mailbox_sv_tb,examples/multiclock/dual_clock_mailbox.sv examples/multiclock/systemverilog/dual_clock_mailbox_sv_tb.sv,write_clk,25))

# The mailbox crosses two domains; the second comparison samples the same
# dumps on the read clock's grid.
wave-eq-multiclock-read: wave-eq-multiclock
	python3 tools/wave_compare.py \
		--a build/cpptb/wave_eq_multiclock/results/multiclock_test.vcd \
		--a-scope dpi_dual_clock_mailbox.i_dut \
		--b $(WAVE_EQ_DIR)/multiclock/twin.vcd \
		--b-scope dual_clock_mailbox_sv_tb.i_dut \
		--clock-signal read_clk --min-cycles 15

wave-equivalence-test: wave-eq-counter wave-eq-fifo_scoreboard \
	wave-eq-apb_regfile wave-eq-multiclock wave-eq-multiclock-read

# The two supported timing backends must be indistinguishable from the
# testbench's point of view: same example, same stimulus, one build per
# backend, and the runs must come out *identical* -- result records
# field for field (wall time excepted) and wave dumps byte for byte.
# That is deliberately stronger than the cycle-sampled equivalence
# above: the backends must schedule the same evals at the same
# simulation times, or the dumps diverge. Same four design classes as
# the pure-SV comparison; the counter repeats in fst to pin the second
# wave format (fst compares outside the header's date field -- the one
# spot the writer records wall-clock time).
# $(1) example dir  $(2) top  $(3) wave format
define BACKEND_EQ_template
backend-eq-$(1)-$(3):
	$$(CPPTB) test --project examples/$(1) \
		--build-dir $$(abspath $$(BUILD_DIR)) \
		--build-name backend_eq_$(1)_$(3)_direct \
		--top $(2) --target $(2) \
		--timing-backend verilator-direct --wave $(3)
	$$(CPPTB) test --project examples/$(1) \
		--build-dir $$(abspath $$(BUILD_DIR)) \
		--build-name backend_eq_$(1)_$(3)_vpi \
		--top $(2) --target $(2) \
		--timing-backend vpi --wave $(3)
	python3 tools/backend_compare.py \
		build/cpptb/backend_eq_$(1)_$(3)_direct/results \
		build/cpptb/backend_eq_$(1)_$(3)_vpi/results
endef

$(eval $(call BACKEND_EQ_template,counter,counter,vcd))
$(eval $(call BACKEND_EQ_template,counter,counter,fst))
$(eval $(call BACKEND_EQ_template,fifo_scoreboard,stream_fifo,vcd))
$(eval $(call BACKEND_EQ_template,apb_regfile,apb_regfile,vcd))
$(eval $(call BACKEND_EQ_template,multiclock,dual_clock_mailbox,vcd))

backend-equivalence-test: backend-eq-counter-vcd backend-eq-counter-fst \
	backend-eq-fifo_scoreboard-vcd backend-eq-apb_regfile-vcd \
	backend-eq-multiclock-vcd

deferred-writes-test:
	UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --frozen cpptb test --project tests/integration/deferred_writes
	UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --frozen cpptb test --project tests/integration/deferred_writes_vpi

examples-test: $(CPPTB_EXAMPLE_TEST_TARGETS)

ground-truth-test: secworks-aes-regmodel-equivalence

secworks-aes-regmodel-equivalence:
	$(MAKE) -C benchmarks/regmodel_ground_truth/secworks_aes equivalence

secworks-aes-regmodel-benchmark:
	$(MAKE) -C benchmarks/regmodel_ground_truth/secworks_aes benchmark \
		REPEATS=$${REPEATS:-180} RUNS=$${RUNS:-6}

cpp-dpi-counter-suite-test:
	$(CPPTB) test $(CPPTB_COUNTER_PROJECT_ARGS)

cpp-dpi-apb-regfile-suite-test:
	$(CPPTB) test $(CPPTB_APB_REGFILE_PROJECT_ARGS)

docs-build: docs-sphinx-build docs-zensical-build

docs-check: docs-diagram-check docs-build

# Every fenced diagram's boxes must close their rectangles; a lopsided box
# reads fine in a diff and looks broken on the rendered page.
docs-diagram-check:
	python3 tools/docs/check_diagrams.py

docs-sphinx-build:
	$(DOCS_RUN) sphinx-build -W --keep-going -c tools/docs/sphinx \
		-d $(DOCS_BUILD_DIR)/sphinx-doctrees \
		-b html docs $(SPHINX_DOCS_DIR)

docs-sphinx-serve: docs-sphinx-build
	$(DOCS_RUN) python -m http.server 8001 --bind 127.0.0.1 \
		--directory $(SPHINX_DOCS_DIR)

docs-zensical-build:
	$(DOCS_RUN) zensical build --clean --strict

docs-zensical-serve: docs-zensical-build
	$(DOCS_RUN) python -m http.server 8002 --bind 127.0.0.1 \
		--directory $(ZENSICAL_DOCS_DIR)

$(OBJ_DIR)/Vcounter.mk: experiments/verilator_c_api/counter.sv
	mkdir -p $(OBJ_DIR)
	verilator --cc --Mdir $(OBJ_DIR) $<

$(OBJ_DIR)/Vcounter__ALL.a: $(OBJ_DIR)/Vcounter.mk
	$(MAKE) -C $(OBJ_DIR) -f Vcounter.mk Vcounter__ALL.a CXXFLAGS="$(VERILATOR_PLATFORM_CXXFLAGS)"

$(BUILD_DIR)/libcounter.$(CPPTB_SHLIB_EXT): experiments/verilator_c_api/counter_c_api.cpp $(OBJ_DIR)/Vcounter__ALL.a
	$(CXX) $(CPPTB_SHLIB_FLAG) -std=c++17 \
		-I$(OBJ_DIR) \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		$(VERILATOR_PLATFORM_CXXFLAGS) \
		experiments/verilator_c_api/counter_c_api.cpp \
		$(OBJ_DIR)/Vcounter__ALL.a \
		$(VERILATOR_ROOT)/include/verilated.cpp \
		$(VERILATOR_ROOT)/include/verilated_threads.cpp \
		-o $@

$(BUILD_DIR)/counter_driver: experiments/verilator_c_api/counter_driver.mojo $(BUILD_DIR)/libcounter.$(CPPTB_SHLIB_EXT)
	mojo build $< -o $@

run: all
	./$(BUILD_DIR)/counter_driver

$(MOJOTB_OBJ_DIR)/Vvpi_counter.mk: experiments/mojo_vpi/examples/vpi_counter.sv
	mkdir -p $(MOJOTB_OBJ_DIR)
	verilator --cc --vpi --public-flat-rw --Mdir $(MOJOTB_OBJ_DIR) $<

$(MOJOTB_OBJ_DIR)/Vvpi_counter__ALL.a: $(MOJOTB_OBJ_DIR)/Vvpi_counter.mk
	$(MAKE) -C $(MOJOTB_OBJ_DIR) -f Vvpi_counter.mk Vvpi_counter__ALL.a CXXFLAGS="$(VERILATOR_PLATFORM_CXXFLAGS)"

$(MOJOTB_BUILD_DIR)/libcounter_test.$(CPPTB_SHLIB_EXT): experiments/mojo_vpi/tests/counter_test.mojo experiments/mojo_vpi/runtime.mojo experiments/mojo_vpi/__init__.mojo
	mkdir -p $(MOJOTB_BUILD_DIR)
	mojo build $< -I . --emit shared-lib -o $@

$(MOJOTB_BUILD_DIR)/vpi_counter_host: experiments/mojo_vpi/verilator_vpi_host.cpp $(MOJOTB_OBJ_DIR)/Vvpi_counter__ALL.a $(MOJOTB_BUILD_DIR)/libcounter_test.$(CPPTB_SHLIB_EXT)
	$(CXX) -std=c++17 \
		-I$(MOJOTB_OBJ_DIR) \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		$(VERILATOR_PLATFORM_CXXFLAGS) \
		experiments/mojo_vpi/verilator_vpi_host.cpp \
		$(MOJOTB_OBJ_DIR)/Vvpi_counter__ALL.a \
		$(VERILATOR_ROOT)/include/verilated.cpp \
		$(VERILATOR_ROOT)/include/verilated_threads.cpp \
		$(VERILATOR_ROOT)/include/verilated_vpi.cpp \
		-o $@

vpi-run: $(MOJOTB_BUILD_DIR)/vpi_counter_host
	./$(MOJOTB_BUILD_DIR)/vpi_counter_host $(MOJOTB_BUILD_DIR)/libcounter_test.$(CPPTB_SHLIB_EXT)

$(CPPTB_OBJ_DIR)/Vvpi_counter.mk: experiments/cpp_vpi/counter/vpi_counter.sv
	mkdir -p $(CPPTB_OBJ_DIR)
	verilator --cc --vpi --public-flat-rw --Mdir $(CPPTB_OBJ_DIR) $<

$(CPPTB_OBJ_DIR)/Vvpi_counter__ALL.a: $(CPPTB_OBJ_DIR)/Vvpi_counter.mk
	$(MAKE) -C $(CPPTB_OBJ_DIR) -f Vvpi_counter.mk Vvpi_counter__ALL.a CXXFLAGS="$(VERILATOR_PLATFORM_CXXFLAGS)"

$(CPPTB_BUILD_DIR)/vpi_counter_host: experiments/cpp_vpi/counter/verilator_vpi_host.cpp experiments/cpp_vpi/counter/runtime.hpp experiments/cpp_vpi/counter/counter_test.cpp experiments/cpp_vpi/counter/counter_test.hpp $(CPPTB_OBJ_DIR)/Vvpi_counter__ALL.a
	$(CXX) -std=c++17 \
		-Iinclude -I. \
		-I$(CPPTB_OBJ_DIR) \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		$(VERILATOR_PLATFORM_CXXFLAGS) \
		experiments/cpp_vpi/counter/verilator_vpi_host.cpp \
		experiments/cpp_vpi/counter/counter_test.cpp \
		$(CPPTB_OBJ_DIR)/Vvpi_counter__ALL.a \
		$(VERILATOR_ROOT)/include/verilated.cpp \
		$(VERILATOR_ROOT)/include/verilated_threads.cpp \
		$(VERILATOR_ROOT)/include/verilated_vpi.cpp \
		-o $@

cpp-vpi-run: $(CPPTB_BUILD_DIR)/vpi_counter_host
	./$(CPPTB_BUILD_DIR)/vpi_counter_host

$(CPPTB_CORO_RUNTIME_TEST): include/cpptb/coro_runtime.hpp include/cpptb/dpi_static_binding.hpp include/cpptb/packed_bits.hpp include/cpptb/probe.hpp \
		tests/unit/coro_runtime_test.cpp
	mkdir -p $(CPPTB_BUILD_DIR)
	$(CXX) -std=c++20 -Iinclude -I. \
		-DCPPTB_CORO_FRAME_POOL_DIAGNOSTICS \
		-DCPPTB_CORO_WAIT_PATH_DIAGNOSTICS \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		tests/unit/coro_runtime_test.cpp -o $@

cpp-coro-runtime-test: $(CPPTB_CORO_RUNTIME_TEST)
	$(CPPTB_CORO_RUNTIME_TEST)

$(CPPTB_PACKED_VALUE_TEST): include/cpptb/packed_bits.hpp include/cpptb/fixed.hpp \
		tests/unit/packed_value_test.cpp
	mkdir -p $(CPPTB_BUILD_DIR)
	$(CXX) -std=c++20 -O3 -Iinclude -I. tests/unit/packed_value_test.cpp -o $@

cpptb-packed-value-test: $(CPPTB_PACKED_VALUE_TEST)
	$(CPPTB_PACKED_VALUE_TEST)

$(CPPTB_RANDOM_TEST): include/cpptb/random.hpp include/cpptb/packed_bits.hpp \
		tests/unit/random_test.cpp
	mkdir -p $(CPPTB_BUILD_DIR)
	$(CXX) -std=c++20 -O3 -Iinclude -I. tests/unit/random_test.cpp -o $@

cpptb-random-test: $(CPPTB_RANDOM_TEST)
	$(CPPTB_RANDOM_TEST)

$(CPPTB_RANDOMIZED_TEST): include/cpptb/randomized.hpp include/cpptb/random.hpp \
		tests/unit/randomized_test.cpp
	mkdir -p $(CPPTB_BUILD_DIR)
	$(CXX) -std=c++20 -O3 -Iinclude -I. tests/unit/randomized_test.cpp -o $@

cpptb-randomized-test: $(CPPTB_RANDOMIZED_TEST)
	$(CPPTB_RANDOMIZED_TEST)

$(CPPTB_Z3_RANDOM_TEST): include/cpptb/z3_random_backend.hpp \
		include/cpptb/randomized.hpp tests/unit/z3_random_backend_test.cpp
	mkdir -p $(CPPTB_BUILD_DIR)
	$(CXX) -std=c++20 -O3 -Iinclude -I. $$(pkg-config --cflags z3) \
		tests/unit/z3_random_backend_test.cpp $$(pkg-config --libs z3) -o $@

cpptb-z3-random-test:
	@if pkg-config --atleast-version=$(CPPTB_MIN_Z3_VERSION) z3; then \
		$(MAKE) $(CPPTB_Z3_RANDOM_TEST) && $(CPPTB_Z3_RANDOM_TEST); \
	elif pkg-config --exists z3; then \
		echo "Z3 $$(pkg-config --modversion z3) is older than $(CPPTB_MIN_Z3_VERSION); skipping optional backend test"; \
	else \
		echo "Z3 not installed; skipping optional backend test"; \
	fi

$(CPPTB_COVERAGE_TEST): include/cpptb/coverage.hpp \
		tests/unit/coverage_test.cpp
	mkdir -p $(CPPTB_BUILD_DIR)
	$(CXX) -std=c++20 -O3 -Iinclude -I. tests/unit/coverage_test.cpp -o $@

cpptb-coverage-test: $(CPPTB_COVERAGE_TEST)
	$(CPPTB_COVERAGE_TEST)

$(CPPTB_TEST_API_TEST): include/cpptb/coro_runtime.hpp \
		include/cpptb/test_api.hpp include/cpptb/test_reporting.hpp \
		include/cpptb/test_result.hpp include/cpptb/randomized.hpp \
		tests/unit/test_api_test.cpp
	mkdir -p $(CPPTB_BUILD_DIR)
	$(CXX) -std=c++20 -Iinclude -I. \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		tests/unit/test_api_test.cpp -o $@

cpptb-test-api-test: $(CPPTB_TEST_API_TEST)
	$(CPPTB_TEST_API_TEST)

$(CPPTB_COMPONENTS_TEST): $(CPPTB_PUBLIC_HEADERS) tests/unit/components_test.cpp
	mkdir -p $(CPPTB_BUILD_DIR)
	$(CXX) -std=c++20 -Iinclude -I. \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		tests/unit/components_test.cpp -o $@

cpptb-components-test: $(CPPTB_COMPONENTS_TEST)
	$(CPPTB_COMPONENTS_TEST)

$(CPPTB_TRANSACTION_RECORDING_TEST): $(CPPTB_PUBLIC_HEADERS) tests/unit/transaction_recording_test.cpp
	mkdir -p $(CPPTB_BUILD_DIR)
	$(CXX) -std=c++20 -Iinclude -I. \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		tests/unit/transaction_recording_test.cpp -o $@

cpptb-transaction-recording-test: $(CPPTB_TRANSACTION_RECORDING_TEST)
	$(CPPTB_TRANSACTION_RECORDING_TEST)

$(CPPTB_MEMORY_MODEL_TEST): $(CPPTB_PUBLIC_HEADERS) tests/unit/memory_model_test.cpp
	mkdir -p $(CPPTB_BUILD_DIR)
	$(CXX) -std=c++20 -Iinclude -I. \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		tests/unit/memory_model_test.cpp -o $@

cpptb-memory-model-test: $(CPPTB_MEMORY_MODEL_TEST)
	$(CPPTB_MEMORY_MODEL_TEST)

$(CPPTB_REGISTER_MODEL_TEST): $(CPPTB_PUBLIC_HEADERS) tests/unit/register_model_test.cpp
	mkdir -p $(CPPTB_BUILD_DIR)
	$(CXX) -std=c++20 -Iinclude -I. \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		tests/unit/register_model_test.cpp -o $@

cpptb-register-model-test: $(CPPTB_REGISTER_MODEL_TEST)
	$(CPPTB_REGISTER_MODEL_TEST)

$(CPPTB_REGISTER_SEQUENCES_TEST): $(CPPTB_PUBLIC_HEADERS) tests/unit/register_sequences_test.cpp
	mkdir -p $(CPPTB_BUILD_DIR)
	$(CXX) -std=c++20 -Iinclude -I. \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		tests/unit/register_sequences_test.cpp -o $@

cpptb-register-sequences-test: $(CPPTB_REGISTER_SEQUENCES_TEST)
	$(CPPTB_REGISTER_SEQUENCES_TEST)

$(CPPTB_REGISTER_COVERAGE_TEST): $(CPPTB_PUBLIC_HEADERS) tests/unit/register_coverage_test.cpp
	mkdir -p $(CPPTB_BUILD_DIR)
	$(CXX) -std=c++20 -Iinclude -I. \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		tests/unit/register_coverage_test.cpp -o $@

cpptb-register-coverage-test: $(CPPTB_REGISTER_COVERAGE_TEST)
	$(CPPTB_REGISTER_COVERAGE_TEST)

$(CPPTB_HIERARCHY_TEST): include/cpptb/hierarchy.hpp include/cpptb/probe.hpp \
		include/cpptb/packed_bits.hpp tests/unit/hierarchy_test.cpp
	mkdir -p $(CPPTB_BUILD_DIR)
	$(CXX) -std=c++20 -O3 -DCPPTB_HIERARCHY_DISCOVERY -Iinclude -I. \
		-I$(VERILATOR_ROOT)/include/vltstd \
		tests/unit/hierarchy_test.cpp -o $@

cpptb-hierarchy-test: $(CPPTB_HIERARCHY_TEST)
	$(CPPTB_HIERARCHY_TEST)

$(CPPTB_APB_EVENT_OBJ_DIR)/Vvpi_apb_event_unit.mk: $(CPPTB_APB_EVENT_RTL)
	mkdir -p $(CPPTB_APB_EVENT_OBJ_DIR)
	verilator --cc --vpi --public-flat-rw -Wno-MULTIDRIVEN \
		--Mdir $(CPPTB_APB_EVENT_OBJ_DIR) \
		--top-module vpi_apb_event_unit \
		$(CPPTB_APB_EVENT_RTL)

$(CPPTB_APB_EVENT_OBJ_DIR)/Vvpi_apb_event_unit__ALL.a: $(CPPTB_APB_EVENT_OBJ_DIR)/Vvpi_apb_event_unit.mk
	$(MAKE) -C $(CPPTB_APB_EVENT_OBJ_DIR) -f Vvpi_apb_event_unit.mk Vvpi_apb_event_unit__ALL.a CXXFLAGS="$(VERILATOR_PLATFORM_CXXFLAGS)"

$(CPPTB_BUILD_DIR)/apb_event_host: include/cpptb/coro_runtime.hpp include/cpptb/packed_bits.hpp \
		experiments/cpp_vpi/rggen_apb_event/apb_event_dut.hpp \
		experiments/cpp_vpi/rggen_apb_event/verilator_host.cpp \
		experiments/cpp_vpi/rggen_apb_event/tests/apb_event_test.cpp \
		experiments/cpp_vpi/rggen_apb_event/tests/apb_event_test.hpp \
		$(CPPTB_APB_EVENT_OBJ_DIR)/Vvpi_apb_event_unit__ALL.a
	$(CXX) -std=c++20 \
		-Iinclude -I. \
		-I$(CPPTB_APB_EVENT_OBJ_DIR) \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		$(VERILATOR_PLATFORM_CXXFLAGS) \
		experiments/cpp_vpi/rggen_apb_event/verilator_host.cpp \
		experiments/cpp_vpi/rggen_apb_event/tests/apb_event_test.cpp \
		$(CPPTB_APB_EVENT_OBJ_DIR)/Vvpi_apb_event_unit__ALL.a \
		$(VERILATOR_ROOT)/include/verilated.cpp \
		$(VERILATOR_ROOT)/include/verilated_threads.cpp \
		$(VERILATOR_ROOT)/include/verilated_vpi.cpp \
		-o $@

cpp-apb-event-run: $(CPPTB_BUILD_DIR)/apb_event_host
	./$(CPPTB_BUILD_DIR)/apb_event_host

$(CPPTB_BENCH_BUILD_DIR)/apb_event_bench_host: include/cpptb/coro_runtime.hpp include/cpptb/packed_bits.hpp \
		experiments/cpp_vpi/rggen_apb_event/apb_event_dut.hpp \
		experiments/cocotb_cpp_comparison/cpptb/apb_event_bench.cpp \
		experiments/cocotb_cpp_comparison/cpptb/apb_event_bench.hpp \
		experiments/cocotb_cpp_comparison/cpptb/verilator_bench_host.cpp \
		$(CPPTB_APB_EVENT_OBJ_DIR)/Vvpi_apb_event_unit__ALL.a
	mkdir -p $(CPPTB_BENCH_BUILD_DIR)
	$(CXX) -std=c++20 \
		-Iinclude -I. \
		-I$(CPPTB_APB_EVENT_OBJ_DIR) \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		$(VERILATOR_PLATFORM_CXXFLAGS) \
		experiments/cocotb_cpp_comparison/cpptb/verilator_bench_host.cpp \
		experiments/cocotb_cpp_comparison/cpptb/apb_event_bench.cpp \
		$(CPPTB_APB_EVENT_OBJ_DIR)/Vvpi_apb_event_unit__ALL.a \
		$(VERILATOR_ROOT)/include/verilated.cpp \
		$(VERILATOR_ROOT)/include/verilated_threads.cpp \
		$(VERILATOR_ROOT)/include/verilated_vpi.cpp \
		-o $@

cpp-apb-event-bench-build: $(CPPTB_BENCH_BUILD_DIR)/apb_event_bench_host

cpp-apb-event-bench-run: $(CPPTB_BENCH_BUILD_DIR)/apb_event_bench_host
	./$(CPPTB_BENCH_BUILD_DIR)/apb_event_bench_host

cpptb-codegen-test:
	$(CODEGEN_PYTHON) -m unittest discover -s tests/codegen

cpptb-peakrdl-test:
	$(PEAKRDL_PYTHON) -m unittest tests.codegen.test_peakrdl_exporter

cpptb-codegen-frontend-check: $(CPPTB_EXAMPLE_FRONTEND_CHECK_TARGETS) \
		authoring-core-dpi-codegen-check
	$(CPPTB_CODEGEN) \
		$(AUTHORING_CORE_DPI_MANIFEST) \
		--clock-config $(AUTHORING_CORE_CLOCK_CONFIG) \
		--access-config $(AUTHORING_CORE_ACCESS_CONFIG) \
		--check --compare-frontend verilator_json
	$(CPPTB_CODEGEN) \
		$(PERIPHERAL_SUITE_DPI_MANIFEST) --check --compare-frontend verilator_json
	$(CPPTB_CODEGEN) \
		$(CPPTB_CONFORMANCE_MANIFEST) --check --compare-frontend verilator_json

cpptb-conformance-codegen:
	$(CPPTB_CODEGEN) $(CPPTB_CONFORMANCE_MANIFEST)

cpptb-conformance-codegen-check:
	$(CPPTB_CODEGEN) \
		$(CPPTB_CONFORMANCE_MANIFEST) --check

cpptb-conformance-frontend-check:
	$(CPPTB_CODEGEN) \
		$(CPPTB_CONFORMANCE_MANIFEST) --check --compare-frontend verilator_json

$(CPPTB_CONFORMANCE_BINARY): $(CPPTB_CODEGEN_SOURCES) \
		$(CPPTB_CONFORMANCE_MANIFEST) $(CPPTB_CONFORMANCE_RUNNER) \
		$(CPPTB_CONFORMANCE_DIR)/scheduler_conformance.sv \
		$(CPPTB_CONFORMANCE_DIR)/framework.hpp \
		$(CPPTB_CONFORMANCE_DIR)/framework.cpp \
		$(CPPTB_CONFORMANCE_DIR)/dpi_transport.cpp \
		$(CPPTB_CONFORMANCE_DIR)/testbench.cpp \
		include/cpptb/coro_runtime.hpp include/cpptb/dpi_static_binding.hpp include/cpptb/packed_bits.hpp include/cpptb/probe.hpp include/cpptb/dpi_runtime.hpp \
		include/cpptb/test_result.hpp
	$(CODEGEN_PYTHON) $(CPPTB_CONFORMANCE_RUNNER) --build-only
	touch $@

cpptb-conformance-build: $(CPPTB_CONFORMANCE_BINARY)

cpptb-conformance-run: $(CPPTB_CONFORMANCE_BINARY)
	$(CODEGEN_PYTHON) $(CPPTB_CONFORMANCE_RUNNER) --no-build

cpptb-conformance-vpi-run:
	$(CODEGEN_PYTHON) $(CPPTB_CONFORMANCE_RUNNER) --timing-backend vpi

$(PERIPHERAL_SUITE_VPI_OBJ_DIR)/Vvpi_peripheral_suite.mk: $(PERIPHERAL_SUITE_VPI_RTL)
	mkdir -p $(PERIPHERAL_SUITE_VPI_OBJ_DIR)
	verilator --cc --vpi --public-flat-rw --no-timing \
		-MAKEFLAGS "OPT_FAST=$(PERIPHERAL_SUITE_OPT_FAST)" \
		-Wno-MULTIDRIVEN -Wno-TIMESCALEMOD -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND \
		-Ibenchmarks/peripheral_suite/rtl \
		--Mdir $(PERIPHERAL_SUITE_VPI_OBJ_DIR) \
		--top-module vpi_peripheral_suite \
		$(PERIPHERAL_SUITE_VPI_RTL)

$(PERIPHERAL_SUITE_VPI_OBJ_DIR)/Vvpi_peripheral_suite__ALL.a: $(PERIPHERAL_SUITE_VPI_OBJ_DIR)/Vvpi_peripheral_suite.mk
	$(MAKE) -C $(PERIPHERAL_SUITE_VPI_OBJ_DIR) -f Vvpi_peripheral_suite.mk Vvpi_peripheral_suite__ALL.a \
		OPT_FAST="$(PERIPHERAL_SUITE_OPT_FAST)" \
		CXXFLAGS="$(PERIPHERAL_SUITE_OPT_FAST) $(VERILATOR_PLATFORM_CXXFLAGS)"

$(PERIPHERAL_SUITE_BUILD_DIR)/peripheral_suite_host: include/cpptb/coro_runtime.hpp include/cpptb/packed_bits.hpp \
		benchmarks/peripheral_suite/testbenches/cpp_vpi/framework/peripheral_suite.hpp \
		benchmarks/peripheral_suite/testbenches/cpp_vpi/framework/peripheral_suite_bench.cpp \
		benchmarks/peripheral_suite/testbenches/cpp_vpi/framework/peripheral_suite_bench.hpp \
		benchmarks/peripheral_suite/testbenches/cpp_vpi/framework/peripheral_suite_dut.hpp \
		benchmarks/peripheral_suite/testbenches/cpp_vpi/framework/peripheral_suite_fixture.cpp \
		benchmarks/peripheral_suite/testbenches/cpp_vpi/framework/peripheral_suite_fixture.hpp \
		benchmarks/peripheral_suite/testbenches/cpp_vpi/framework/verilator_suite_host.cpp \
		benchmarks/peripheral_suite/testbenches/cpp_vpi/testbench.cpp \
		$(PERIPHERAL_SUITE_VPI_OBJ_DIR)/Vvpi_peripheral_suite__ALL.a
	mkdir -p $(PERIPHERAL_SUITE_BUILD_DIR)
	$(CXX) -std=c++20 $(PERIPHERAL_SUITE_OPT_FAST) \
		-Iinclude -I. \
		-I$(PERIPHERAL_SUITE_VPI_OBJ_DIR) \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		$(VERILATOR_PLATFORM_CXXFLAGS) \
		benchmarks/peripheral_suite/testbenches/cpp_vpi/framework/verilator_suite_host.cpp \
		benchmarks/peripheral_suite/testbenches/cpp_vpi/framework/peripheral_suite_bench.cpp \
		benchmarks/peripheral_suite/testbenches/cpp_vpi/framework/peripheral_suite_fixture.cpp \
		benchmarks/peripheral_suite/testbenches/cpp_vpi/testbench.cpp \
		$(PERIPHERAL_SUITE_VPI_OBJ_DIR)/Vvpi_peripheral_suite__ALL.a \
		$(VERILATOR_ROOT)/include/verilated.cpp \
		$(VERILATOR_ROOT)/include/verilated_threads.cpp \
		$(VERILATOR_ROOT)/include/verilated_vpi.cpp \
		-o $@

peripheral-suite-build: $(PERIPHERAL_SUITE_BUILD_DIR)/peripheral_suite_host

peripheral-suite-run: $(PERIPHERAL_SUITE_BUILD_DIR)/peripheral_suite_host
	./$(PERIPHERAL_SUITE_BUILD_DIR)/peripheral_suite_host

$(PERIPHERAL_SUITE_SV_OBJ_DIR)/Vperipheral_suite_sv_tb: $(PERIPHERAL_SUITE_SV_RTL)
	mkdir -p $(PERIPHERAL_SUITE_SV_OBJ_DIR)
	verilator --binary --timing \
		-MAKEFLAGS "OPT_FAST=$(PERIPHERAL_SUITE_OPT_FAST)" \
		-Wno-MULTIDRIVEN -Wno-TIMESCALEMOD -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND \
		-Wno-WIDTH -Wno-BLKSEQ -Wno-UNUSEDSIGNAL \
		-Ibenchmarks/peripheral_suite/rtl \
		--Mdir $(PERIPHERAL_SUITE_SV_OBJ_DIR) \
		--top-module peripheral_suite_sv_tb \
		$(PERIPHERAL_SUITE_SV_RTL)

peripheral-suite-sv-build: $(PERIPHERAL_SUITE_SV_OBJ_DIR)/Vperipheral_suite_sv_tb

peripheral-suite-sv-run: $(PERIPHERAL_SUITE_SV_OBJ_DIR)/Vperipheral_suite_sv_tb
	$(PERIPHERAL_SUITE_SV_OBJ_DIR)/Vperipheral_suite_sv_tb +PERIPHERAL_SUITE_ITERS=$${PERIPHERAL_SUITE_ITERS:-1000}

$(PERIPHERAL_SUITE_DPI_CODEGEN_STAMP): $(CPPTB_CODEGEN_SOURCES) \
		$(PERIPHERAL_SUITE_DPI_MANIFEST) $(PERIPHERAL_SUITE_CORE_RTL)
	mkdir -p $(dir $@)
	$(CPPTB_CODEGEN) $(PERIPHERAL_SUITE_DPI_MANIFEST)
	touch $@

$(PERIPHERAL_SUITE_DPI_GENERATED): $(PERIPHERAL_SUITE_DPI_CODEGEN_STAMP)
	@if [ ! -f $@ ]; then \
		$(CPPTB_CODEGEN) $(PERIPHERAL_SUITE_DPI_MANIFEST); \
	fi
	@touch $@

peripheral-suite-dpi-codegen: $(PERIPHERAL_SUITE_DPI_GENERATED)

peripheral-suite-dpi-codegen-check:
	$(CPPTB_CODEGEN) $(PERIPHERAL_SUITE_DPI_MANIFEST) --check

$(PERIPHERAL_SUITE_DPI_OBJ_DIR)/Vdpi_peripheral_suite: $(PERIPHERAL_SUITE_DPI_RTL) \
		benchmarks/peripheral_suite/testbenches/cpp_dpi/framework/dpi_transport.cpp \
		benchmarks/peripheral_suite/testbenches/cpp_dpi/framework/peripheral_suite.hpp \
		benchmarks/peripheral_suite/testbenches/cpp_dpi/framework/peripheral_suite_bench.cpp \
		benchmarks/peripheral_suite/testbenches/cpp_dpi/framework/peripheral_suite_bench.hpp \
		benchmarks/peripheral_suite/testbenches/cpp_dpi/framework/peripheral_suite_fixture.cpp \
		benchmarks/peripheral_suite/testbenches/cpp_dpi/framework/peripheral_suite_fixture.hpp \
		$(PERIPHERAL_SUITE_DPI_GENERATED) \
		benchmarks/peripheral_suite/testbenches/cpp_dpi/testbench.cpp \
		include/cpptb/coro_runtime.hpp include/cpptb/packed_bits.hpp include/cpptb/dpi_runtime.hpp \
		include/cpptb/test_result.hpp
	mkdir -p $(PERIPHERAL_SUITE_DPI_OBJ_DIR)
	verilator --binary --timing \
		--no-sched-zero-delay \
		-MAKEFLAGS "OPT_FAST=$(PERIPHERAL_SUITE_OPT_FAST)" \
		-Wno-MULTIDRIVEN -Wno-TIMESCALEMOD -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND \
		-Wno-WIDTH -Wno-BLKSEQ -Wno-UNUSEDSIGNAL \
		-Ibenchmarks/peripheral_suite/rtl \
		-CFLAGS "-std=c++20 $(PERIPHERAL_SUITE_OPT_FAST) -I$(CURDIR) -I$(CURDIR)/include" \
		--Mdir $(PERIPHERAL_SUITE_DPI_OBJ_DIR) \
		--top-module dpi_peripheral_suite \
		$(PERIPHERAL_SUITE_DPI_RTL) \
		benchmarks/peripheral_suite/testbenches/cpp_dpi/framework/dpi_transport.cpp \
		benchmarks/peripheral_suite/testbenches/cpp_dpi/framework/peripheral_suite_bench.cpp \
		benchmarks/peripheral_suite/testbenches/cpp_dpi/framework/peripheral_suite_fixture.cpp \
		benchmarks/peripheral_suite/testbenches/cpp_dpi/testbench.cpp

peripheral-suite-dpi-build: $(PERIPHERAL_SUITE_DPI_OBJ_DIR)/Vdpi_peripheral_suite

peripheral-suite-dpi-run: $(PERIPHERAL_SUITE_DPI_OBJ_DIR)/Vdpi_peripheral_suite
	$(PERIPHERAL_SUITE_DPI_OBJ_DIR)/Vdpi_peripheral_suite +PERIPHERAL_SUITE_ITERS=$${PERIPHERAL_SUITE_ITERS:-1000}

$(AUTHORING_CORE_DPI_CODEGEN_STAMP): $(CPPTB_CODEGEN_SOURCES) \
		$(AUTHORING_CORE_DPI_MANIFEST) $(AUTHORING_CORE_RTL) \
		$(AUTHORING_CORE_CLOCK_DISCOVERY_CPP) \
		$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/testbench.cpp \
		include/cpptb/clock_discovery.hpp
	mkdir -p $(dir $@)
	$(CPPTB_CODEGEN) $(AUTHORING_CORE_DPI_MANIFEST)
	$(CXX) -std=c++20 -DCPPTB_HIERARCHY_DISCOVERY \
		-I$(CURDIR) -I$(CURDIR)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		$(AUTHORING_CORE_CLOCK_DISCOVERY_CPP) \
		$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/testbench.cpp \
		-o $(AUTHORING_CORE_CLOCK_DISCOVERY)
	$(AUTHORING_CORE_CLOCK_DISCOVERY) $(AUTHORING_CORE_CLOCK_CONFIG) \
		$(AUTHORING_CORE_ACCESS_CONFIG)
	$(CPPTB_CODEGEN) $(AUTHORING_CORE_DPI_MANIFEST) \
		--clock-config $(AUTHORING_CORE_CLOCK_CONFIG) \
		--access-config $(AUTHORING_CORE_ACCESS_CONFIG)
	touch $@

$(AUTHORING_CORE_DPI_GENERATED): $(AUTHORING_CORE_DPI_CODEGEN_STAMP)
	@test -f $@

authoring-core-dpi-codegen: $(AUTHORING_CORE_DPI_GENERATED)

authoring-core-dpi-codegen-check:
	mkdir -p $(AUTHORING_CORE_BUILD_DIR)
	$(CXX) -std=c++20 -DCPPTB_HIERARCHY_DISCOVERY \
		-I$(CURDIR) -I$(CURDIR)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		$(AUTHORING_CORE_CLOCK_DISCOVERY_CPP) \
		$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/testbench.cpp \
		-o $(AUTHORING_CORE_CLOCK_DISCOVERY)
	$(AUTHORING_CORE_CLOCK_DISCOVERY) $(AUTHORING_CORE_CLOCK_CONFIG) \
		$(AUTHORING_CORE_ACCESS_CONFIG)
	$(CPPTB_CODEGEN) $(AUTHORING_CORE_DPI_MANIFEST) \
		--clock-config $(AUTHORING_CORE_CLOCK_CONFIG) \
		--access-config $(AUTHORING_CORE_ACCESS_CONFIG) --check

define AUTHORING_CORE_DPI_template
$(AUTHORING_CORE_BUILD_DIR)/cpp_dpi_$(1)/Vdpi_authoring_core: \
		$(AUTHORING_CORE_RTL) $(AUTHORING_CORE_CPP) \
		$(AUTHORING_CORE_DPI_GENERATED) $(CPPTB_PUBLIC_HEADERS) \
		$(if $(filter mixed_logging,$(1)),$(CPPTB_SV_LOGGING_ASSETS)) \
		$(CPPTB_CODEGEN_SOURCES) Makefile $(AUTHORING_CORE_BUILD_PROVENANCE) \
		$(if $(filter timing_phases timing_phases_deferred,$(1)),src/verilator_timing_main.cpp)
	mkdir -p $$(dir $$@)
	verilator $(if $(filter timing_phases timing_phases_deferred,$(1)),--cc --exe --build --vpi,--binary) --timing --no-sched-zero-delay \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-BLKSEQ -Wno-BLKANDNBLK -Wno-UNUSEDSIGNAL \
		-Wno-MULTIDRIVEN \
		$(if $(filter mixed_logging,$(1)),-I$$(CURDIR)/include -DCPPTB_ENABLE_SV_LOGGING -DAUTHORING_CORE_MIXED_LOGGING) \
		-MAKEFLAGS "OPT_FAST=$$(AUTHORING_CORE_OPT_FAST)" \
		-CFLAGS "-std=c++20 $$(AUTHORING_CORE_OPT_FAST) -I$$(CURDIR) -I$$(CURDIR)/include -DAUTHORING_CORE_KERNEL=$(2) $(if $(filter timing_phases timing_phases_deferred,$(1)),-DCPPTB_VERILATED_TOP=Vdpi_authoring_core -DCPPTB_VERILATOR_DIRECT_TIMING) $(if $(filter timing_phases_deferred,$(1)),-DCPPTB_DEFERRED_WRITES) $$(AUTHORING_CORE_EXTRA_CFLAGS)" \
		$$(if $$(strip $$(AUTHORING_CORE_EXTRA_LDFLAGS)),-LDFLAGS "$$(AUTHORING_CORE_EXTRA_LDFLAGS)",) \
		--Mdir $$(dir $$@) \
		--top-module dpi_authoring_core \
		$(if $(filter mixed_logging,$(1)),include/cpptb/sv/cpptb_log_pkg.sv) \
		$(AUTHORING_CORE_RTL) \
		$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/generated/dpi_authoring_core.sv \
		$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/framework/dpi_transport.cpp \
		$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/testbench.cpp \
		$(if $(filter mixed_logging,$(1)),include/cpptb/sv/cpptb_sv_log_bridge.cpp) \
		$(if $(filter timing_phases timing_phases_deferred,$(1)),src/verilator_timing_main.cpp)
	python3 $(AUTHORING_CORE_BUILD_PROVENANCE) stamp --mode cpp_dpi \
		--kernel $(1) --binary $$@ \
		--opt-fast="$$(AUTHORING_CORE_OPT_FAST)" \
		--converge-limit="$$(AUTHORING_CORE_CONVERGE_LIMIT)" \
		--extra-cflags="$$(AUTHORING_CORE_EXTRA_CFLAGS)" \
		--extra-ldflags="$$(AUTHORING_CORE_EXTRA_LDFLAGS)" \
		--cxx="$$(CXX)" --cxxflags="$$(CXXFLAGS)" \
		--cppflags="$$(CPPFLAGS)" --ldflags="$$(LDFLAGS)" \
		--verilator="verilator"
endef

$(eval $(call AUTHORING_CORE_DPI_template,control,0))
$(eval $(call AUTHORING_CORE_DPI_template,task_value,1))
$(eval $(call AUTHORING_CORE_DPI_template,clock_cycles,2))
$(eval $(call AUTHORING_CORE_DPI_template,timeout,3))
$(eval $(call AUTHORING_CORE_DPI_template,wait_until,4))
$(eval $(call AUTHORING_CORE_DPI_template,event,5))
$(eval $(call AUTHORING_CORE_DPI_template,queue,6))
$(eval $(call AUTHORING_CORE_DPI_template,all,7))
$(eval $(call AUTHORING_CORE_DPI_template,task_timeout,8))
$(eval $(call AUTHORING_CORE_DPI_template,wide64,9))
$(eval $(call AUTHORING_CORE_DPI_template,wide_echo_137,10))
$(eval $(call AUTHORING_CORE_DPI_template,wide_slice,11))
$(eval $(call AUTHORING_CORE_DPI_template,fixed_mac,12))
$(eval $(call AUTHORING_CORE_DPI_template,array_index,13))
$(eval $(call AUTHORING_CORE_DPI_template,array_wide,14))
$(eval $(call AUTHORING_CORE_DPI_template,mem_rw,15))
$(eval $(call AUTHORING_CORE_DPI_template,hier_probe,16))
$(eval $(call AUTHORING_CORE_DPI_template,mem_backdoor,17))
$(eval $(call AUTHORING_CORE_DPI_template,mem_probe_read,18))
$(eval $(call AUTHORING_CORE_DPI_template,mem_probe_deposit,19))
$(eval $(call AUTHORING_CORE_DPI_template,mem_probe_read_deposit,20))
$(eval $(call AUTHORING_CORE_DPI_template,signal_edge,21))
$(eval $(call AUTHORING_CORE_DPI_template,array_multidim,22))
$(eval $(call AUTHORING_CORE_DPI_template,force_release,23))
$(eval $(call AUTHORING_CORE_DPI_template,packed_view,24))
$(eval $(call AUTHORING_CORE_DPI_template,force_direct,25))
$(eval $(call AUTHORING_CORE_DPI_template,hier_data,26))
$(eval $(call AUTHORING_CORE_DPI_template,timing_phases,27))
$(eval $(call AUTHORING_CORE_DPI_template,timing_phases_deferred,27))
$(eval $(call AUTHORING_CORE_DPI_template,queue_sync,28))
$(eval $(call AUTHORING_CORE_DPI_template,test_lifecycle,29))
$(eval $(call AUTHORING_CORE_DPI_template,dynamic_spawn,30))
$(eval $(call AUTHORING_CORE_DPI_template,dynamic_task,31))
$(eval $(call AUTHORING_CORE_DPI_template,dynamic_spawn_scheduler,32))
$(eval $(call AUTHORING_CORE_DPI_template,dynamic_spawn_suspending,33))
$(eval $(call AUTHORING_CORE_DPI_template,dynamic_monitor,34))
$(eval $(call AUTHORING_CORE_DPI_template,analysis_fanout,35))
$(eval $(call AUTHORING_CORE_DPI_template,random_stimulus,36))
$(eval $(call AUTHORING_CORE_DPI_template,constrained_packet,37))
$(eval $(call AUTHORING_CORE_DPI_template,constraint_extensions,38))
$(eval $(call AUTHORING_CORE_DPI_template,coverage_sampling,39))
$(eval $(call AUTHORING_CORE_DPI_template,coverage_native,59))
$(eval $(call AUTHORING_CORE_DPI_template,apb_component,40))
$(eval $(call AUTHORING_CORE_DPI_template,transaction_recording,58))
$(eval $(call AUTHORING_CORE_DPI_template,process_pipeline,41))
$(eval $(call AUTHORING_CORE_DPI_template,memory_model,42))
$(eval $(call AUTHORING_CORE_DPI_template,memory_model_direct,43))
$(eval $(call AUTHORING_CORE_DPI_template,register_prediction_validity,44))
$(eval $(call AUTHORING_CORE_DPI_template,register_backdoor,45))
$(eval $(call AUTHORING_CORE_DPI_template,register_hierarchy,46))
$(eval $(call AUTHORING_CORE_DPI_template,register_split,47))
$(eval $(call AUTHORING_CORE_DPI_template,register_wide,48))
$(eval $(call AUTHORING_CORE_DPI_template,register_enum,49))
$(eval $(call AUTHORING_CORE_DPI_template,register_memory,50))
$(eval $(call AUTHORING_CORE_DPI_template,register_sequences,51))
$(eval $(call AUTHORING_CORE_DPI_template,register_coverage,52))
$(eval $(call AUTHORING_CORE_DPI_template,register_maps,53))
$(eval $(call AUTHORING_CORE_DPI_template,register_user_effects,54))
$(eval $(call AUTHORING_CORE_DPI_template,structured_logging,55))
$(eval $(call AUTHORING_CORE_DPI_template,structured_log_history,56))
$(eval $(call AUTHORING_CORE_DPI_template,mixed_logging,57))

define AUTHORING_CORE_SV_DPI_TIMING_template
$(2)/Vdpi_authoring_core: $(AUTHORING_CORE_RTL) $(AUTHORING_CORE_CPP) \
		$(AUTHORING_CORE_DPI_GENERATED) include/cpptb/coro_runtime.hpp \
		include/cpptb/dpi_runtime.hpp include/cpptb/dpi_static_binding.hpp Makefile
	mkdir -p $(2)
	verilator --binary --timing --no-sched-zero-delay \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-BLKSEQ -Wno-BLKANDNBLK -Wno-UNUSEDSIGNAL \
		-Wno-MULTIDRIVEN \
		-DCPPTB_SV_DPI_TIMING $(3) \
		-MAKEFLAGS "OPT_FAST=$$(AUTHORING_CORE_OPT_FAST)" \
		-CFLAGS "-std=c++20 $$(AUTHORING_CORE_OPT_FAST) -I$$(CURDIR) -I$$(CURDIR)/include -DAUTHORING_CORE_KERNEL=27 -DCPPTB_SV_DPI_TIMING $(4)" \
		--Mdir $(2) \
		--top-module dpi_authoring_core \
		$$(AUTHORING_CORE_RTL) \
		$$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/generated/dpi_authoring_core.sv \
		$$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/framework/dpi_transport.cpp \
		$$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/testbench.cpp
endef

$(eval $(call AUTHORING_CORE_SV_DPI_TIMING_template,inline,$(AUTHORING_CORE_TIMING_INLINE_DIR),,))
$(eval $(call AUTHORING_CORE_SV_DPI_TIMING_template,nba,$(AUTHORING_CORE_TIMING_NBA_DIR),-DCPPTB_SV_DPI_NBA_TIMING,-DCPPTB_SV_DPI_NBA_TIMING))
$(eval $(call AUTHORING_CORE_SV_DPI_TIMING_template,calendar,$(AUTHORING_CORE_TIMING_CALENDAR_DIR),-DCPPTB_SV_DPI_NBA_TIMING -DCPPTB_SV_DPI_CALENDAR_TIMING,-DCPPTB_SV_DPI_NBA_TIMING -DCPPTB_SV_DPI_CALENDAR_TIMING))

$(AUTHORING_CORE_TIMING_VPI_DIR)/Vdpi_authoring_core: \
		$(AUTHORING_CORE_RTL) $(AUTHORING_CORE_CPP) $(AUTHORING_CORE_DPI_GENERATED) \
		include/cpptb/coro_runtime.hpp include/cpptb/dpi_runtime.hpp \
		include/cpptb/dpi_static_binding.hpp src/verilator_timing_main.cpp Makefile
	mkdir -p $(AUTHORING_CORE_TIMING_VPI_DIR)
	verilator --cc --exe --build --vpi --timing --no-sched-zero-delay \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-BLKSEQ -Wno-BLKANDNBLK -Wno-UNUSEDSIGNAL \
		-Wno-MULTIDRIVEN \
		-MAKEFLAGS "OPT_FAST=$(AUTHORING_CORE_OPT_FAST)" \
		-CFLAGS "-std=c++20 $(AUTHORING_CORE_OPT_FAST) -I$(CURDIR) -I$(CURDIR)/include -DAUTHORING_CORE_KERNEL=27 -DCPPTB_VERILATED_TOP=Vdpi_authoring_core" \
		--Mdir $(AUTHORING_CORE_TIMING_VPI_DIR) \
		--top-module dpi_authoring_core \
		$(AUTHORING_CORE_RTL) \
		$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/generated/dpi_authoring_core.sv \
		$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/framework/dpi_transport.cpp \
		$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/testbench.cpp \
		src/verilator_timing_main.cpp

authoring-core-timing-experiments-build: \
		$(AUTHORING_CORE_TIMING_INLINE_DIR)/Vdpi_authoring_core \
		$(AUTHORING_CORE_TIMING_NBA_DIR)/Vdpi_authoring_core \
		$(AUTHORING_CORE_TIMING_CALENDAR_DIR)/Vdpi_authoring_core \
		$(AUTHORING_CORE_TIMING_VPI_DIR)/Vdpi_authoring_core \
		$(AUTHORING_CORE_BUILD_DIR)/cpp_dpi_timing_phases/Vdpi_authoring_core \
		authoring-core-sv-build

AUTHORING_CORE_DPI_BINARIES := $(foreach kernel,$(AUTHORING_CORE_KERNELS),$(AUTHORING_CORE_BUILD_DIR)/cpp_dpi_$(kernel)/Vdpi_authoring_core)

.DELETE_ON_ERROR: $(AUTHORING_CORE_DPI_BINARIES) \
	$(AUTHORING_CORE_SV_OBJ_DIR)/Vauthoring_core_sv_tb \
	$(AUTHORING_CORE_BUILD_DIR)/force_direct_sv_obj/Vforce_direct_sv_tb

authoring-core-dpi-build: $(AUTHORING_CORE_DPI_BINARIES)

authoring-core-dpi-run: $(AUTHORING_CORE_BUILD_DIR)/cpp_dpi_$(AUTHORING_CORE_KERNEL)/Vdpi_authoring_core
	$(AUTHORING_CORE_BUILD_DIR)/cpp_dpi_$(AUTHORING_CORE_KERNEL)/Vdpi_authoring_core \
		+AUTHORING_CORE_ITERS=$${AUTHORING_CORE_ITERS:-10000}

$(AUTHORING_CORE_SV_OBJ_DIR)/Vauthoring_core_sv_tb: \
		$(AUTHORING_CORE_RTL) $(AUTHORING_CORE_DIR)/testbenches/systemverilog/authoring_core_sv_tb.sv \
		Makefile $(AUTHORING_CORE_BUILD_PROVENANCE)
	mkdir -p $(AUTHORING_CORE_SV_OBJ_DIR)
	verilator --binary --timing \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-BLKSEQ -Wno-BLKANDNBLK -Wno-UNUSEDSIGNAL \
		-Wno-MULTIDRIVEN \
		--converge-limit $(AUTHORING_CORE_CONVERGE_LIMIT) \
		-MAKEFLAGS "OPT_FAST=$(AUTHORING_CORE_OPT_FAST)" \
		$(if $(strip $(AUTHORING_CORE_EXTRA_CFLAGS)),-CFLAGS "$(AUTHORING_CORE_EXTRA_CFLAGS)",) \
		$(if $(strip $(AUTHORING_CORE_EXTRA_LDFLAGS)),-LDFLAGS "$(AUTHORING_CORE_EXTRA_LDFLAGS)",) \
		--Mdir $(AUTHORING_CORE_SV_OBJ_DIR) \
		--top-module authoring_core_sv_tb \
		$(AUTHORING_CORE_RTL) \
		$(AUTHORING_CORE_DIR)/testbenches/systemverilog/authoring_core_sv_tb.sv
	python3 $(AUTHORING_CORE_BUILD_PROVENANCE) stamp --mode pure_sv \
		--kernel shared --binary $@ \
		--opt-fast="$(AUTHORING_CORE_OPT_FAST)" \
		--converge-limit="$(AUTHORING_CORE_CONVERGE_LIMIT)" \
		--extra-cflags="$(AUTHORING_CORE_EXTRA_CFLAGS)" \
		--extra-ldflags="$(AUTHORING_CORE_EXTRA_LDFLAGS)" \
		--cxx="$(CXX)" --cxxflags="$(CXXFLAGS)" \
		--cppflags="$(CPPFLAGS)" --ldflags="$(LDFLAGS)" \
		--verilator="verilator"

authoring-core-sv-build: $(AUTHORING_CORE_SV_OBJ_DIR)/Vauthoring_core_sv_tb

$(AUTHORING_CORE_BUILD_DIR)/force_direct_sv_obj/Vforce_direct_sv_tb: \
		$(AUTHORING_CORE_RTL) $(AUTHORING_CORE_DIR)/testbenches/systemverilog/force_direct_sv_tb.sv \
		Makefile $(AUTHORING_CORE_BUILD_PROVENANCE)
	mkdir -p $(AUTHORING_CORE_BUILD_DIR)/force_direct_sv_obj
	verilator --binary --timing \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-UNUSEDSIGNAL -Wno-PINMISSING \
		-MAKEFLAGS "OPT_FAST=$(AUTHORING_CORE_OPT_FAST)" \
		$(if $(strip $(AUTHORING_CORE_EXTRA_CFLAGS)),-CFLAGS "$(AUTHORING_CORE_EXTRA_CFLAGS)",) \
		$(if $(strip $(AUTHORING_CORE_EXTRA_LDFLAGS)),-LDFLAGS "$(AUTHORING_CORE_EXTRA_LDFLAGS)",) \
		--Mdir $(AUTHORING_CORE_BUILD_DIR)/force_direct_sv_obj \
		--top-module force_direct_sv_tb \
		$(AUTHORING_CORE_RTL) \
		$(AUTHORING_CORE_DIR)/testbenches/systemverilog/force_direct_sv_tb.sv
	python3 $(AUTHORING_CORE_BUILD_PROVENANCE) stamp --mode pure_sv \
		--kernel force_direct --binary $@ \
		--opt-fast="$(AUTHORING_CORE_OPT_FAST)" \
		--converge-limit="$(AUTHORING_CORE_CONVERGE_LIMIT)" \
		--extra-cflags="$(AUTHORING_CORE_EXTRA_CFLAGS)" \
		--extra-ldflags="$(AUTHORING_CORE_EXTRA_LDFLAGS)" \
		--cxx="$(CXX)" --cxxflags="$(CXXFLAGS)" \
		--cppflags="$(CPPFLAGS)" --ldflags="$(LDFLAGS)" \
		--verilator="verilator"

authoring-core-force-direct-sv-build: $(AUTHORING_CORE_BUILD_DIR)/force_direct_sv_obj/Vforce_direct_sv_tb

authoring-core-sv-run: $(AUTHORING_CORE_SV_OBJ_DIR)/Vauthoring_core_sv_tb
	$(AUTHORING_CORE_SV_OBJ_DIR)/Vauthoring_core_sv_tb \
		+AUTHORING_CORE_ITERS=$${AUTHORING_CORE_ITERS:-10000} \
		+AUTHORING_CORE_KERNEL=$(AUTHORING_CORE_KERNEL)

$(FRAMEWORK_COMPARISON_VPI_BINARY): $(AUTHORING_CORE_RTL) \
		$(FRAMEWORK_COMPARISON_VPI_TOP) $(FRAMEWORK_COMPARISON_VPI_HOST) \
		include/cpptb/coro_runtime.hpp include/cpptb/packed_bits.hpp Makefile
	mkdir -p $(FRAMEWORK_COMPARISON_VPI_OBJ_DIR)
	verilator --cc --exe --build --timing --vpi --public-flat-rw \
		-MAKEFLAGS "OPT_FAST=$(AUTHORING_CORE_OPT_FAST)" \
		-Wno-PINMISSING -Wno-TIMESCALEMOD -Wno-WIDTH -Wno-UNUSEDSIGNAL \
		-CFLAGS "-std=c++20 $(AUTHORING_CORE_OPT_FAST) -I$(CURDIR) -I$(CURDIR)/include" \
		--Mdir $(FRAMEWORK_COMPARISON_VPI_OBJ_DIR) \
		--top-module authoring_core_vpi_top \
		$(AUTHORING_CORE_RTL) $(FRAMEWORK_COMPARISON_VPI_TOP) \
		$(FRAMEWORK_COMPARISON_VPI_HOST)

framework-comparison-vpi-build: $(FRAMEWORK_COMPARISON_VPI_BINARY)

framework-comparison-vpi-run: $(FRAMEWORK_COMPARISON_VPI_BINARY)
	FRAMEWORK_COMPARISON_WORKLOAD=$${FRAMEWORK_COMPARISON_WORKLOAD:-control} \
	FRAMEWORK_COMPARISON_ITERS=$${FRAMEWORK_COMPARISON_ITERS:-1000} \
	$(FRAMEWORK_COMPARISON_VPI_BINARY)

framework-comparison-cocotb-build:
	UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --no-project \
		--python $(COCOTB_BENCH_PYTHON) --with cocotb==2.0.1 \
		python $(FRAMEWORK_COMPARISON_COCOTB_RUNNER) --build-only

framework-comparison-build: framework-comparison-vpi-build \
		framework-comparison-cocotb-build authoring-core-sv-build

# No --skip-build: the runner's build_authoring_backends() resolves the
# cpp_dpi_<workload> kernels for its own AUTHORING_WORKLOADS list, which
# framework-comparison-build does not cover. Hardcoding that list here would
# drift from benchmarks/framework_comparison/workload.py.
framework-comparison-benchmark: framework-comparison-build
	python3 $(FRAMEWORK_COMPARISON_DIR)/run_benchmark.py

.PHONY: framework-comparison-heavy-codegen framework-comparison-heavy-codegen-check \
	framework-comparison-heavy-dpi-build framework-comparison-heavy-sv-build \
	framework-comparison-heavy-vpi-build framework-comparison-heavy-cocotb-build \
	framework-comparison-heavy-build framework-comparison-heavy-test \
	framework-comparison-heavy-benchmark

$(HEAVY_SUITE_DPI_CODEGEN_STAMP): $(CPPTB_CODEGEN_SOURCES) \
		$(HEAVY_SUITE_DPI_MANIFEST) $(HEAVY_SUITE_RTL) \
		$(HEAVY_SUITE_CLOCK_DISCOVERY_CPP) $(HEAVY_SUITE_DPI_CPP) \
		include/cpptb/clock_discovery.hpp
	mkdir -p $(HEAVY_SUITE_BUILD_DIR)
	$(CPPTB_CODEGEN) $(HEAVY_SUITE_DPI_MANIFEST)
	$(CXX) -std=c++20 -I$(CURDIR) -I$(CURDIR)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		$(HEAVY_SUITE_CLOCK_DISCOVERY_CPP) \
		$(HEAVY_SUITE_DIR)/testbenches/cpp_dpi/testbench.cpp \
		-o $(HEAVY_SUITE_CLOCK_DISCOVERY)
	$(HEAVY_SUITE_CLOCK_DISCOVERY) $(HEAVY_SUITE_CLOCK_CONFIG)
	$(CPPTB_CODEGEN) $(HEAVY_SUITE_DPI_MANIFEST) \
		--clock-config $(HEAVY_SUITE_CLOCK_CONFIG)
	touch $@

$(HEAVY_SUITE_DPI_GENERATED): $(HEAVY_SUITE_DPI_CODEGEN_STAMP)
	@test -f $@

framework-comparison-heavy-codegen: $(HEAVY_SUITE_DPI_GENERATED)

framework-comparison-heavy-codegen-check: framework-comparison-heavy-codegen
	$(CPPTB_CODEGEN) $(HEAVY_SUITE_DPI_MANIFEST) \
		--clock-config $(HEAVY_SUITE_CLOCK_CONFIG) --check

define HEAVY_SUITE_DPI_template
$(HEAVY_SUITE_BUILD_DIR)/cpp_dpi_$(1)/Vdpi_heavy_benchmark: \
		$(HEAVY_SUITE_RTL) $(HEAVY_SUITE_DPI_CPP) \
		$(HEAVY_SUITE_DPI_GENERATED) include/cpptb/coro_runtime.hpp \
		include/cpptb/dpi_runtime.hpp include/cpptb/dpi_static_binding.hpp Makefile
	mkdir -p $$(dir $$@)
	verilator --binary --timing --no-sched-zero-delay \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-BLKSEQ -Wno-UNUSEDSIGNAL \
		-MAKEFLAGS "OPT_FAST=$$(HEAVY_SUITE_OPT_FAST)" \
		-CFLAGS "-std=c++20 $$(HEAVY_SUITE_OPT_FAST) -I$$(CURDIR) -I$$(CURDIR)/include -DHEAVY_WORKLOAD=$(2)" \
		--Mdir $$(dir $$@) --top-module dpi_heavy_benchmark \
		$$(HEAVY_SUITE_RTL) \
		$$(HEAVY_SUITE_DIR)/testbenches/cpp_dpi/generated/dpi_heavy_benchmark.sv \
		$$(HEAVY_SUITE_DIR)/testbenches/cpp_dpi/framework/dpi_transport.cpp \
		$$(HEAVY_SUITE_DIR)/testbenches/cpp_dpi/testbench.cpp
endef

$(eval $(call HEAVY_SUITE_DPI_template,streaming_fir,0))
$(eval $(call HEAVY_SUITE_DPI_template,packet_crc32,1))
$(eval $(call HEAVY_SUITE_DPI_template,matrix4x4,2))

HEAVY_SUITE_DPI_BINARIES := $(foreach workload,$(HEAVY_SUITE_WORKLOADS),$(HEAVY_SUITE_BUILD_DIR)/cpp_dpi_$(workload)/Vdpi_heavy_benchmark)

framework-comparison-heavy-dpi-build: $(HEAVY_SUITE_DPI_BINARIES)

$(HEAVY_SUITE_SV_BINARY): $(HEAVY_SUITE_RTL) \
		$(HEAVY_SUITE_DIR)/testbenches/systemverilog/heavy_benchmark_sv_tb.sv Makefile
	mkdir -p $(dir $@)
	verilator --binary --timing \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-BLKSEQ -Wno-UNUSEDSIGNAL \
		-MAKEFLAGS "OPT_FAST=$(HEAVY_SUITE_OPT_FAST)" --Mdir $(dir $@) \
		--top-module heavy_benchmark_sv_tb $(HEAVY_SUITE_RTL) \
		$(HEAVY_SUITE_DIR)/testbenches/systemverilog/heavy_benchmark_sv_tb.sv

framework-comparison-heavy-sv-build: $(HEAVY_SUITE_SV_BINARY)

$(HEAVY_SUITE_VPI_BINARY): $(HEAVY_SUITE_RTL) \
		$(HEAVY_SUITE_DIR)/testbenches/cpp_vpi/heavy_benchmark_vpi_top.sv \
		$(HEAVY_SUITE_DIR)/testbenches/cpp_vpi/heavy_benchmark_vpi_host.cpp \
		include/cpptb/coro_runtime.hpp Makefile
	mkdir -p $(dir $@)
	verilator --cc --exe --build --timing --vpi --public-flat-rw \
		-MAKEFLAGS "OPT_FAST=$(HEAVY_SUITE_OPT_FAST)" \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-UNUSEDSIGNAL \
		-CFLAGS "-std=c++20 $(HEAVY_SUITE_OPT_FAST) -I$(CURDIR) -I$(CURDIR)/include" \
		--Mdir $(dir $@) --top-module heavy_benchmark_vpi_top \
		$(HEAVY_SUITE_RTL) \
		$(HEAVY_SUITE_DIR)/testbenches/cpp_vpi/heavy_benchmark_vpi_top.sv \
		$(HEAVY_SUITE_DIR)/testbenches/cpp_vpi/heavy_benchmark_vpi_host.cpp

framework-comparison-heavy-vpi-build: $(HEAVY_SUITE_VPI_BINARY)

framework-comparison-heavy-cocotb-build:
	UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --offline --no-project \
		--python $(COCOTB_BENCH_PYTHON) --with cocotb==2.0.1 \
		python $(HEAVY_SUITE_COCOTB_RUNNER) --build-only

framework-comparison-heavy-build: framework-comparison-heavy-codegen-check \
	framework-comparison-heavy-dpi-build framework-comparison-heavy-sv-build \
	framework-comparison-heavy-vpi-build framework-comparison-heavy-cocotb-build

framework-comparison-heavy-test: framework-comparison-heavy-build
	python3 -m unittest discover -s $(HEAVY_SUITE_DIR)/tests -p 'test_*.py'
	python3 $(HEAVY_SUITE_DIR)/run_benchmark.py --skip-build --iters 3 --runs 4 \
		--output-stem smoke

framework-comparison-heavy-benchmark: framework-comparison-heavy-build
	python3 $(HEAVY_SUITE_DIR)/run_benchmark.py --skip-build

.PHONY: framework-comparison-open-cores-codegen \
	framework-comparison-open-cores-codegen-check \
	framework-comparison-open-cores-dpi-build \
	framework-comparison-open-cores-sv-build \
	framework-comparison-open-cores-vpi-build \
	framework-comparison-open-cores-cocotb-build \
	framework-comparison-open-cores-cocotb-run \
	framework-comparison-open-cores-build \
	framework-comparison-open-cores-test \
	framework-comparison-open-cores-benchmark

$(OPEN_CORES_DPI_CODEGEN_STAMP): $(CPPTB_CODEGEN_SOURCES) \
		$(OPEN_CORES_DPI_MANIFEST) $(OPEN_CORES_RTL) \
		$(OPEN_CORES_CLOCK_DISCOVERY_CPP) $(OPEN_CORES_DPI_CPP) \
		include/cpptb/clock_discovery.hpp
	mkdir -p $(OPEN_CORES_BUILD_DIR)
	$(CPPTB_CODEGEN) $(OPEN_CORES_DPI_MANIFEST)
	$(CXX) -std=c++20 -I$(CURDIR) -I$(CURDIR)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		$(OPEN_CORES_CLOCK_DISCOVERY_CPP) \
		$(OPEN_CORES_DIR)/testbenches/cpp_dpi/testbench.cpp \
		-o $(OPEN_CORES_CLOCK_DISCOVERY)
	$(OPEN_CORES_CLOCK_DISCOVERY) $(OPEN_CORES_CLOCK_CONFIG)
	$(CPPTB_CODEGEN) $(OPEN_CORES_DPI_MANIFEST) \
		--clock-config $(OPEN_CORES_CLOCK_CONFIG)
	touch $@

$(OPEN_CORES_DPI_GENERATED): $(OPEN_CORES_DPI_CODEGEN_STAMP)
	@test -f $@

framework-comparison-open-cores-codegen: $(OPEN_CORES_DPI_GENERATED)

framework-comparison-open-cores-codegen-check: framework-comparison-open-cores-codegen
	$(CPPTB_CODEGEN) $(OPEN_CORES_DPI_MANIFEST) \
		--clock-config $(OPEN_CORES_CLOCK_CONFIG) --check

define OPEN_CORES_DPI_template
$(OPEN_CORES_BUILD_DIR)/cpp_dpi_$(1)/Vdpi_open_cores_benchmark: \
		$(OPEN_CORES_RTL) $(OPEN_CORES_DPI_CPP) $(OPEN_CORES_DPI_GENERATED) \
		include/cpptb/coro_runtime.hpp include/cpptb/dpi_runtime.hpp \
		include/cpptb/dpi_static_binding.hpp Makefile
	mkdir -p $$(dir $$@)
	verilator --binary --timing --no-sched-zero-delay \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-BLKSEQ -Wno-UNUSEDSIGNAL \
		-Wno-UNOPTFLAT -DOPEN_CORE_WORKLOAD=$(2) \
		-MAKEFLAGS "OPT_FAST=$$(OPEN_CORES_OPT_FAST)" \
		-CFLAGS "-std=c++20 $$(OPEN_CORES_OPT_FAST) -I$$(CURDIR) -I$$(CURDIR)/include -DOPEN_CORE_WORKLOAD=$(2)" \
		--Mdir $$(dir $$@) --top-module dpi_open_cores_benchmark \
		$$(OPEN_CORES_RTL) \
		$$(OPEN_CORES_DIR)/testbenches/cpp_dpi/generated/dpi_open_cores_benchmark.sv \
		$$(OPEN_CORES_DIR)/testbenches/cpp_dpi/framework/dpi_transport.cpp \
		$$(OPEN_CORES_DIR)/testbenches/cpp_dpi/testbench.cpp
endef

$(eval $(call OPEN_CORES_DPI_template,picorv32_firmware,0))
$(eval $(call OPEN_CORES_DPI_template,secworks_aes128,1))
$(eval $(call OPEN_CORES_DPI_template,ethernet_fcs64,2))

OPEN_CORES_DPI_BINARIES := $(foreach workload,$(OPEN_CORES_WORKLOADS),$(OPEN_CORES_BUILD_DIR)/cpp_dpi_$(workload)/Vdpi_open_cores_benchmark)

framework-comparison-open-cores-dpi-build: $(OPEN_CORES_DPI_BINARIES)

define OPEN_CORES_SV_template
$(OPEN_CORES_BUILD_DIR)/pure_sv_$(1)/Vopen_cores_sv_tb: \
		$(OPEN_CORES_RTL) \
		$(OPEN_CORES_DIR)/testbenches/systemverilog/open_cores_sv_tb.sv Makefile
	mkdir -p $$(dir $$@)
	verilator --binary --timing \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-BLKSEQ -Wno-UNUSEDSIGNAL \
		-Wno-UNOPTFLAT -DOPEN_CORE_WORKLOAD=$(2) \
		-MAKEFLAGS "OPT_FAST=$$(OPEN_CORES_OPT_FAST)" --Mdir $$(dir $$@) \
		--top-module open_cores_sv_tb $$(OPEN_CORES_RTL) \
		$$(OPEN_CORES_DIR)/testbenches/systemverilog/open_cores_sv_tb.sv
endef

$(eval $(call OPEN_CORES_SV_template,picorv32_firmware,0))
$(eval $(call OPEN_CORES_SV_template,secworks_aes128,1))
$(eval $(call OPEN_CORES_SV_template,ethernet_fcs64,2))

OPEN_CORES_SV_BINARIES := $(foreach workload,$(OPEN_CORES_WORKLOADS),$(OPEN_CORES_BUILD_DIR)/pure_sv_$(workload)/Vopen_cores_sv_tb)

framework-comparison-open-cores-sv-build: $(OPEN_CORES_SV_BINARIES)

define OPEN_CORES_VPI_template
$(OPEN_CORES_BUILD_DIR)/cpp_vpi_$(1)/Vopen_cores_benchmark_top: \
		$(OPEN_CORES_RTL) $(OPEN_CORES_DIR)/rtl/open_cores_benchmark_top.sv \
		$(OPEN_CORES_DIR)/testbenches/cpp_vpi/open_cores_vpi_host.cpp \
		include/cpptb/coro_runtime.hpp Makefile
	mkdir -p $$(dir $$@)
	verilator --cc --exe --build --timing --vpi --public-flat-rw \
		-MAKEFLAGS "OPT_FAST=$$(OPEN_CORES_OPT_FAST)" \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-UNUSEDSIGNAL -Wno-UNOPTFLAT \
		-DOPEN_CORE_WORKLOAD=$(2) \
		-CFLAGS "-std=c++20 $$(OPEN_CORES_OPT_FAST) -I$$(CURDIR) -I$$(CURDIR)/include -DOPEN_CORE_WORKLOAD=$(2)" \
		--Mdir $$(dir $$@) --top-module open_cores_benchmark_top \
		$$(OPEN_CORES_RTL) $$(OPEN_CORES_DIR)/rtl/open_cores_benchmark_top.sv \
		$$(OPEN_CORES_DIR)/testbenches/cpp_vpi/open_cores_vpi_host.cpp
endef

$(eval $(call OPEN_CORES_VPI_template,picorv32_firmware,0))
$(eval $(call OPEN_CORES_VPI_template,secworks_aes128,1))
$(eval $(call OPEN_CORES_VPI_template,ethernet_fcs64,2))

OPEN_CORES_VPI_BINARIES := $(foreach workload,$(OPEN_CORES_WORKLOADS),$(OPEN_CORES_BUILD_DIR)/cpp_vpi_$(workload)/Vopen_cores_benchmark_top)

framework-comparison-open-cores-vpi-build: $(OPEN_CORES_VPI_BINARIES)

framework-comparison-open-cores-cocotb-build:
	UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --offline --no-project \
		--python $(COCOTB_BENCH_PYTHON) --with cocotb==2.0.1 \
		python $(OPEN_CORES_COCOTB_RUNNER) --build-only --workload picorv32_firmware
	UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --offline --no-project \
		--python $(COCOTB_BENCH_PYTHON) --with cocotb==2.0.1 \
		python $(OPEN_CORES_COCOTB_RUNNER) --build-only --workload secworks_aes128
	UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --offline --no-project \
		--python $(COCOTB_BENCH_PYTHON) --with cocotb==2.0.1 \
		python $(OPEN_CORES_COCOTB_RUNNER) --build-only --workload ethernet_fcs64

framework-comparison-open-cores-cocotb-run:
	UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --offline --no-project \
		--python $(COCOTB_BENCH_PYTHON) --with cocotb==2.0.1 \
		python $(OPEN_CORES_COCOTB_RUNNER) \
		--workload $(OPEN_CORES_COCOTB_WORKLOAD) \
		--iters $(OPEN_CORES_COCOTB_ITERS)

framework-comparison-open-cores-build: \
	framework-comparison-open-cores-codegen-check \
	framework-comparison-open-cores-dpi-build \
	framework-comparison-open-cores-sv-build \
	framework-comparison-open-cores-vpi-build \
	framework-comparison-open-cores-cocotb-build

framework-comparison-open-cores-test: framework-comparison-open-cores-build
	python3 -m unittest discover -s $(OPEN_CORES_DIR)/tests -p 'test_*.py'
	python3 $(OPEN_CORES_DIR)/run_benchmark.py --skip-build --iters 1 --runs 4 \
		--output-stem smoke

framework-comparison-open-cores-benchmark: framework-comparison-open-cores-build
	python3 $(OPEN_CORES_DIR)/run_benchmark.py --skip-build

authoring-core-build: authoring-core-dpi-codegen-check authoring-core-dpi-build authoring-core-sv-build

authoring-core-benchmark: authoring-core-build
	python3 $(AUTHORING_CORE_DIR)/run_benchmark.py \
		--example $(AUTHORING_CORE_KERNEL) --skip-build

feature-list:
	$(FEATURE_REGRESSION_RUNNER) list

feature-test:
	@test -n "$(FEATURE)" || { echo "FEATURE is required" >&2; exit 2; }
	$(FEATURE_REGRESSION_RUNNER) semantic-check "$(FEATURE)"

feature-benchmark:
	@test -n "$(FEATURE)" || { echo "FEATURE is required" >&2; exit 2; }
	$(FEATURE_REGRESSION_RUNNER) benchmark "$(FEATURE)"

feature-regression:
	$(FEATURE_REGRESSION_RUNNER) regression

registry-check:
	$(FEATURE_REGRESSION_RUNNER) registry-check

clean:
	rm -rf $(BUILD_DIR)
