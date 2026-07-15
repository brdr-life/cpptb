BUILD_DIR := build
.DEFAULT_GOAL := help

OBJ_DIR := $(BUILD_DIR)/obj
MOJOTB_BUILD_DIR := $(BUILD_DIR)/mojotb
MOJOTB_OBJ_DIR := $(MOJOTB_BUILD_DIR)/obj
CPPTB_BUILD_DIR := $(BUILD_DIR)/cpptb
CPPTB_OBJ_DIR := $(CPPTB_BUILD_DIR)/obj
CPPTB_CORO_RUNTIME_TEST := $(CPPTB_BUILD_DIR)/coro_runtime_test
CPPTB_PACKED_VALUE_TEST := $(CPPTB_BUILD_DIR)/packed_value_test
CPPTB_TEST_API_TEST := $(CPPTB_BUILD_DIR)/test_api_test
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
PERIPHERAL_SUITE_VPI_OBJ_DIR := $(PERIPHERAL_SUITE_BUILD_DIR)/cpp_vpi_obj
PERIPHERAL_SUITE_SV_OBJ_DIR := $(PERIPHERAL_SUITE_BUILD_DIR)/pure_sv_obj
PERIPHERAL_SUITE_DPI_OBJ_DIR := $(PERIPHERAL_SUITE_BUILD_DIR)/cpp_dpi_obj
PERIPHERAL_SUITE_DPI_MANIFEST := benchmarks/peripheral_suite/testbenches/cpp_dpi/peripheral_suite.dpi.json
CPPTB_CODEGEN_ENTRY := tools/codegen/cpptb_codegen/generate_dpi_bindings.py
PERIPHERAL_SUITE_DPI_GENERATOR := $(CPPTB_CODEGEN_ENTRY)
AUTHORING_CORE_DIR := benchmarks/authoring_core
AUTHORING_CORE_BUILD_DIR := $(BUILD_DIR)/benchmarks/authoring_core
AUTHORING_CORE_SV_OBJ_DIR := $(AUTHORING_CORE_BUILD_DIR)/pure_sv_obj
AUTHORING_CORE_DPI_MANIFEST := $(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/authoring_core.dpi.json
AUTHORING_CORE_DPI_GENERATOR := $(CPPTB_CODEGEN_ENTRY)
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
AUTHORING_CORE_KERNELS := control task_value clock_cycles timeout task_timeout wait_until event channel all wide64 wide_echo_137 wide_slice fixed_mac array_index array_wide mem_rw hier_probe mem_backdoor mem_probe_read mem_probe_deposit mem_probe_read_deposit signal_edge array_multidim force_release packed_view force_direct hier_data timing_phases
AUTHORING_CORE_KERNEL ?= control
AUTHORING_CORE_OPT_FAST ?= -O3
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
COCOTB_BENCH_PYTHON ?= /opt/homebrew/bin/python3.12
FEATURE ?=
FEATURE_REGRESSION_RUNNER := python3 benchmarks/run_regression.py
UV_CACHE_DIR ?= $(BUILD_DIR)/uv-cache
CODEGEN_PYTHON := UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --frozen python
DOCS_RUN := UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --frozen --group docs
CPPTB_CODEGEN := UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --frozen cpptb-codegen
CPPTB_CODEGEN_SOURCES := \
	$(CPPTB_CODEGEN_ENTRY) \
	tools/codegen/cpptb_codegen/__init__.py \
	tools/codegen/cpptb_codegen/design_ir.py \
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
else
VERILATOR_PLATFORM_CXXFLAGS :=
endif
CXX ?= clang++

CPPTB_EXAMPLE_PHONY_TARGETS :=
CPPTB_EXAMPLE_TEST_TARGETS :=
CPPTB_EXAMPLE_FRONTEND_CHECK_TARGETS :=

# $(1): target slug, $(2): variable prefix, $(3): directory name,
# $(4): RTL basename, $(5): generated DPI top, $(6): pure-SV top,
# $(7): optional advanced code-generation manifest.
define CPPTB_EXAMPLE_template
CPPTB_$(2)_DIR := examples/$(3)
CPPTB_$(2)_CODEGEN_INPUT := $$(CPPTB_$(2)_DIR)/$(if $(7),$(7),$(4).sv)
CPPTB_$(2)_OBJ_DIR := $$(CPPTB_BUILD_DIR)/dpi_$(3)_obj
CPPTB_$(2)_SV_OBJ_DIR := $$(CPPTB_BUILD_DIR)/dpi_$(3)_sv_obj
CPPTB_$(2)_SV_TB := $$(CPPTB_$(2)_DIR)/systemverilog/$(6).sv
CPPTB_$(2)_GENERATED := \
	$$(CPPTB_$(2)_DIR)/generated/$(4)_dut.hpp \
	$$(CPPTB_$(2)_DIR)/generated/$(4)_binding.hpp \
	$$(CPPTB_$(2)_DIR)/generated/$(5).sv \
	$$(CPPTB_$(2)_DIR)/generated/$(5).cpp \
	$$(CPPTB_$(2)_DIR)/generated/discover_$(4)_clocks.cpp
CPPTB_$(2)_CODEGEN_STAMP := $$(CPPTB_BUILD_DIR)/dpi_$(3).codegen.stamp
CPPTB_$(2)_CLOCK_CONFIG := $$(CPPTB_BUILD_DIR)/dpi_$(3).clocks.json
CPPTB_$(2)_ACCESS_CONFIG := $$(CPPTB_BUILD_DIR)/dpi_$(3).access.json
CPPTB_$(2)_CLOCK_DISCOVERY := $$(CPPTB_BUILD_DIR)/discover_$(3)_clocks
CPPTB_$(2)_CODEGEN_COMMAND = $$(CPPTB_CODEGEN) \
	$$(CPPTB_$(2)_CODEGEN_INPUT)
CPPTB_$(2)_FINAL_CODEGEN_COMMAND = $$(CPPTB_$(2)_CODEGEN_COMMAND) \
	--clock-config $$(CPPTB_$(2)_CLOCK_CONFIG) \
	--access-config $$(CPPTB_$(2)_ACCESS_CONFIG)

CPPTB_EXAMPLE_PHONY_TARGETS += cpp-dpi-$(1)-codegen \
	cpp-dpi-$(1)-codegen-check cpp-dpi-$(1)-frontend-check \
	cpp-dpi-$(1)-build cpp-dpi-$(1)-run \
	cpp-dpi-$(1)-sv-build cpp-dpi-$(1)-sv-run
CPPTB_EXAMPLE_TEST_TARGETS += cpp-dpi-$(1)-run cpp-dpi-$(1)-sv-run
CPPTB_EXAMPLE_FRONTEND_CHECK_TARGETS += cpp-dpi-$(1)-frontend-check

$$(CPPTB_$(2)_CODEGEN_STAMP): Makefile $$(CPPTB_CODEGEN_SOURCES) \
		$$(CPPTB_$(2)_CODEGEN_INPUT) \
		$$(CPPTB_$(2)_DIR)/$(4).sv \
		$$(CPPTB_$(2)_DIR)/testbench.cpp \
		include/cpptb/clock_discovery.hpp include/cpptb/hierarchy.hpp
	mkdir -p $$(dir $$@)
	$$(CPPTB_$(2)_CODEGEN_COMMAND)
	$$(CXX) -std=c++20 -DCPPTB_HIERARCHY_DISCOVERY \
		-I$$(CURDIR) -I$$(CURDIR)/include \
		-I$$(VERILATOR_ROOT)/include/vltstd \
		$$(CPPTB_$(2)_DIR)/generated/discover_$(4)_clocks.cpp \
		$$(CPPTB_$(2)_DIR)/testbench.cpp \
		-o $$(CPPTB_$(2)_CLOCK_DISCOVERY)
	$$(CPPTB_$(2)_CLOCK_DISCOVERY) $$(CPPTB_$(2)_CLOCK_CONFIG) \
		$$(CPPTB_$(2)_ACCESS_CONFIG)
	$$(CPPTB_$(2)_FINAL_CODEGEN_COMMAND)
	touch $$@

$$(CPPTB_$(2)_GENERATED): $$(CPPTB_$(2)_CODEGEN_STAMP)
	@if [ ! -f $$@ ]; then \
		$$(CPPTB_$(2)_CODEGEN_COMMAND); \
		$$(CXX) -std=c++20 -DCPPTB_HIERARCHY_DISCOVERY \
			-I$$(CURDIR) -I$$(CURDIR)/include \
			-I$$(VERILATOR_ROOT)/include/vltstd \
			$$(CPPTB_$(2)_DIR)/generated/discover_$(4)_clocks.cpp \
			$$(CPPTB_$(2)_DIR)/testbench.cpp \
			-o $$(CPPTB_$(2)_CLOCK_DISCOVERY); \
		$$(CPPTB_$(2)_CLOCK_DISCOVERY) $$(CPPTB_$(2)_CLOCK_CONFIG) \
			$$(CPPTB_$(2)_ACCESS_CONFIG); \
		$$(CPPTB_$(2)_FINAL_CODEGEN_COMMAND); \
	fi
	@touch $$@

cpp-dpi-$(1)-codegen: $$(CPPTB_$(2)_GENERATED)

cpp-dpi-$(1)-codegen-check:
	mkdir -p $$(CPPTB_BUILD_DIR)
	$$(CXX) -std=c++20 -DCPPTB_HIERARCHY_DISCOVERY \
		-I$$(CURDIR) -I$$(CURDIR)/include \
		-I$$(VERILATOR_ROOT)/include/vltstd \
		$$(CPPTB_$(2)_DIR)/generated/discover_$(4)_clocks.cpp \
		$$(CPPTB_$(2)_DIR)/testbench.cpp \
		-o $$(CPPTB_$(2)_CLOCK_DISCOVERY)
	$$(CPPTB_$(2)_CLOCK_DISCOVERY) $$(CPPTB_$(2)_CLOCK_CONFIG) \
		$$(CPPTB_$(2)_ACCESS_CONFIG)
	$$(CPPTB_$(2)_FINAL_CODEGEN_COMMAND) --check

cpp-dpi-$(1)-frontend-check:
	$$(MAKE) cpp-dpi-$(1)-codegen-check
	$$(CPPTB_$(2)_FINAL_CODEGEN_COMMAND) \
		--check --compare-frontend verilator_json

$$(CPPTB_$(2)_OBJ_DIR)/V$(5): \
		$$(CPPTB_$(2)_DIR)/$(4).sv \
		$$(CPPTB_$(2)_DIR)/testbench.cpp \
		$$(CPPTB_$(2)_GENERATED) include/cpptb/coro_runtime.hpp \
		include/cpptb/packed_bits.hpp include/cpptb/dpi_runtime.hpp \
		include/cpptb/dpi_static_binding.hpp include/cpptb/test_api.hpp \
		include/cpptb/test_result.hpp
	mkdir -p $$(CPPTB_$(2)_OBJ_DIR)
	verilator --binary --timing --no-sched-zero-delay \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-UNUSEDSIGNAL -Wno-BLKANDNBLK \
		-Wno-MULTIDRIVEN \
		-CFLAGS "-I$$(CURDIR) -I$$(CURDIR)/include" \
		--Mdir $$(CPPTB_$(2)_OBJ_DIR) \
		--top-module $(5) \
		$$(CPPTB_$(2)_DIR)/$(4).sv \
		$$(CPPTB_$(2)_DIR)/generated/$(5).sv \
		$$(CPPTB_$(2)_DIR)/generated/$(5).cpp \
		$$(CPPTB_$(2)_DIR)/testbench.cpp

cpp-dpi-$(1)-build: $$(CPPTB_$(2)_OBJ_DIR)/V$(5)

cpp-dpi-$(1)-run: $$(CPPTB_$(2)_OBJ_DIR)/V$(5)
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

$(eval $(call CPPTB_EXAMPLE_template,counter,COUNTER,counter,counter,dpi_counter,counter_sv_tb))
$(eval $(call CPPTB_EXAMPLE_template,multiclock,MULTICLOCK,multiclock,dual_clock_mailbox,dpi_dual_clock_mailbox,dual_clock_mailbox_sv_tb))
$(eval $(call CPPTB_EXAMPLE_template,timer-only,TIMER_ONLY,timer_only,timer_only_probe,dpi_timer_only_probe,timer_only_probe_sv_tb))
$(eval $(call CPPTB_EXAMPLE_template,fifo-scoreboard,FIFO_SCOREBOARD,fifo_scoreboard,stream_fifo,dpi_stream_fifo,stream_fifo_sv_tb))
$(eval $(call CPPTB_EXAMPLE_template,apb-regfile,APB_REGFILE,apb_regfile,apb_regfile,dpi_apb_regfile,apb_regfile_sv_tb))
$(eval $(call CPPTB_EXAMPLE_template,watchdog-timeout,WATCHDOG_TIMEOUT,watchdog_timeout,stalling_responder,dpi_stalling_responder,stalling_responder_sv_tb))
$(eval $(call CPPTB_EXAMPLE_template,fault-injection,FAULT_INJECTION,fault_injection,fault_injection,dpi_fault_injection,fault_injection_sv_tb))
$(eval $(call CPPTB_EXAMPLE_template,rich-data,RICH_DATA,rich_data,rich_data,dpi_rich_data,rich_data_sv_tb))

.PHONY: help all test unit-test python-test codegen-test conformance-test examples-test docs-build docs-check docs-sphinx-build docs-sphinx-serve docs-zensical-build docs-zensical-serve run vpi-run cpp-vpi-run cpp-coro-runtime-test cpptb-packed-value-test cpptb-test-api-test cpp-apb-event-run cpp-apb-event-bench-build cpp-apb-event-bench-run cpptb-codegen-test cpptb-codegen-frontend-check cpptb-conformance-codegen cpptb-conformance-codegen-check cpptb-conformance-frontend-check cpptb-conformance-build cpptb-conformance-run cpptb-conformance-vpi-run $(CPPTB_EXAMPLE_PHONY_TARGETS) peripheral-suite-build peripheral-suite-run peripheral-suite-sv-build peripheral-suite-sv-run peripheral-suite-dpi-codegen peripheral-suite-dpi-codegen-check peripheral-suite-dpi-build peripheral-suite-dpi-run authoring-core-dpi-codegen authoring-core-dpi-codegen-check authoring-core-dpi-build authoring-core-dpi-run authoring-core-sv-build authoring-core-sv-run authoring-core-build authoring-core-benchmark framework-comparison-vpi-build framework-comparison-vpi-run framework-comparison-cocotb-build framework-comparison-build framework-comparison-benchmark feature-list feature-test feature-benchmark feature-regression registry-check clean

help:
	@printf '%s\n' \
		'cpptb development targets:' \
		'  make test                    Run all unit, codegen, conformance, and example tests' \
		'  make feature-list            List semantic/performance benchmark features' \
		'  make feature-test FEATURE=…  Run one semantic comparison' \
		'  make feature-benchmark FEATURE=…  Benchmark one C++/SV pair' \
		'  make feature-regression      Run the complete serial regression' \
		'  make docs-build              Build both documentation variants' \
		'  make docs-sphinx-serve       Preview Sphinx at localhost:8001' \
		'  make docs-zensical-serve     Preview Zensical at localhost:8002' \
		'  make clean                   Remove generated build output'

all: test

test: unit-test python-test codegen-test conformance-test examples-test registry-check

unit-test: cpp-coro-runtime-test cpptb-packed-value-test cpptb-test-api-test \
	cpptb-hierarchy-test

python-test:
	$(CODEGEN_PYTHON) -m unittest discover -s benchmarks/tests
	$(CODEGEN_PYTHON) -m unittest discover -s benchmarks/authoring_core/tests
	$(CODEGEN_PYTHON) -m unittest discover -s benchmarks/framework_comparison/tests
	$(CODEGEN_PYTHON) -m unittest discover -s benchmarks/framework_comparison/heavy_suite/tests
	$(CODEGEN_PYTHON) -m unittest discover -s benchmarks/framework_comparison/open_cores/tests
	$(CODEGEN_PYTHON) -m unittest discover -s benchmarks/peripheral_suite/tests

codegen-test: cpptb-codegen-test cpptb-codegen-frontend-check

conformance-test: cpptb-conformance-run

examples-test: $(CPPTB_EXAMPLE_TEST_TARGETS)

docs-build: docs-sphinx-build docs-zensical-build

docs-check: docs-build

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

$(BUILD_DIR)/libcounter.dylib: experiments/verilator_c_api/counter_c_api.cpp $(OBJ_DIR)/Vcounter__ALL.a
	$(CXX) -dynamiclib -std=c++17 \
		-I$(OBJ_DIR) \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		$(VERILATOR_PLATFORM_CXXFLAGS) \
		experiments/verilator_c_api/counter_c_api.cpp \
		$(OBJ_DIR)/Vcounter__ALL.a \
		$(VERILATOR_ROOT)/include/verilated.cpp \
		$(VERILATOR_ROOT)/include/verilated_threads.cpp \
		-o $@

$(BUILD_DIR)/counter_driver: experiments/verilator_c_api/counter_driver.mojo $(BUILD_DIR)/libcounter.dylib
	mojo build $< -o $@

run: all
	./$(BUILD_DIR)/counter_driver

$(MOJOTB_OBJ_DIR)/Vvpi_counter.mk: experiments/mojo_vpi/examples/vpi_counter.sv
	mkdir -p $(MOJOTB_OBJ_DIR)
	verilator --cc --vpi --public-flat-rw --Mdir $(MOJOTB_OBJ_DIR) $<

$(MOJOTB_OBJ_DIR)/Vvpi_counter__ALL.a: $(MOJOTB_OBJ_DIR)/Vvpi_counter.mk
	$(MAKE) -C $(MOJOTB_OBJ_DIR) -f Vvpi_counter.mk Vvpi_counter__ALL.a CXXFLAGS="$(VERILATOR_PLATFORM_CXXFLAGS)"

$(MOJOTB_BUILD_DIR)/libcounter_test.dylib: experiments/mojo_vpi/tests/counter_test.mojo experiments/mojo_vpi/runtime.mojo experiments/mojo_vpi/__init__.mojo
	mkdir -p $(MOJOTB_BUILD_DIR)
	mojo build $< -I . --emit shared-lib -o $@

$(MOJOTB_BUILD_DIR)/vpi_counter_host: experiments/mojo_vpi/verilator_vpi_host.cpp $(MOJOTB_OBJ_DIR)/Vvpi_counter__ALL.a $(MOJOTB_BUILD_DIR)/libcounter_test.dylib
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
	./$(MOJOTB_BUILD_DIR)/vpi_counter_host $(MOJOTB_BUILD_DIR)/libcounter_test.dylib

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

$(CPPTB_TEST_API_TEST): include/cpptb/coro_runtime.hpp \
		include/cpptb/test_api.hpp include/cpptb/test_result.hpp \
		tests/unit/test_api_test.cpp
	mkdir -p $(CPPTB_BUILD_DIR)
	$(CXX) -std=c++20 -Iinclude -I. \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		tests/unit/test_api_test.cpp -o $@

cpptb-test-api-test: $(CPPTB_TEST_API_TEST)
	$(CPPTB_TEST_API_TEST)

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
		-Wno-MULTIDRIVEN -Wno-TIMESCALEMOD -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND \
		-Ibenchmarks/peripheral_suite/rtl \
		--Mdir $(PERIPHERAL_SUITE_VPI_OBJ_DIR) \
		--top-module vpi_peripheral_suite \
		$(PERIPHERAL_SUITE_VPI_RTL)

$(PERIPHERAL_SUITE_VPI_OBJ_DIR)/Vvpi_peripheral_suite__ALL.a: $(PERIPHERAL_SUITE_VPI_OBJ_DIR)/Vvpi_peripheral_suite.mk
	$(MAKE) -C $(PERIPHERAL_SUITE_VPI_OBJ_DIR) -f Vvpi_peripheral_suite.mk Vvpi_peripheral_suite__ALL.a CXXFLAGS="$(VERILATOR_PLATFORM_CXXFLAGS)"

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
	$(CXX) -std=c++20 \
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
		-Wno-MULTIDRIVEN -Wno-TIMESCALEMOD -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND \
		-Wno-WIDTH -Wno-BLKSEQ -Wno-UNUSEDSIGNAL \
		-Ibenchmarks/peripheral_suite/rtl \
		-CFLAGS "-I$(CURDIR) -I$(CURDIR)/include" \
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
	@if [ ! -f $@ ]; then \
		$(CPPTB_CODEGEN) $(AUTHORING_CORE_DPI_MANIFEST); \
	fi
	@touch $@

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
		$(AUTHORING_CORE_DPI_GENERATED) include/cpptb/coro_runtime.hpp \
		include/cpptb/packed_bits.hpp include/cpptb/fixed.hpp include/cpptb/dpi_runtime.hpp include/cpptb/dpi_static_binding.hpp \
		include/cpptb/test_result.hpp Makefile $(if $(filter timing_phases,$(1)),src/verilator_timing_main.cpp)
	mkdir -p $$(dir $$@)
	verilator $(if $(filter timing_phases,$(1)),--cc --exe --build --vpi,--binary) --timing --no-sched-zero-delay \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-BLKSEQ -Wno-BLKANDNBLK -Wno-UNUSEDSIGNAL \
		-Wno-MULTIDRIVEN \
		-MAKEFLAGS "OPT_FAST=$$(AUTHORING_CORE_OPT_FAST)" \
		-CFLAGS "-I$$(CURDIR) -I$$(CURDIR)/include -DAUTHORING_CORE_KERNEL=$(2) $(if $(filter timing_phases,$(1)),-DCPPTB_VERILATED_TOP=Vdpi_authoring_core -DCPPTB_VERILATOR_DIRECT_TIMING) $$(AUTHORING_CORE_EXTRA_CFLAGS)" \
		$$(if $$(strip $$(AUTHORING_CORE_EXTRA_LDFLAGS)),-LDFLAGS "$$(AUTHORING_CORE_EXTRA_LDFLAGS)",) \
		--Mdir $$(dir $$@) \
		--top-module dpi_authoring_core \
		$(AUTHORING_CORE_RTL) \
		$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/generated/dpi_authoring_core.sv \
		$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/framework/dpi_transport.cpp \
		$(AUTHORING_CORE_DIR)/testbenches/cpp_dpi/testbench.cpp \
		$(if $(filter timing_phases,$(1)),src/verilator_timing_main.cpp)
endef

$(eval $(call AUTHORING_CORE_DPI_template,control,0))
$(eval $(call AUTHORING_CORE_DPI_template,task_value,1))
$(eval $(call AUTHORING_CORE_DPI_template,clock_cycles,2))
$(eval $(call AUTHORING_CORE_DPI_template,timeout,3))
$(eval $(call AUTHORING_CORE_DPI_template,wait_until,4))
$(eval $(call AUTHORING_CORE_DPI_template,event,5))
$(eval $(call AUTHORING_CORE_DPI_template,channel,6))
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

AUTHORING_CORE_DPI_BINARIES := $(foreach kernel,$(AUTHORING_CORE_KERNELS),$(AUTHORING_CORE_BUILD_DIR)/cpp_dpi_$(kernel)/Vdpi_authoring_core)

authoring-core-dpi-build: $(AUTHORING_CORE_DPI_BINARIES)

authoring-core-dpi-run: $(AUTHORING_CORE_BUILD_DIR)/cpp_dpi_$(AUTHORING_CORE_KERNEL)/Vdpi_authoring_core
	$(AUTHORING_CORE_BUILD_DIR)/cpp_dpi_$(AUTHORING_CORE_KERNEL)/Vdpi_authoring_core \
		+AUTHORING_CORE_ITERS=$${AUTHORING_CORE_ITERS:-10000}

$(AUTHORING_CORE_SV_OBJ_DIR)/Vauthoring_core_sv_tb: \
		$(AUTHORING_CORE_RTL) $(AUTHORING_CORE_DIR)/testbenches/systemverilog/authoring_core_sv_tb.sv \
		Makefile
	mkdir -p $(AUTHORING_CORE_SV_OBJ_DIR)
	verilator --binary --timing \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-BLKSEQ -Wno-BLKANDNBLK -Wno-UNUSEDSIGNAL \
		-Wno-MULTIDRIVEN \
		-MAKEFLAGS "OPT_FAST=$(AUTHORING_CORE_OPT_FAST)" \
		$(if $(strip $(AUTHORING_CORE_EXTRA_CFLAGS)),-CFLAGS "$(AUTHORING_CORE_EXTRA_CFLAGS)",) \
		$(if $(strip $(AUTHORING_CORE_EXTRA_LDFLAGS)),-LDFLAGS "$(AUTHORING_CORE_EXTRA_LDFLAGS)",) \
		--Mdir $(AUTHORING_CORE_SV_OBJ_DIR) \
		--top-module authoring_core_sv_tb \
		$(AUTHORING_CORE_RTL) \
		$(AUTHORING_CORE_DIR)/testbenches/systemverilog/authoring_core_sv_tb.sv

authoring-core-sv-build: $(AUTHORING_CORE_SV_OBJ_DIR)/Vauthoring_core_sv_tb

$(AUTHORING_CORE_BUILD_DIR)/force_direct_sv_obj/Vforce_direct_sv_tb: \
		$(AUTHORING_CORE_RTL) $(AUTHORING_CORE_DIR)/testbenches/systemverilog/force_direct_sv_tb.sv Makefile
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
		-Wno-PINMISSING -Wno-TIMESCALEMOD -Wno-WIDTH -Wno-UNUSEDSIGNAL \
		-CFLAGS "-std=c++20 -I$(CURDIR) -I$(CURDIR)/include" \
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

framework-comparison-benchmark: framework-comparison-build
	python3 $(FRAMEWORK_COMPARISON_DIR)/run_benchmark.py --skip-build

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
		-MAKEFLAGS "OPT_FAST=-O3" \
		-CFLAGS "-I$$(CURDIR) -I$$(CURDIR)/include -DHEAVY_WORKLOAD=$(2)" \
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
		-MAKEFLAGS "OPT_FAST=-O3" --Mdir $(dir $@) \
		--top-module heavy_benchmark_sv_tb $(HEAVY_SUITE_RTL) \
		$(HEAVY_SUITE_DIR)/testbenches/systemverilog/heavy_benchmark_sv_tb.sv

framework-comparison-heavy-sv-build: $(HEAVY_SUITE_SV_BINARY)

$(HEAVY_SUITE_VPI_BINARY): $(HEAVY_SUITE_RTL) \
		$(HEAVY_SUITE_DIR)/testbenches/cpp_vpi/heavy_benchmark_vpi_top.sv \
		$(HEAVY_SUITE_DIR)/testbenches/cpp_vpi/heavy_benchmark_vpi_host.cpp \
		include/cpptb/coro_runtime.hpp Makefile
	mkdir -p $(dir $@)
	verilator --cc --exe --build --timing --vpi --public-flat-rw \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-UNUSEDSIGNAL \
		-CFLAGS "-std=c++20 -I$(CURDIR) -I$(CURDIR)/include" \
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
		-MAKEFLAGS "OPT_FAST=-O3" \
		-CFLAGS "-I$$(CURDIR) -I$$(CURDIR)/include -DOPEN_CORE_WORKLOAD=$(2)" \
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
		-MAKEFLAGS "OPT_FAST=-O3" --Mdir $$(dir $$@) \
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
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-UNUSEDSIGNAL -Wno-UNOPTFLAT \
		-DOPEN_CORE_WORKLOAD=$(2) \
		-CFLAGS "-std=c++20 -I$$(CURDIR) -I$$(CURDIR)/include -DOPEN_CORE_WORKLOAD=$(2)" \
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
