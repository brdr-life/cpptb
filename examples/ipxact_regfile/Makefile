ROOT := ../..

.PHONY: build run sv-run model-test test

build:
	$(MAKE) -C $(ROOT) cpp-dpi-ipxact-regfile-build

run:
	$(MAKE) -C $(ROOT) cpp-dpi-ipxact-regfile-run

sv-run:
	$(MAKE) -C $(ROOT) cpp-dpi-ipxact-regfile-sv-run

model-test:
	$(MAKE) -C $(ROOT) cpptb-ipxact-regfile-model-test

test: model-test run sv-run
