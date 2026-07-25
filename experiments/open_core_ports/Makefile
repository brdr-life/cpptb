# Self-contained: this directory is not part of `make test`, the benchmark
# registry, the performance guard, or CI. The root Makefile need not know it
# exists; invoke it directly, or add a single `$(MAKE) -C` line if that ever
# becomes convenient.

HERE := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
PYTHON ?= python3

.DEFAULT_GOAL := help
.PHONY: help list fetch clean distclean

help:
	@printf '%s\n' \
		'open-core port targets:' \
		'  make list       Show pinned upstream sources and whether they are fetched' \
		'  make fetch      Fetch every pinned source into deps/' \
		'  make clean      Remove build scratch under work/' \
		'  make distclean  Also remove fetched sources under deps/'

list:
	@$(PYTHON) $(HERE)fetch.py --list

fetch:
	@$(PYTHON) $(HERE)fetch.py --all

clean:
	rm -rf $(HERE)work

# deps/ is disposable: everything in it is reproducible from sources.toml.
distclean: clean
	rm -rf $(HERE)deps
