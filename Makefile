BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
MOJOTB_BUILD_DIR := $(BUILD_DIR)/mojotb
MOJOTB_OBJ_DIR := $(MOJOTB_BUILD_DIR)/obj
CPPTB_BUILD_DIR := $(BUILD_DIR)/cpptb
CPPTB_OBJ_DIR := $(CPPTB_BUILD_DIR)/obj
CPPTB_CORO_RUNTIME_TEST := $(CPPTB_BUILD_DIR)/coro_runtime_test
CPPTB_PACKED_VALUE_TEST := $(CPPTB_BUILD_DIR)/packed_value_test
CPPTB_APB_EVENT_OBJ_DIR := $(CPPTB_BUILD_DIR)/apb_event_obj
CPPTB_MULTICLOCK_DIR := cpptb/examples/dpi_multiclock
CPPTB_MULTICLOCK_OBJ_DIR := $(CPPTB_BUILD_DIR)/dpi_multiclock_obj
CPPTB_MULTICLOCK_SV_OBJ_DIR := $(CPPTB_BUILD_DIR)/dpi_multiclock_sv_obj
CPPTB_MULTICLOCK_SV_TB := $(CPPTB_MULTICLOCK_DIR)/pure_sv/dual_clock_mailbox_sv_tb.sv
CPPTB_MULTICLOCK_MANIFEST := $(CPPTB_MULTICLOCK_DIR)/dual_clock_mailbox.dpi.json
CPPTB_MULTICLOCK_GENERATED := \
	$(CPPTB_MULTICLOCK_DIR)/generated/dual_clock_mailbox_dut.hpp \
	$(CPPTB_MULTICLOCK_DIR)/generated/dual_clock_mailbox_binding.hpp \
	$(CPPTB_MULTICLOCK_DIR)/generated/dpi_dual_clock_mailbox.sv
CPPTB_MULTICLOCK_CODEGEN_STAMP := $(CPPTB_BUILD_DIR)/dpi_multiclock.codegen.stamp
CPPTB_CONFORMANCE_DIR := cpptb/conformance
CPPTB_CONFORMANCE_MANIFEST := $(CPPTB_CONFORMANCE_DIR)/scheduler_conformance.dpi.json
CPPTB_CONFORMANCE_RUNNER := $(CPPTB_CONFORMANCE_DIR)/run_conformance.py
CPPTB_CONFORMANCE_BINARY := $(CPPTB_BUILD_DIR)/conformance_obj/Vdpi_scheduler_conformance
CPPTB_BENCH_BUILD_DIR := $(BUILD_DIR)/benchmarks/cocotb_cpp_compare
PERIPHERAL_SUITE_BUILD_DIR := $(BUILD_DIR)/benchmarks/peripheral_suite
PERIPHERAL_SUITE_VPI_OBJ_DIR := $(PERIPHERAL_SUITE_BUILD_DIR)/cpp_vpi_obj
PERIPHERAL_SUITE_SV_OBJ_DIR := $(PERIPHERAL_SUITE_BUILD_DIR)/pure_sv_obj
PERIPHERAL_SUITE_DPI_OBJ_DIR := $(PERIPHERAL_SUITE_BUILD_DIR)/cpp_dpi_obj
PERIPHERAL_SUITE_RUNTIME_OLD_ROOT := benchmarks/diagnostics/runtime_old
PERIPHERAL_SUITE_RUNTIME_OLD_OBJ_DIR := $(BUILD_DIR)/diagnostics/runtime_old_obj
PERIPHERAL_SUITE_DPI_MANIFEST := benchmarks/peripheral_suite/cpp_dpi/peripheral_suite.dpi.json
PERIPHERAL_SUITE_DPI_GENERATOR := cpptb/codegen/generate_dpi_bindings.py
AUTHORING_CORE_DIR := benchmarks/authoring_core
AUTHORING_CORE_BUILD_DIR := $(BUILD_DIR)/benchmarks/authoring_core
AUTHORING_CORE_SV_OBJ_DIR := $(AUTHORING_CORE_BUILD_DIR)/pure_sv_obj
AUTHORING_CORE_DPI_MANIFEST := $(AUTHORING_CORE_DIR)/cpp_dpi/authoring_core.dpi.json
AUTHORING_CORE_DPI_GENERATOR := cpptb/codegen/generate_dpi_bindings.py
AUTHORING_CORE_DPI_CODEGEN_STAMP := $(AUTHORING_CORE_BUILD_DIR)/cpp_dpi.codegen.stamp
AUTHORING_CORE_DPI_GENERATED := \
	$(AUTHORING_CORE_DIR)/cpp_dpi/generated/authoring_core_dut.hpp \
	$(AUTHORING_CORE_DIR)/cpp_dpi/generated/authoring_core_binding.hpp \
	$(AUTHORING_CORE_DIR)/cpp_dpi/generated/dpi_authoring_core.sv
AUTHORING_CORE_RTL := $(AUTHORING_CORE_DIR)/rtl/authoring_core_dut.sv
AUTHORING_CORE_CPP := \
	$(AUTHORING_CORE_DIR)/cpp_dpi/framework/authoring_core.hpp \
	$(AUTHORING_CORE_DIR)/cpp_dpi/framework/dpi_transport.cpp \
	$(AUTHORING_CORE_DIR)/cpp_dpi/testbench.cpp
AUTHORING_CORE_KERNELS := control task_value clock_cycles timeout task_timeout wait_until event channel all wide64 wide_echo_137 wide_slice fixed_mac array_index array_wide mem_rw hier_probe mem_backdoor mem_probe_read mem_probe_deposit mem_probe_read_deposit signal_edge array_multidim force_release packed_view
AUTHORING_CORE_KERNEL ?= control
AUTHORING_CORE_OPT_FAST ?= -O3
AUTHORING_CORE_EXTRA_CFLAGS ?=
FEATURE ?=
FEATURE_REGRESSION_RUNNER := python3 benchmarks/run_regression.py
UV_CACHE_DIR ?= $(BUILD_DIR)/uv-cache
CODEGEN_PYTHON := UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --frozen python
CPPTB_CODEGEN_SOURCES := \
	$(PERIPHERAL_SUITE_DPI_GENERATOR) \
	cpptb/codegen/design_ir.py \
	cpptb/codegen/frontends/__init__.py \
	cpptb/codegen/frontends/slang.py \
	cpptb/codegen/frontends/verilator_json.py \
	pyproject.toml uv.lock
PERIPHERAL_SUITE_DPI_GENERATED := \
	benchmarks/peripheral_suite/cpp_dpi/generated/peripheral_suite_dut.hpp \
	benchmarks/peripheral_suite/cpp_dpi/generated/peripheral_suite_binding.hpp \
	benchmarks/peripheral_suite/cpp_dpi/generated/dpi_peripheral_suite.sv
PERIPHERAL_SUITE_DPI_CODEGEN_STAMP := $(PERIPHERAL_SUITE_BUILD_DIR)/cpp_dpi.codegen.stamp
CPPTB_APB_EVENT_RTL := \
	cpptb/rggen_apb_event/rtl/apb_event_service_unit_regs_core_pkg.sv \
	cpptb/rggen_apb_event/rtl/apb_event_service_unit_regs_core.sv \
	cpptb/rggen_apb_event/rtl/apb_event_sleep_unit_regs_core_pkg.sv \
	cpptb/rggen_apb_event/rtl/apb_event_sleep_unit_regs_core.sv \
	cpptb/rggen_apb_event/rtl/apb_event_unit_peakrdl.sv \
	cpptb/rggen_apb_event/rtl/vpi_apb_event_unit.sv
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
	benchmarks/peripheral_suite/cpp_vpi/vpi_peripheral_suite.sv
PERIPHERAL_SUITE_SV_RTL := \
	$(PERIPHERAL_SUITE_CORE_RTL) \
	benchmarks/peripheral_suite/pure_sv/peripheral_suite_sv_tb.sv
PERIPHERAL_SUITE_DPI_RTL := \
	$(PERIPHERAL_SUITE_CORE_RTL) \
	benchmarks/peripheral_suite/cpp_dpi/generated/dpi_peripheral_suite.sv
VERILATOR_ROOT := $(shell verilator --getenv VERILATOR_ROOT)
SDKROOT := $(shell xcrun --show-sdk-path)
LIBCXX_INC := $(SDKROOT)/usr/include/c++/v1
CXX ?= clang++

.PHONY: all run vpi-run cpp-vpi-run cpp-coro-runtime-test cpp-apb-event-run cpp-apb-event-bench-build cpp-apb-event-bench-run cpptb-codegen-test cpptb-codegen-frontend-check cpptb-conformance-codegen cpptb-conformance-codegen-check cpptb-conformance-frontend-check cpptb-conformance-build cpptb-conformance-run cpp-dpi-multiclock-codegen cpp-dpi-multiclock-codegen-check cpp-dpi-multiclock-build cpp-dpi-multiclock-run cpp-dpi-multiclock-sv-build cpp-dpi-multiclock-sv-run peripheral-suite-build peripheral-suite-run peripheral-suite-sv-build peripheral-suite-sv-run peripheral-suite-dpi-codegen peripheral-suite-dpi-codegen-check peripheral-suite-dpi-build peripheral-suite-dpi-run peripheral-suite-runtime-old-diagnostic-build authoring-core-dpi-codegen authoring-core-dpi-codegen-check authoring-core-dpi-build authoring-core-dpi-run authoring-core-sv-build authoring-core-sv-run authoring-core-build authoring-core-benchmark feature-list feature-test feature-benchmark feature-regression registry-check clean

all: $(BUILD_DIR)/counter_driver

$(OBJ_DIR)/Vcounter.mk: examples/counter.sv
	mkdir -p $(OBJ_DIR)
	verilator --cc --Mdir $(OBJ_DIR) $<

$(OBJ_DIR)/Vcounter__ALL.a: $(OBJ_DIR)/Vcounter.mk
	$(MAKE) -C $(OBJ_DIR) -f Vcounter.mk Vcounter__ALL.a CXXFLAGS=-I$(LIBCXX_INC)

$(BUILD_DIR)/libcounter.dylib: examples/counter_c_api.cpp $(OBJ_DIR)/Vcounter__ALL.a
	$(CXX) -dynamiclib -std=c++17 \
		-I$(OBJ_DIR) \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		-I$(LIBCXX_INC) \
		examples/counter_c_api.cpp \
		$(OBJ_DIR)/Vcounter__ALL.a \
		$(VERILATOR_ROOT)/include/verilated.cpp \
		$(VERILATOR_ROOT)/include/verilated_threads.cpp \
		-o $@

$(BUILD_DIR)/counter_driver: examples/counter_driver.mojo $(BUILD_DIR)/libcounter.dylib
	mojo build $< -o $@

run: all
	./$(BUILD_DIR)/counter_driver

$(MOJOTB_OBJ_DIR)/Vvpi_counter.mk: mojotb/examples/vpi_counter.sv
	mkdir -p $(MOJOTB_OBJ_DIR)
	verilator --cc --vpi --public-flat-rw --Mdir $(MOJOTB_OBJ_DIR) $<

$(MOJOTB_OBJ_DIR)/Vvpi_counter__ALL.a: $(MOJOTB_OBJ_DIR)/Vvpi_counter.mk
	$(MAKE) -C $(MOJOTB_OBJ_DIR) -f Vvpi_counter.mk Vvpi_counter__ALL.a CXXFLAGS=-I$(LIBCXX_INC)

$(MOJOTB_BUILD_DIR)/libcounter_test.dylib: mojotb/tests/counter_test.mojo mojotb/runtime.mojo mojotb/__init__.mojo
	mkdir -p $(MOJOTB_BUILD_DIR)
	mojo build $< -I . --emit shared-lib -o $@

$(MOJOTB_BUILD_DIR)/vpi_counter_host: mojotb/verilator_vpi_host.cpp $(MOJOTB_OBJ_DIR)/Vvpi_counter__ALL.a $(MOJOTB_BUILD_DIR)/libcounter_test.dylib
	$(CXX) -std=c++17 \
		-I$(MOJOTB_OBJ_DIR) \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		-I$(LIBCXX_INC) \
		mojotb/verilator_vpi_host.cpp \
		$(MOJOTB_OBJ_DIR)/Vvpi_counter__ALL.a \
		$(VERILATOR_ROOT)/include/verilated.cpp \
		$(VERILATOR_ROOT)/include/verilated_threads.cpp \
		$(VERILATOR_ROOT)/include/verilated_vpi.cpp \
		-o $@

vpi-run: $(MOJOTB_BUILD_DIR)/vpi_counter_host
	./$(MOJOTB_BUILD_DIR)/vpi_counter_host $(MOJOTB_BUILD_DIR)/libcounter_test.dylib

$(CPPTB_OBJ_DIR)/Vvpi_counter.mk: cpptb/examples/vpi_counter.sv
	mkdir -p $(CPPTB_OBJ_DIR)
	verilator --cc --vpi --public-flat-rw --Mdir $(CPPTB_OBJ_DIR) $<

$(CPPTB_OBJ_DIR)/Vvpi_counter__ALL.a: $(CPPTB_OBJ_DIR)/Vvpi_counter.mk
	$(MAKE) -C $(CPPTB_OBJ_DIR) -f Vvpi_counter.mk Vvpi_counter__ALL.a CXXFLAGS=-I$(LIBCXX_INC)

$(CPPTB_BUILD_DIR)/vpi_counter_host: cpptb/verilator_vpi_host.cpp cpptb/runtime.hpp cpptb/tests/counter_test.cpp cpptb/tests/counter_test.hpp $(CPPTB_OBJ_DIR)/Vvpi_counter__ALL.a
	$(CXX) -std=c++17 \
		-I. \
		-I$(CPPTB_OBJ_DIR) \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		-I$(LIBCXX_INC) \
		cpptb/verilator_vpi_host.cpp \
		cpptb/tests/counter_test.cpp \
		$(CPPTB_OBJ_DIR)/Vvpi_counter__ALL.a \
		$(VERILATOR_ROOT)/include/verilated.cpp \
		$(VERILATOR_ROOT)/include/verilated_threads.cpp \
		$(VERILATOR_ROOT)/include/verilated_vpi.cpp \
		-o $@

cpp-vpi-run: $(CPPTB_BUILD_DIR)/vpi_counter_host
	./$(CPPTB_BUILD_DIR)/vpi_counter_host

$(CPPTB_CORO_RUNTIME_TEST): cpptb/coro_runtime.hpp cpptb/packed_bits.hpp cpptb/probe.hpp \
		cpptb/tests/coro_runtime_test.cpp
	mkdir -p $(CPPTB_BUILD_DIR)
	$(CXX) -std=c++20 -I. \
		-DCPPTB_CORO_FRAME_POOL_DIAGNOSTICS \
		-DCPPTB_CORO_WAIT_PATH_DIAGNOSTICS \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		cpptb/tests/coro_runtime_test.cpp -o $@

cpp-coro-runtime-test: $(CPPTB_CORO_RUNTIME_TEST)
	$(CPPTB_CORO_RUNTIME_TEST)

$(CPPTB_PACKED_VALUE_TEST): cpptb/packed_bits.hpp cpptb/fixed.hpp \
		cpptb/tests/packed_value_test.cpp
	mkdir -p $(CPPTB_BUILD_DIR)
	$(CXX) -std=c++20 -O3 -I. cpptb/tests/packed_value_test.cpp -o $@

cpptb-packed-value-test: $(CPPTB_PACKED_VALUE_TEST)
	$(CPPTB_PACKED_VALUE_TEST)

$(CPPTB_APB_EVENT_OBJ_DIR)/Vvpi_apb_event_unit.mk: $(CPPTB_APB_EVENT_RTL)
	mkdir -p $(CPPTB_APB_EVENT_OBJ_DIR)
	verilator --cc --vpi --public-flat-rw -Wno-MULTIDRIVEN \
		--Mdir $(CPPTB_APB_EVENT_OBJ_DIR) \
		--top-module vpi_apb_event_unit \
		$(CPPTB_APB_EVENT_RTL)

$(CPPTB_APB_EVENT_OBJ_DIR)/Vvpi_apb_event_unit__ALL.a: $(CPPTB_APB_EVENT_OBJ_DIR)/Vvpi_apb_event_unit.mk
	$(MAKE) -C $(CPPTB_APB_EVENT_OBJ_DIR) -f Vvpi_apb_event_unit.mk Vvpi_apb_event_unit__ALL.a CXXFLAGS=-I$(LIBCXX_INC)

$(CPPTB_BUILD_DIR)/apb_event_host: cpptb/coro_runtime.hpp cpptb/packed_bits.hpp \
		cpptb/rggen_apb_event/apb_event_dut.hpp \
		cpptb/rggen_apb_event/verilator_host.cpp \
		cpptb/rggen_apb_event/tests/apb_event_test.cpp \
		cpptb/rggen_apb_event/tests/apb_event_test.hpp \
		$(CPPTB_APB_EVENT_OBJ_DIR)/Vvpi_apb_event_unit__ALL.a
	$(CXX) -std=c++20 \
		-I. \
		-I$(CPPTB_APB_EVENT_OBJ_DIR) \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		-I$(LIBCXX_INC) \
		cpptb/rggen_apb_event/verilator_host.cpp \
		cpptb/rggen_apb_event/tests/apb_event_test.cpp \
		$(CPPTB_APB_EVENT_OBJ_DIR)/Vvpi_apb_event_unit__ALL.a \
		$(VERILATOR_ROOT)/include/verilated.cpp \
		$(VERILATOR_ROOT)/include/verilated_threads.cpp \
		$(VERILATOR_ROOT)/include/verilated_vpi.cpp \
		-o $@

cpp-apb-event-run: $(CPPTB_BUILD_DIR)/apb_event_host
	./$(CPPTB_BUILD_DIR)/apb_event_host

$(CPPTB_BENCH_BUILD_DIR)/apb_event_bench_host: cpptb/coro_runtime.hpp cpptb/packed_bits.hpp \
		cpptb/rggen_apb_event/apb_event_dut.hpp \
		benchmarks/cocotb_cpp_compare/cpptb/apb_event_bench.cpp \
		benchmarks/cocotb_cpp_compare/cpptb/apb_event_bench.hpp \
		benchmarks/cocotb_cpp_compare/cpptb/verilator_bench_host.cpp \
		$(CPPTB_APB_EVENT_OBJ_DIR)/Vvpi_apb_event_unit__ALL.a
	mkdir -p $(CPPTB_BENCH_BUILD_DIR)
	$(CXX) -std=c++20 \
		-I. \
		-I$(CPPTB_APB_EVENT_OBJ_DIR) \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		-I$(LIBCXX_INC) \
		benchmarks/cocotb_cpp_compare/cpptb/verilator_bench_host.cpp \
		benchmarks/cocotb_cpp_compare/cpptb/apb_event_bench.cpp \
		$(CPPTB_APB_EVENT_OBJ_DIR)/Vvpi_apb_event_unit__ALL.a \
		$(VERILATOR_ROOT)/include/verilated.cpp \
		$(VERILATOR_ROOT)/include/verilated_threads.cpp \
		$(VERILATOR_ROOT)/include/verilated_vpi.cpp \
		-o $@

cpp-apb-event-bench-build: $(CPPTB_BENCH_BUILD_DIR)/apb_event_bench_host

cpp-apb-event-bench-run: $(CPPTB_BENCH_BUILD_DIR)/apb_event_bench_host
	./$(CPPTB_BENCH_BUILD_DIR)/apb_event_bench_host

$(CPPTB_MULTICLOCK_CODEGEN_STAMP): $(CPPTB_CODEGEN_SOURCES) \
		$(CPPTB_MULTICLOCK_MANIFEST) $(CPPTB_MULTICLOCK_DIR)/dual_clock_mailbox.sv
	mkdir -p $(dir $@)
	$(CODEGEN_PYTHON) $(PERIPHERAL_SUITE_DPI_GENERATOR) $(CPPTB_MULTICLOCK_MANIFEST)
	touch $@

$(CPPTB_MULTICLOCK_GENERATED): $(CPPTB_MULTICLOCK_CODEGEN_STAMP)
	@if [ ! -f $@ ]; then \
		$(CODEGEN_PYTHON) $(PERIPHERAL_SUITE_DPI_GENERATOR) $(CPPTB_MULTICLOCK_MANIFEST); \
	fi
	@touch $@

cpp-dpi-multiclock-codegen: $(CPPTB_MULTICLOCK_GENERATED)

cpp-dpi-multiclock-codegen-check:
	$(CODEGEN_PYTHON) $(PERIPHERAL_SUITE_DPI_GENERATOR) $(CPPTB_MULTICLOCK_MANIFEST) --check

cpptb-codegen-test:
	$(CODEGEN_PYTHON) -m unittest discover -s cpptb/codegen/tests

cpptb-codegen-frontend-check:
	$(CODEGEN_PYTHON) $(PERIPHERAL_SUITE_DPI_GENERATOR) \
		$(AUTHORING_CORE_DPI_MANIFEST) --check --compare-frontend verilator_json
	$(CODEGEN_PYTHON) $(PERIPHERAL_SUITE_DPI_GENERATOR) \
		$(CPPTB_MULTICLOCK_MANIFEST) --check --compare-frontend verilator_json
	$(CODEGEN_PYTHON) $(PERIPHERAL_SUITE_DPI_GENERATOR) \
		$(PERIPHERAL_SUITE_DPI_MANIFEST) --check --compare-frontend verilator_json
	$(CODEGEN_PYTHON) $(PERIPHERAL_SUITE_DPI_GENERATOR) \
		$(CPPTB_CONFORMANCE_MANIFEST) --check --compare-frontend verilator_json

cpptb-conformance-codegen:
	$(CODEGEN_PYTHON) $(PERIPHERAL_SUITE_DPI_GENERATOR) $(CPPTB_CONFORMANCE_MANIFEST)

cpptb-conformance-codegen-check:
	$(CODEGEN_PYTHON) $(PERIPHERAL_SUITE_DPI_GENERATOR) \
		$(CPPTB_CONFORMANCE_MANIFEST) --check

cpptb-conformance-frontend-check:
	$(CODEGEN_PYTHON) $(PERIPHERAL_SUITE_DPI_GENERATOR) \
		$(CPPTB_CONFORMANCE_MANIFEST) --check --compare-frontend verilator_json

$(CPPTB_CONFORMANCE_BINARY): $(CPPTB_CODEGEN_SOURCES) \
		$(CPPTB_CONFORMANCE_MANIFEST) $(CPPTB_CONFORMANCE_RUNNER) \
		$(CPPTB_CONFORMANCE_DIR)/scheduler_conformance.sv \
		$(CPPTB_CONFORMANCE_DIR)/framework.hpp \
		$(CPPTB_CONFORMANCE_DIR)/framework.cpp \
		$(CPPTB_CONFORMANCE_DIR)/dpi_transport.cpp \
		$(CPPTB_CONFORMANCE_DIR)/testbench.cpp \
		cpptb/coro_runtime.hpp cpptb/packed_bits.hpp cpptb/probe.hpp cpptb/dpi_runtime.hpp \
		cpptb/test_result.hpp
	$(CODEGEN_PYTHON) $(CPPTB_CONFORMANCE_RUNNER) --build-only
	touch $@

cpptb-conformance-build: $(CPPTB_CONFORMANCE_BINARY)

cpptb-conformance-run: $(CPPTB_CONFORMANCE_BINARY)
	$(CODEGEN_PYTHON) $(CPPTB_CONFORMANCE_RUNNER) --no-build

$(CPPTB_MULTICLOCK_OBJ_DIR)/Vdpi_dual_clock_mailbox: \
		$(CPPTB_MULTICLOCK_DIR)/dual_clock_mailbox.sv \
		$(CPPTB_MULTICLOCK_DIR)/framework.cpp \
		$(CPPTB_MULTICLOCK_DIR)/framework.hpp \
		$(CPPTB_MULTICLOCK_DIR)/testbench.cpp \
		$(CPPTB_MULTICLOCK_DIR)/dpi_transport.cpp \
		$(CPPTB_MULTICLOCK_GENERATED) cpptb/coro_runtime.hpp cpptb/packed_bits.hpp \
		cpptb/dpi_runtime.hpp cpptb/test_result.hpp
	mkdir -p $(CPPTB_MULTICLOCK_OBJ_DIR)
	verilator --binary --timing --no-sched-zero-delay \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-UNUSEDSIGNAL \
		-CFLAGS -I$(CURDIR) \
		--Mdir $(CPPTB_MULTICLOCK_OBJ_DIR) \
		--top-module dpi_dual_clock_mailbox \
		$(CPPTB_MULTICLOCK_DIR)/dual_clock_mailbox.sv \
		$(CPPTB_MULTICLOCK_DIR)/generated/dpi_dual_clock_mailbox.sv \
		$(CPPTB_MULTICLOCK_DIR)/dpi_transport.cpp \
		$(CPPTB_MULTICLOCK_DIR)/framework.cpp \
		$(CPPTB_MULTICLOCK_DIR)/testbench.cpp

cpp-dpi-multiclock-build: $(CPPTB_MULTICLOCK_OBJ_DIR)/Vdpi_dual_clock_mailbox

cpp-dpi-multiclock-run: $(CPPTB_MULTICLOCK_OBJ_DIR)/Vdpi_dual_clock_mailbox
	$(CPPTB_MULTICLOCK_OBJ_DIR)/Vdpi_dual_clock_mailbox \
		+CPPTB_MULTICLOCK_ITERS=$${CPPTB_MULTICLOCK_ITERS:-16}

$(CPPTB_MULTICLOCK_SV_OBJ_DIR)/Vdual_clock_mailbox_sv_tb: \
		$(CPPTB_MULTICLOCK_DIR)/dual_clock_mailbox.sv \
		$(CPPTB_MULTICLOCK_SV_TB)
	mkdir -p $(CPPTB_MULTICLOCK_SV_OBJ_DIR)
	verilator --binary --timing --no-sched-zero-delay \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-UNUSEDSIGNAL \
		--Mdir $(CPPTB_MULTICLOCK_SV_OBJ_DIR) \
		--top-module dual_clock_mailbox_sv_tb \
		$(CPPTB_MULTICLOCK_DIR)/dual_clock_mailbox.sv \
		$(CPPTB_MULTICLOCK_SV_TB)

cpp-dpi-multiclock-sv-build: $(CPPTB_MULTICLOCK_SV_OBJ_DIR)/Vdual_clock_mailbox_sv_tb

cpp-dpi-multiclock-sv-run: $(CPPTB_MULTICLOCK_SV_OBJ_DIR)/Vdual_clock_mailbox_sv_tb
	$(CPPTB_MULTICLOCK_SV_OBJ_DIR)/Vdual_clock_mailbox_sv_tb \
		+CPPTB_MULTICLOCK_ITERS=$${CPPTB_MULTICLOCK_ITERS:-16}

$(PERIPHERAL_SUITE_VPI_OBJ_DIR)/Vvpi_peripheral_suite.mk: $(PERIPHERAL_SUITE_VPI_RTL)
	mkdir -p $(PERIPHERAL_SUITE_VPI_OBJ_DIR)
	verilator --cc --vpi --public-flat-rw --no-timing \
		-Wno-MULTIDRIVEN -Wno-TIMESCALEMOD -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND \
		-Ibenchmarks/peripheral_suite/rtl \
		--Mdir $(PERIPHERAL_SUITE_VPI_OBJ_DIR) \
		--top-module vpi_peripheral_suite \
		$(PERIPHERAL_SUITE_VPI_RTL)

$(PERIPHERAL_SUITE_VPI_OBJ_DIR)/Vvpi_peripheral_suite__ALL.a: $(PERIPHERAL_SUITE_VPI_OBJ_DIR)/Vvpi_peripheral_suite.mk
	$(MAKE) -C $(PERIPHERAL_SUITE_VPI_OBJ_DIR) -f Vvpi_peripheral_suite.mk Vvpi_peripheral_suite__ALL.a CXXFLAGS=-I$(LIBCXX_INC)

$(PERIPHERAL_SUITE_BUILD_DIR)/peripheral_suite_host: cpptb/coro_runtime.hpp cpptb/packed_bits.hpp \
		benchmarks/peripheral_suite/cpp_vpi/framework/peripheral_suite.hpp \
		benchmarks/peripheral_suite/cpp_vpi/framework/peripheral_suite_bench.cpp \
		benchmarks/peripheral_suite/cpp_vpi/framework/peripheral_suite_bench.hpp \
		benchmarks/peripheral_suite/cpp_vpi/framework/peripheral_suite_dut.hpp \
		benchmarks/peripheral_suite/cpp_vpi/framework/peripheral_suite_fixture.cpp \
		benchmarks/peripheral_suite/cpp_vpi/framework/peripheral_suite_fixture.hpp \
		benchmarks/peripheral_suite/cpp_vpi/framework/verilator_suite_host.cpp \
		benchmarks/peripheral_suite/cpp_vpi/testbench.cpp \
		$(PERIPHERAL_SUITE_VPI_OBJ_DIR)/Vvpi_peripheral_suite__ALL.a
	mkdir -p $(PERIPHERAL_SUITE_BUILD_DIR)
	$(CXX) -std=c++20 \
		-I. \
		-I$(PERIPHERAL_SUITE_VPI_OBJ_DIR) \
		-I$(VERILATOR_ROOT)/include \
		-I$(VERILATOR_ROOT)/include/vltstd \
		-I$(LIBCXX_INC) \
		benchmarks/peripheral_suite/cpp_vpi/framework/verilator_suite_host.cpp \
		benchmarks/peripheral_suite/cpp_vpi/framework/peripheral_suite_bench.cpp \
		benchmarks/peripheral_suite/cpp_vpi/framework/peripheral_suite_fixture.cpp \
		benchmarks/peripheral_suite/cpp_vpi/testbench.cpp \
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
	$(CODEGEN_PYTHON) $(PERIPHERAL_SUITE_DPI_GENERATOR) $(PERIPHERAL_SUITE_DPI_MANIFEST)
	touch $@

$(PERIPHERAL_SUITE_DPI_GENERATED): $(PERIPHERAL_SUITE_DPI_CODEGEN_STAMP)
	@if [ ! -f $@ ]; then \
		$(CODEGEN_PYTHON) $(PERIPHERAL_SUITE_DPI_GENERATOR) $(PERIPHERAL_SUITE_DPI_MANIFEST); \
	fi
	@touch $@

peripheral-suite-dpi-codegen: $(PERIPHERAL_SUITE_DPI_GENERATED)

peripheral-suite-dpi-codegen-check:
	$(CODEGEN_PYTHON) $(PERIPHERAL_SUITE_DPI_GENERATOR) $(PERIPHERAL_SUITE_DPI_MANIFEST) --check

$(PERIPHERAL_SUITE_DPI_OBJ_DIR)/Vdpi_peripheral_suite: $(PERIPHERAL_SUITE_DPI_RTL) \
		benchmarks/peripheral_suite/cpp_dpi/framework/dpi_transport.cpp \
		benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite.hpp \
		benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_bench.cpp \
		benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_bench.hpp \
		benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_fixture.cpp \
		benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_fixture.hpp \
		$(PERIPHERAL_SUITE_DPI_GENERATED) \
		benchmarks/peripheral_suite/cpp_dpi/testbench.cpp \
		cpptb/coro_runtime.hpp cpptb/packed_bits.hpp cpptb/dpi_runtime.hpp \
		cpptb/test_result.hpp
	mkdir -p $(PERIPHERAL_SUITE_DPI_OBJ_DIR)
	verilator --binary --timing \
		--no-sched-zero-delay \
		-Wno-MULTIDRIVEN -Wno-TIMESCALEMOD -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND \
		-Wno-WIDTH -Wno-BLKSEQ -Wno-UNUSEDSIGNAL \
		-Ibenchmarks/peripheral_suite/rtl \
		-CFLAGS -I$(CURDIR) \
		--Mdir $(PERIPHERAL_SUITE_DPI_OBJ_DIR) \
		--top-module dpi_peripheral_suite \
		$(PERIPHERAL_SUITE_DPI_RTL) \
		benchmarks/peripheral_suite/cpp_dpi/framework/dpi_transport.cpp \
		benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_bench.cpp \
		benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_fixture.cpp \
		benchmarks/peripheral_suite/cpp_dpi/testbench.cpp

peripheral-suite-dpi-build: $(PERIPHERAL_SUITE_DPI_OBJ_DIR)/Vdpi_peripheral_suite

peripheral-suite-dpi-run: $(PERIPHERAL_SUITE_DPI_OBJ_DIR)/Vdpi_peripheral_suite
	$(PERIPHERAL_SUITE_DPI_OBJ_DIR)/Vdpi_peripheral_suite +PERIPHERAL_SUITE_ITERS=$${PERIPHERAL_SUITE_ITERS:-1000}

$(PERIPHERAL_SUITE_RUNTIME_OLD_OBJ_DIR)/Vdpi_peripheral_suite: $(PERIPHERAL_SUITE_CORE_RTL) \
		benchmarks/peripheral_suite/cpp_dpi/framework/dpi_transport.cpp \
		benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_bench.cpp \
		benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_bench.hpp \
		$(PERIPHERAL_SUITE_RUNTIME_OLD_ROOT)/benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite.hpp \
		$(PERIPHERAL_SUITE_RUNTIME_OLD_ROOT)/benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_fixture.cpp \
		$(PERIPHERAL_SUITE_RUNTIME_OLD_ROOT)/benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_fixture.hpp \
		$(PERIPHERAL_SUITE_RUNTIME_OLD_ROOT)/benchmarks/peripheral_suite/cpp_dpi/testbench.cpp \
		$(PERIPHERAL_SUITE_RUNTIME_OLD_ROOT)/cpptb/coro_runtime.hpp \
		cpptb/dpi_runtime.hpp cpptb/test_result.hpp
	@test -f benchmarks/peripheral_suite/cpp_dpi/generated/dpi_peripheral_suite.sv
	@test -f benchmarks/peripheral_suite/cpp_dpi/generated/peripheral_suite_dut.hpp
	@test -f benchmarks/peripheral_suite/cpp_dpi/generated/peripheral_suite_binding.hpp
	mkdir -p $(PERIPHERAL_SUITE_RUNTIME_OLD_OBJ_DIR)
	verilator --binary --timing \
		--no-sched-zero-delay \
		-Wno-MULTIDRIVEN -Wno-TIMESCALEMOD -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND \
		-Wno-WIDTH -Wno-BLKSEQ -Wno-UNUSEDSIGNAL \
		-Ibenchmarks/peripheral_suite/rtl \
		-CFLAGS -I$(CURDIR)/$(PERIPHERAL_SUITE_RUNTIME_OLD_ROOT) \
		-CFLAGS -I$(CURDIR) \
		--Mdir $(PERIPHERAL_SUITE_RUNTIME_OLD_OBJ_DIR) \
		--top-module dpi_peripheral_suite \
		$(PERIPHERAL_SUITE_DPI_RTL) \
		benchmarks/peripheral_suite/cpp_dpi/framework/dpi_transport.cpp \
		benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_bench.cpp \
		$(PERIPHERAL_SUITE_RUNTIME_OLD_ROOT)/benchmarks/peripheral_suite/cpp_dpi/framework/peripheral_suite_fixture.cpp \
		$(PERIPHERAL_SUITE_RUNTIME_OLD_ROOT)/benchmarks/peripheral_suite/cpp_dpi/testbench.cpp

peripheral-suite-runtime-old-diagnostic-build: $(PERIPHERAL_SUITE_RUNTIME_OLD_OBJ_DIR)/Vdpi_peripheral_suite

$(AUTHORING_CORE_DPI_CODEGEN_STAMP): $(CPPTB_CODEGEN_SOURCES) \
		$(AUTHORING_CORE_DPI_MANIFEST) $(AUTHORING_CORE_RTL)
	mkdir -p $(dir $@)
	$(CODEGEN_PYTHON) $(AUTHORING_CORE_DPI_GENERATOR) $(AUTHORING_CORE_DPI_MANIFEST)
	touch $@

$(AUTHORING_CORE_DPI_GENERATED): $(AUTHORING_CORE_DPI_CODEGEN_STAMP)
	@if [ ! -f $@ ]; then \
		$(CODEGEN_PYTHON) $(AUTHORING_CORE_DPI_GENERATOR) $(AUTHORING_CORE_DPI_MANIFEST); \
	fi
	@touch $@

authoring-core-dpi-codegen: $(AUTHORING_CORE_DPI_GENERATED)

authoring-core-dpi-codegen-check:
	$(CODEGEN_PYTHON) $(AUTHORING_CORE_DPI_GENERATOR) $(AUTHORING_CORE_DPI_MANIFEST) --check

define AUTHORING_CORE_DPI_template
$(AUTHORING_CORE_BUILD_DIR)/cpp_dpi_$(1)/Vdpi_authoring_core: \
		$(AUTHORING_CORE_RTL) $(AUTHORING_CORE_CPP) \
		$(AUTHORING_CORE_DPI_GENERATED) cpptb/coro_runtime.hpp \
		cpptb/packed_bits.hpp cpptb/fixed.hpp cpptb/dpi_runtime.hpp \
		cpptb/test_result.hpp Makefile
	mkdir -p $$(dir $$@)
	verilator --binary --timing --no-sched-zero-delay \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-BLKSEQ -Wno-BLKANDNBLK -Wno-UNUSEDSIGNAL \
		-Wno-MULTIDRIVEN \
		-MAKEFLAGS "OPT_FAST=$$(AUTHORING_CORE_OPT_FAST)" \
		-CFLAGS "-I$$(CURDIR) -DAUTHORING_CORE_KERNEL=$(2) $$(AUTHORING_CORE_EXTRA_CFLAGS)" \
		--Mdir $$(dir $$@) \
		--top-module dpi_authoring_core \
		$(AUTHORING_CORE_RTL) \
		$(AUTHORING_CORE_DIR)/cpp_dpi/generated/dpi_authoring_core.sv \
		$(AUTHORING_CORE_DIR)/cpp_dpi/framework/dpi_transport.cpp \
		$(AUTHORING_CORE_DIR)/cpp_dpi/testbench.cpp
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

AUTHORING_CORE_DPI_BINARIES := $(foreach kernel,$(AUTHORING_CORE_KERNELS),$(AUTHORING_CORE_BUILD_DIR)/cpp_dpi_$(kernel)/Vdpi_authoring_core)

authoring-core-dpi-build: $(AUTHORING_CORE_DPI_BINARIES)

authoring-core-dpi-run: $(AUTHORING_CORE_BUILD_DIR)/cpp_dpi_$(AUTHORING_CORE_KERNEL)/Vdpi_authoring_core
	$(AUTHORING_CORE_BUILD_DIR)/cpp_dpi_$(AUTHORING_CORE_KERNEL)/Vdpi_authoring_core \
		+AUTHORING_CORE_ITERS=$${AUTHORING_CORE_ITERS:-10000}

$(AUTHORING_CORE_SV_OBJ_DIR)/Vauthoring_core_sv_tb: \
		$(AUTHORING_CORE_RTL) $(AUTHORING_CORE_DIR)/pure_sv/authoring_core_sv_tb.sv \
		Makefile
	mkdir -p $(AUTHORING_CORE_SV_OBJ_DIR)
	verilator --binary --timing \
		-Wno-TIMESCALEMOD -Wno-WIDTH -Wno-BLKSEQ -Wno-BLKANDNBLK -Wno-UNUSEDSIGNAL \
		-Wno-MULTIDRIVEN \
		-MAKEFLAGS "OPT_FAST=$(AUTHORING_CORE_OPT_FAST)" \
		--Mdir $(AUTHORING_CORE_SV_OBJ_DIR) \
		--top-module authoring_core_sv_tb \
		$(AUTHORING_CORE_RTL) \
		$(AUTHORING_CORE_DIR)/pure_sv/authoring_core_sv_tb.sv

authoring-core-sv-build: $(AUTHORING_CORE_SV_OBJ_DIR)/Vauthoring_core_sv_tb

authoring-core-sv-run: $(AUTHORING_CORE_SV_OBJ_DIR)/Vauthoring_core_sv_tb
	$(AUTHORING_CORE_SV_OBJ_DIR)/Vauthoring_core_sv_tb \
		+AUTHORING_CORE_ITERS=$${AUTHORING_CORE_ITERS:-10000} \
		+AUTHORING_CORE_KERNEL=$(AUTHORING_CORE_KERNEL)

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
