ROOT := $(abspath ../../..)
BUILD_DIR := $(ROOT)/build/benchmarks/regmodel_ground_truth/secworks_aes
RTL_DIR := $(ROOT)/benchmarks/framework_comparison/open_cores/third_party/secworks_aes
UV_CACHE_DIR ?= $(ROOT)/build/uv-cache
REPEATS ?= 180
RUNS ?= 6

RTL := \
	$(RTL_DIR)/aes_sbox.v \
	$(RTL_DIR)/aes_inv_sbox.v \
	$(RTL_DIR)/aes_key_mem.v \
	$(RTL_DIR)/aes_encipher_block.v \
	$(RTL_DIR)/aes_decipher_block.v \
	$(RTL_DIR)/aes_core.v \
	$(RTL_DIR)/aes.v
OBSERVER := observers/aes_bus_observer.sv
ORACLE_TB := upstream/tb_aes.v
SV_TB := systemverilog/repeated_aes_tb.sv
RDL := registers/aes.rdl
GENERATED_HEADER := $(BUILD_DIR)/generated/aes_regs.hpp
REGISTER_CODEGEN_SOURCES := \
	$(wildcard $(ROOT)/tools/codegen/cpptb_codegen/*.py) \
	$(ROOT)/pyproject.toml \
	$(ROOT)/uv.lock
ORACLE_BINARY := $(BUILD_DIR)/oracle_obj/Vtb_aes
SV_BINARY := $(BUILD_DIR)/systemverilog_obj/Vsecworks_aes_repeated_tb
CPPTB_BINARY := $(BUILD_DIR)/cpptb/secworks_aes_regmodel/obj/Vdpi_secworks_aes_regmodel
VERILATOR_FLAGS := --binary --timing -Wno-fatal -Wno-TIMESCALEMOD \
	-Wno-WIDTH -Wno-UNUSEDSIGNAL -MAKEFLAGS "OPT_FAST=-O3"

.PHONY: oracle-build cpptb-build sv-build build oracle-test equivalence benchmark

$(GENERATED_HEADER): $(RDL) $(REGISTER_CODEGEN_SOURCES)
	mkdir -p $(dir $@)
	cd $(ROOT) && UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --frozen --extra peakrdl \
		peakrdl cpptb benchmarks/regmodel_ground_truth/secworks_aes/$(RDL) \
		-o $@ --namespace secworks_aes_regs

$(ORACLE_BINARY): $(ORACLE_TB) $(OBSERVER) $(RTL) Makefile
	mkdir -p $(dir $@)
	verilator $(VERILATOR_FLAGS) --top-module tb_aes \
		--Mdir $(dir $@) -o $(notdir $@) $(ORACLE_TB) $(OBSERVER) $(RTL)

$(SV_BINARY): $(SV_TB) $(OBSERVER) $(RTL) Makefile
	mkdir -p $(dir $@)
	verilator $(VERILATOR_FLAGS) --top-module secworks_aes_repeated_tb \
		--Mdir $(dir $@) -o $(notdir $@) $(SV_TB) $(OBSERVER) $(RTL)

oracle-build: $(ORACLE_BINARY)

cpptb-build: $(GENERATED_HEADER)
	cd $(ROOT) && UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --frozen cpptb build \
		--project benchmarks/regmodel_ground_truth/secworks_aes
	test -x $(CPPTB_BINARY)

sv-build: $(SV_BINARY)

build: oracle-build cpptb-build sv-build

oracle-test: oracle-build
	$(ORACLE_BINARY) +AES_BUS_TRACE

equivalence: build
	python3 run_equivalence.py --skip-build

benchmark: cpptb-build sv-build
	python3 run_benchmark.py --skip-build --repeats $(REPEATS) --runs $(RUNS)
