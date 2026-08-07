# Draft: prim_assert.sv compiles all assertions away under Verilator 5

**Target:** `lowRISC/ibex` (the file is vendored from lowrisc_ip, so the fix
may belong in `lowRISC/opentitan` first; Ibex would pick it up on the next
vendor update).

**Suggested title:** `prim_assert.sv` routes Verilator to the dummy macros,
discarding 130 working assertions

---

## Body

`prim_assert.sv` sends the Verilator branch to `prim_assert_dummy_macros.svh`:

```systemverilog
`ifdef VERILATOR
 `include "prim_assert_dummy_macros.svh"
`elsif SYNTHESIS
 ...
```

so every assertion macro expands to nothing under Verilator. That guard
predates Verilator's assertion support and no longer buys anything: on
Verilator 5.050, routing the `VERILATOR` branch to the real macros instead
compiles 130 of Ibex's 132 assertions cleanly, and the full 944-entry directed
testlist runs twice with zero failures and zero entries changing outcome
relative to the dummy-macro baseline. The guard's only present-day effect is
turning 130 live properties into dead text.

The change itself is two lines — point the `VERILATOR` branch at
`prim_assert_standard_macros.svh` (or simply delete the branch and let
Verilator take the standard path).

### One coupled fix that has to land with it

`prim_lfsr.sv` randomizes its default seed **inside** `` `ASSERT_I ``:

```systemverilog
// prim_lfsr.sv (SecCheck section)
`ASSERT_I(..., std::randomize(...) ...)
```

On any tool that compiles assertions out, that randomization silently does not
happen and the seed keeps its initial value — measured as `0x0`, a dead LFSR,
instead of the intended randomized `DefaultSeed`. Today the surrounding
`` `ifdef VERILATOR `` guard in `prim_lfsr.sv` hides this, which makes the two
guards coupled: enabling real assertion macros without also keeping (or
fixing) the `prim_lfsr` guard changes simulation behaviour, and removing the
`prim_lfsr` guard alone runs side-effecting code or none depending on the
assertion macros. The clean resolution is to move the seed randomization out
of the assertion macro so it happens unconditionally, then drop both guards.

### Evidence

- Verilator 5.050, Ibex at the `ibex_cosim` pin.
- 130 of 132 assertions compile with the standard macros (the other two use
  constructs Verilator still rejects; they can stay individually guarded).
- Full `riscv-dv` directed testlist (944 entries): 912 pass, identical to the
  dummy-macro baseline entry for entry; no assertion fires.
- The `prim_lfsr` dead-seed measurement: with `SIMULATION` defined and the
  LFSR guard disabled under dummy macros, `DefaultSeedLocal` stays `0x0`; with
  the standard macros it randomizes as intended.
