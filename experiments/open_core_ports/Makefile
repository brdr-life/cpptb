# Self-contained: this directory is not part of `make test`, the benchmark
# registry, the performance guard, or CI. The root Makefile need not know it
# exists; invoke it directly, or add a single `$(MAKE) -C` line if that ever
# becomes convenient.

HERE := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
PYTHON ?= python3

.DEFAULT_GOAL := help
.PHONY: help list fetch tools spike clean distclean

help:
	@printf '%s\n' \
		'open-core port targets:' \
		'  make list       Show pinned upstream sources and whether they are fetched' \
		'  make fetch      Fetch every pinned source into deps/' \
		'  make tools      Unpack the system libraries the upstream flows link' \
		'                  against, without root (Linux/dpkg only)' \
		'  make spike      Build the Spike reference model for co-simulation' \
		'  make clean      Remove build scratch under work/' \
		'  make distclean  Also remove fetched sources under deps/'

list:
	@$(PYTHON) $(HERE)fetch.py --list

fetch:
	@$(PYTHON) $(HERE)fetch.py --all

# Only needed where `sudo apt install` is not available; see local_deps.py.
tools:
	@$(PYTHON) $(HERE)local_deps.py

# Needs fetch first. Takes a few minutes and installs under deps/, not /opt.
spike:
	@$(PYTHON) $(HERE)build_spike.py

clean:
	rm -rf $(HERE)work

# deps/ is disposable: everything in it is reproducible from sources.toml.
distclean: clean
	rm -rf $(HERE)deps
