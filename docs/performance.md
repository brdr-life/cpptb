# Performance

Performance comparisons use an equivalent C++ DPI and pure SystemVerilog
testbench for every authoring feature. The peripheral suite additionally keeps
cocotb and C++ VPI implementations for four-mode comparisons.

The hard framework guard rejects a final C++ DPI to SystemVerilog process-wall
ratio above `1.10`. Measurements run serially, alternate pair order, record
load and resource evidence, and do not normalize measured samples.

```sh
make feature-benchmark FEATURE=signal_edge
make feature-regression
```

Shared CI is used for semantic and equivalence checks only. Reference
performance reports belong in `benchmarks/baselines/`; raw journals and
machine-specific command captures are local artifacts.

Historical scheduler experiments and their accepted/rejected rationale are in
[`benchmarks/authoring_core/OPTIMIZATION_NOTES.md`](../benchmarks/authoring_core/OPTIMIZATION_NOTES.md).
