# Reference baselines

This directory contains compact, reviewed performance summaries suitable for
source control. Raw benchmark JSON, JSONL journals, command output, resource
samples, and machine-specific paths are intentionally excluded.

A baseline is evidence from one controlled environment, not a portable timing
promise. Baselines below predate the build policy in
[`docs/performance.md`](../../docs/performance.md) and cannot be compared
against runs made after it. Before that policy the four modes were not built
alike. The peripheral suite set no optimization at all, so both of its halves
fell back to Verilator's `-Os`; the C++ VPI and cocotb modes were left at that
default across every suite while pure SystemVerilog and C++ DPI were built
`-O3`; and the peripheral suite's C++ VPI host, which is linked outside
Verilator, was compiled with no optimization flag whatsoever. Ratios recorded
against those builds overstate the cost of whichever mode was built less
optimized. Re-measuring the peripheral suite under the current policy moved
C++ VPI from `7.81x` to `1.90x` against pure SystemVerilog. The enforced contract remains the benchmark runner's final C++ DPI to
pure SystemVerilog ratio of at most `1.10` for every unwaived feature. A waiver
must remain visible in the registry and baseline, preserve the raw diagnostic,
and enforce its own ceiling.

After a complete accepted regression, summarize the environment and final
ratios in a dated Markdown and JSON pair. Do not copy absolute paths, hostnames,
power output, raw command logs, or individual timing samples into the compact
record.

- [`2026-07-13-macos-arm64.md`](2026-07-13-macos-arm64.md): development
  baseline before repository publication cleanup.
- [`2026-07-13-publication-validation.md`](2026-07-13-publication-validation.md):
  clean public-layout regression with all 29 registry entries.
