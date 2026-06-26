BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
MOJOTB_BUILD_DIR := $(BUILD_DIR)/mojotb
MOJOTB_OBJ_DIR := $(MOJOTB_BUILD_DIR)/obj
VERILATOR_ROOT := $(shell verilator --getenv VERILATOR_ROOT)
SDKROOT := $(shell xcrun --show-sdk-path)
LIBCXX_INC := $(SDKROOT)/usr/include/c++/v1
CXX ?= clang++

.PHONY: all run vpi-run clean

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

clean:
	rm -rf $(BUILD_DIR)
