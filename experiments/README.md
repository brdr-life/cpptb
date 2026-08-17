# Experiments

Non-shipping investigation work. Nothing under this directory is part of the
installable package, wired into `make test`, or covered by the performance
guard; each subdirectory carries its own README and either graduated into the
main tree, informed a roadmap decision, or records why a direction was not
taken.

- `open_core_ports/` — ports of real third-party verification environments
  (Ibex's UVM testbenches, the RISC-V architectural tests, riscv-dv) with
  both-harness comparisons and Spike co-simulation. The docs site summarizes
  it in "Ports of real testbenches".
- `uvm_comparison/` — a small UVM-on-Verilator viability matrix.
- The remaining directories are historical prototypes (Mojo, raw VPI, C API
  bindings) kept for provenance.
