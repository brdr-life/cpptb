# Upstream issue drafts

Ready-to-file drafts for the three findings worth sending upstream, prepared so
filing is a paste, not a research project. Whether and when to file is a
separate decision; nothing here has been submitted.

| Draft | Target | Finding |
| --- | --- | --- |
| [lowrisc-ibex-prim-assert.md](lowrisc-ibex-prim-assert.md) | `lowRISC/ibex` (or `lowRISC/opentitan`, where `prim_assert.sv` is vendored from) | I1, I15 |
| [verilator-randomize-with-item.md](verilator-randomize-with-item.md) | `verilator/verilator` | V1 |
| [verilator-covergroup-transition-ice.md](verilator-covergroup-transition-ice.md) | `verilator/verilator` | V9, V10 |

Each draft is self-contained: title, body, and a repro that runs from a clean
checkout of the target project plus a stock Verilator. Reduced cases live in
`../ports/core_ibex_uvm/shims/` and are quoted inline so the issue does not
depend on this repository being public.
