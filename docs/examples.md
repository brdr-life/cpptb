# Examples

Every example here is runnable, and each ships with a pure-SystemVerilog twin
that runs the same stimulus, checks, and primary-clock cycle count — so you can
read the two side by side. The pages show the user-facing C++ that constructs
each bench; generated bindings, DPI transport, and result-reporting glue stay
out of the way.

If you have not run cpptb yet, start with
[Getting started](getting-started.md), which walks through the counter example
end to end.

## Simple examples

[**Simple examples**](examples/simple.md) introduce one idea at a time: a
single-clock DUT, clockless timing, a first driver and scoreboard, independent
clock domains, and bounded operations. Start here when you are choosing a shape
for your first testbench.

## Advanced examples

[**Advanced examples**](examples/advanced.md) combine reusable components,
protocol and register models, generated artifacts, richer data types, and
production-scale open-source RTL. Go here once you know what you are building
and want a working precedent to copy.

---

Every example keeps its authored files separate from generated ones;
[Project layout and build ownership](running-tests.md#project-layout-and-build-ownership)
describes the directory structure they share. The
[Core ideas](core-ideas.md) page explains the model these examples share, and
[Tasks and concurrency](testbench-authoring.md) is the reference for the
primitives they compose.
