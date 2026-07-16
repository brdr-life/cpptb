# Optional convenience aliases. The cpptb CLI owns generation and compilation.

REPO_ROOT := $(abspath ../..)
BUILD_ROOT ?= $(REPO_ROOT)/build
UV_CACHE_DIR ?= $(BUILD_ROOT)/uv-cache
CPPTB := UV_CACHE_DIR=$(UV_CACHE_DIR) uv run --project $(REPO_ROOT) --frozen cpptb
PROJECT_ARGS := --project . --build-dir $(BUILD_ROOT)
TEST ?= counter_reset_defaults

.DEFAULT_GOAL := build
.PHONY: build list test run-all run rebuild cpp-dpi-counter-build

build cpp-dpi-counter-build:
	$(CPPTB) build $(PROJECT_ARGS)

list:
	$(CPPTB) list $(PROJECT_ARGS)

test run-all:
	$(CPPTB) test $(PROJECT_ARGS)

run:
	$(CPPTB) test $(PROJECT_ARGS) $(TEST)

rebuild:
	$(CPPTB) build $(PROJECT_ARGS) --rebuild
