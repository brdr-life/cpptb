# Findings

Every defect and surprise this exercise turned up, in one list. Each entry says
what it is, where the problematic code is, and which feature or tool it was hit
through — because most of these were found by using a tool for something its
authors had not needed it for, and that context is the difference between a
useful report and a puzzle.

Nothing here blocked the ports. Every one has a workaround in use, which is
exactly why they are easy to lose track of.

Longer write-ups live in each port's README; [open-issues.md](open-issues.md)
carries the ones needing a decision. Paths are relative to this directory.

---

## Verilator

Eleven findings. Six are randomization or constraint-solver defects with reduced
cases; three are internal faults; two are unsupported constructs that matter
because silence is the failure mode.

| # | Finding | Reduced case / evidence | Hit through |
| --- | --- | --- | --- |
| V1 | A `randomize() with` constraint naming a variable `item` is **silently dropped**. The name, not the scope: renaming to `itm` fixes it. `item` is the implicit `with` iterator. | [`ports/core_ibex_uvm/shims/verilator_constraint_item_name.sv`](ports/core_ibex_uvm/shims/verilator_constraint_item_name.sv) — fix in [`verilator-item-fix.patch`](ports/core_ibex_uvm/verilator-item-fix.patch) | `randomize() with` in `ibex_mem_intf_response_seq` |
| V2 | A class-scope `dist` breaks `randomize(x)` on a member of that class. | [`ports/core_ibex_uvm/shims/verilator_dist_on_state_var.sv`](ports/core_ibex_uvm/shims/verilator_dist_on_state_var.sv) | `dist` in `core_ibex_new_seq_lib.sv` |
| V3 | `force` is not substituted inside an **array index**. | [`ports/core_ibex_uvm/shims/verilator_force_array_index.sv`](ports/core_ibex_uvm/shims/verilator_force_array_index.sv) | UVM `uvm_hdl_force` through VPI |
| V4 | A `dist` and an equality on the same variable are **unsatisfiable** together. | [`ports/ibex_icache_uvm/shims/verilator_dist_plus_equality.sv`](ports/ibex_icache_uvm/shims/verilator_dist_plus_equality.sv) | icache stimulus constraints |
| V5 | `std::randomize` **ignores `dist` weights**. | [`ports/ibex_icache_uvm/shims/verilator_dist_std_randomize.sv`](ports/ibex_icache_uvm/shims/verilator_dist_std_randomize.sv) | icache sequence `std::randomize` |
| V6 | A `solve...before` **disables every soft constraint** in the class. | [`ports/ibex_icache_uvm/shims/verilator_soft_with_solve_before.sv`](ports/ibex_icache_uvm/shims/verilator_soft_with_solve_before.sv) | `ibex_icache_core_base_seq` |
| V7 | A clocking-block pulse is **lost unless it starts on an edge**. | [`ports/ibex_icache_uvm/shims/verilator_clocking_pulse.sv`](ports/ibex_icache_uvm/shims/verilator_clocking_pulse.sv) | icache `clk_rst_if` driving |
| V8 | Constrained `randomize()` over `inside {[1:20]}` is **not uniform** — about half of every draw lands in `[16:20]`, where a quarter belongs. Mean 13.2 against 10.5. | [`ports/ibex_icache_cpptb/shims/verilator_inside_range_uniformity.sv`](ports/ibex_icache_cpptb/shims/verilator_inside_range_uniformity.sv) | Comparing the cpptb port's stimulus against the UVM baseline's |
| V9 | **Internal error** on a transition bin over enum items: `AstNode is not of expected type, but instead has type 'ENUMITEMREF'`. | `deps/ibex/dv/uvm/core_ibex/fcov/core_ibex_fcov_if.sv:589` — `bins out_of_reset = (RESET => BOOT_SET);` | `build_tb.py --fcov`, functional coverage |
| V10 | A **second internal fault**, with no source location, after V9 is worked around by casting each enum item to `ctrl_fsm_e`. | `FCOV_TRANSITION` in [`ports/core_ibex_uvm/build_tb.py`](ports/core_ibex_uvm/build_tb.py) applies the cast; the fault follows | `build_tb.py --fcov`, functional coverage |
| V11 | An assertion cannot reach into an **instance from the generate block that holds it**: `Can't find definition of 'tag_bank'`. | `deps/ibex/rtl/ibex_top.sv:701,707` — `tag_bank.key_valid_i`, instance at `:615`; dropped by `ASSERT_OVERLAYS` so the other 130 can compile | Compiling the RTL's assertions, SVA |

### Not defects, but material

| Finding | Evidence | Hit through |
| --- | --- | --- |
| **456 covergroup constructs are silently discarded** with `COVERIGN` warnings: 208 `intersect`, 131 `&&`, 71 explicit cross bins, 21 `iff`-in-cross, 9 `with`, 8 `\|\|`, 2 `binsof`, 2 `default sequence`, 2 bin-array sizes. A cross whose select is dropped keeps *every* combination, so the bin set is larger than the source describes and the result reads as coverage without being it. | `build/compile_tb_opentitan-fcov.log` | Functional coverage |
| **Verilator 5.050 runs 130 of Ibex's 132 assertions.** The two exceptions are V11. Across the whole directed testlist — 944 entries, ~15M cycles — **no assertion fires**, and the outcomes match the baseline entry for entry, 912 of 944 with zero differences. So the checking is free of false positives, and **it is now on by default here** -- `--no-assertions` restores upstream's behaviour. | Runs `assert-all944` and `default-with-assertions`; `core_ibex_tb_top`'s own `$assertoff` calls resolve real assertion names | SVA |

Every reduced case ships in this repository, in the `shims/` directories the
table links. Fix branches for a subset are staged for upstream submission;
existing verilator/verilator issues referenced above: #7676, #7963, #8010,
#8024.

---

## Ibex and lowRISC DV

| # | Finding | Where | Hit through |
| --- | --- | --- | --- |
| I1 | `prim_assert.sv` sends the Verilator branch to `prim_assert_dummy_macros.svh`, where **every assertion macro expands to nothing**. All 132 assertions compile away, upstream and here. Verilator 5.050 runs 130 of them across the full testlist with no failure and no change in outcome, so the guard costs 130 live properties and buys nothing. **The clearest upstream contribution in this list.** | `deps/ibex/vendor/lowrisc_ip/ip/prim/rtl/prim_assert.sv:102` | Trying to enable assertions |
| I2 | `clk_rst_if.sv` wraps its UVM includes in `` `ifndef VERILATOR ``, so the shared DV library **excludes itself** and fails at the first `DV_CHECK_FATAL`. | `deps/ibex/vendor/lowrisc_ip/dv/sv/common_ifs/clk_rst_if.sv` | Building the UVM testbench on Verilator |
| I3 | `logic o_rst_n;` has **no initialiser**. On four-state X→0 gives an edge; on two-state 0→0 gives none, so the asynchronous reset never fires, the core boots in User mode, and the first CSR access of every program traps. | `deps/ibex/vendor/lowrisc_ip/dv/sv/common_ifs/clk_rst_if.sv` | Every directed test failing identically |
| I4 | `wait (vif.<cb>.reset === 1'b0)` **fires at time 0**: a clocking-block input has no sampled value before its first clocking event. Ten sites. | `RESET_WAIT` in [`ports/core_ibex_uvm/build_tb.py`](ports/core_ibex_uvm/build_tb.py) lists them | UVM agents starting before reset |
| I5 | **LRM violation**: `csr_rd_sub` and `mem_rd_sub` write an automatic task's `output` argument after a timing control, which IEEE 1800 13.2.2 forbids. | `deps/ibex/vendor/lowrisc_ip/dv/sv/csr_utils/csr_utils_pkg.sv` | Elaboration |
| I6 | `ibex_mem_intf_response_agent_cfg` declares `rand bit zero_delays` with a 50/50 `dist`, and **nothing ever randomizes that object** — so delays are always drawn and the zero-delay half of the intended distribution never runs. | `deps/ibex/dv/uvm/core_ibex/common/ibex_mem_intf_agent/ibex_mem_intf_response_agent_cfg.sv` | Porting the agent and matching its stimulus |
| I7 | `illegal_bins illegal_transitions = default sequence;` — implemented per IEEE 1800 19.5.2 it catches `DECODE => DECODE`, so it fires **15,080,448 times in 14,987,388 sampled cycles**. Upstream's own comment says "VCS does not implement default sequence so illegal_bins will be empty", which is the only reason it reads as harmless. | `deps/ibex/dv/uvm/core_ibex/fcov/core_ibex_fcov_if.sv:607,614`, with upstream's TODO on the line above each | Implementing the covergroup in cpptb |
| I8 | Two **ePMP test bugs**, both introduced by commit `a7c5d5d`; 26 of 744 entries self-check-fail because of them. | `ports/core_ibex_uvm/README.md` §"The directed tests" | Running the directed testlist |
| I9 | `tb_cs_registers` **drives zero transactions and reports `TEST PASSED`** on Verilator 5.050. | [`ports/ibex_cs_registers/README.md`](ports/ibex_cs_registers/README.md) | Porting it, and building a control |
| I10 | Upstream's CSR reference model **does not implement MML write suppression**, so it expects a PMP write the RTL correctly refuses. *Filed.* | [`ports/ibex_cs_registers/README.md`](ports/ibex_cs_registers/README.md) | cpptb port stopping at transaction 1,119 |
| I11 | Two `prim` cores **do not declare what they use**. | [open-issues.md](open-issues.md) §19 | fusesoc dependency resolution |
| I12 | `ibex_icache_caching` checks the caching ratio on only about **40% of seeds**. | [open-issues.md](open-issues.md) §20 | icache equivalence comparison |
| I13 | Builds write **into the source tree**: `dv/uvm/core_ibex/link.log`, `examples/sw/benchmarks/coremark/coremark.map`, and ten `build-*/` directories under `deps/ibex`. Untracked, so nothing is corrupted, but outputs land where inputs live. | `deps/ibex/` | `git status` in the vendored checkout |
| I14 | The 944 directed tests **never raise an interrupt, enter debug mode or sleep**, leaving 26 coverage bins unreachable — named bin by bin by `coverage.py`. | `coverage.py build/directed/<run>` in [`ports/core_ibex_cpptb`](ports/core_ibex_cpptb/) | Merging functional coverage over the regression |
| I15 | **Side-effecting code inside an assertion macro.** `prim_lfsr` randomizes its default seed inside `` `ASSERT_I(...) ``, so on any tool that compiles assertions out the randomization silently does not happen and the seed keeps its initial value — measured as `0x0`, a dead LFSR, rather than the intended `DefaultSeed`. The `` `ifdef VERILATOR `` guard above it is what stops that being visible today, which makes the two guards coupled: remove either alone and the behaviour is wrong. | `deps/ibex/vendor/lowrisc_ip/ip/prim/rtl/prim_lfsr.sv:251` | Testing whether the LFSR guard was obsolete |

---

## Audit: every tool guard, and whether its reason still holds

`prim_assert.sv` turned out to hide 130 working assertions behind a guard that
predated the capability, which raises the obvious question of how many more
there are. This is the exhaustive answer, not a sample: every
`` `ifdef``/`` `ifndef`` naming a tool across the Ibex tree, the lowRISC DV
library and our own ports.

Fourteen sites in tracked upstream source. Five are in files the core_ibex build
compiles:

Each was tested rather than reasoned about, and two of the verdicts came out the
opposite of the obvious guess.

| Site | Works around | Still true on 5.050? | How it was tested |
| --- | --- | --- | --- |
| `prim_assert.sv:102` | concurrent assertions | **No — removed, and now the default.** 130 of 132 run, none fires over 944 entries | Full testlist twice; 912 pass and zero entries differ from the pre-assertion baseline |
| `clk_rst_if.sv:22` | UVM under Verilator | **No** — already overlaid | The whole UVM environment runs |
| `pins_if.sv:87` | drive strengths (`1'bz`, pull) | **No**, but unreachable | Simulated: strong beats weak, pull-up holds when released, all four cases correct |
| `ibex_register_file_latch.sv:161` | `$fatal "Latch-based register file not supported for Verilator simulation"` | **No**, but unreachable | Simulated: wrote x1–x5, read them back, x0 reads zero and stays zero after a write |
| `dv_fcov_macros.svh:14` | covergroups | **Yes.** Two internal faults and 456 discarded constructs — V9, V10 | `build_tb.py --fcov` |
| `prim_lfsr.sv:251` | randomizing the LFSR default seed | **Yes — and load-bearing.** See below | Simulated with `SIMULATION` defined and the guard disabled |

The remaining nine are in files this build does not compile: `prim_pad_attr`,
`prim_pad_wrapper` (×2), `prim_usb_diff_rx` — all drive strengths, all still
needed; `prim_double_lfsr` and `prim_lfsr` — see below; `rst_shadowed_if:16` —
the same UVM-exclusion pattern as `clk_rst_if` and so probably as obsolete,
untested because nothing here compiles it; and `tb_cs_registers.sv:51` and
`ibex_simple_system.sv:131`, which exclude a SystemVerilog clock generator
because under Verilator the C++ harness drives the clock. Those last two are
**correct as written** — worth saying, because they look like the same pattern
and are not.

Three notes on the ones that need qualifying:

- **The latch register file.** The `$fatal` is stale, but no Ibex configuration
  selects `RegFileLatch` — every entry in `ibex_configs.yaml` is `RegFileFF` —
  so it is unexercised either way, and clean elaboration is not the same as
  correct simulation. The claim here is only that the blanket "not supported"
  no longer holds at elaboration.
- **`prim_lfsr:251` is load-bearing, and why is a finding of its own — I15.**
  Removing it makes things worse, not better. The non-Verilator branch
  randomizes the seed inside an assertion macro:

  ```systemverilog
  `ASSERT_I(DefaultSeedLocalRandomizeCheck_A, std::randomize(DefaultSeedLocal) with {
                                              !(DefaultSeedLocal inside {'0, '1});})
  ```

  With `prim_assert.sv`'s dummy macros — Verilator's default — `ASSERT_I`
  expands to nothing, so the randomization never runs and `DefaultSeedLocal`
  keeps its initial `0`. Measured: seed `0x0`, and the LFSR never advances,
  where the guard as shipped gives `DefaultSeed`. With the standard macros the
  same code randomizes correctly. So the two guards are coupled, and removing
  this one alone converts a fixed seed into a dead one.

  It is also moot for this build — the block sits behind `` `ifdef SIMULATION ``
  and nothing here defines it — but the coupling is the point.
- **`dv_vif_wrap.sv:58`,** the only `VCS`/`XCELIUM` guard, is benign: the branch
  Verilator takes is identical to the Xcelium one.

`SYNTHESIS` (14 sites) and `FPV_ON` (14) are not simulator workarounds and are
out of scope. **Our own ports carry no tool guards** in tracked source.

So the answer to "how much more of this is there": **four guards are stale, and
only one of them is reachable.** The assertions are now on by default here. The
DV library excluding itself was already overlaid. Drive strengths and the latch
register file both work, and neither is reachable — nothing instantiates
`pins_if`, and no configuration in `ibex_configs.yaml` selects `RegFileLatch` —
so removing those two guards would change nothing and the overlays are not
worth their weight. `prim_lfsr`'s guard turned out to be load-bearing. The rest
are still true, moot, or correct by design.

Two of these came out opposite to the obvious guess. Drive strengths were
written off as unsupported on received wisdom and turned out to work; the LFSR
guard looked stale and turned out to be the only thing preventing a dead
LFSR.

---

## riscv-dv and pyflow

About eighteen findings, written up in
[`ports/core_ibex_uvm/README.md`](ports/core_ibex_uvm/README.md) §"What pyflow
cannot honour" and the sections after it. The ones that change results:

| # | Finding | Where | Hit through |
| --- | --- | --- | --- |
| R1 | `riscv_rand_instr_test` generates **200 instructions, not 10,000** as its testlist entry asks. | README §"riscv_rand_instr_test generates 200 instructions" | Building the riscv-dv testlist at the sizes it specifies |
| R2 | A pyflow generation error **hangs forever** instead of failing. | README §"a pyflow generation error hangs" | Running the testlist unattended |
| R3 | Python's `and`/`or` are **not pyvsc operators**, so constraints written with them silently mean something else. | README §"Python's `and` and `or` are not pyvsc operators" | Generator constraints |
| R4 | The debug ROM stub is a **`dret`, not a self-loop**. | README §"The debug ROM stub is a `dret`" | Debug-mode programs |
| R5 | `+boot_mode=u` **leaves the program in machine mode**. | README §"+boot_mode=u" | Privilege-mode entries |
| R6 | Twenty directed tests wait for a **handshake they never get**. | README §"The reason every test timed out" | Every entry timing out |
| R7 | **28 of 57** riscv-dv entries are unbuildable faithfully; only 1 of the 30 that pass is faithful. | README §"Where that leaves it" | Comparing riscv-dv coverage of the two harnesses |

---

## Spike

| # | Finding | Where | Hit through |
| --- | --- | --- | --- |
| S1 | The pinned Spike **never registers `CSR_MENVCFGH`**. | [open-issues.md](open-issues.md) §15 | Co-simulation mismatch |

---

## cpptb

Ours, and therefore fixable here.

| # | Finding | Where | Hit through |
| --- | --- | --- | --- |
| C1 | **Fixed.** The `CPPTB_HIERARCHY_DISCOVERY` pass compiled and **ran** the testbench, so a testbench that hangs or exits at time zero broke the build in a way that read as a compile failure. The build now extracts the access set from compile-only object sections (`cpptb_access`) and never executes the testbench; clocks are registered at runtime by `start_clock` and driven by generated per-signal SV tasks. | [open-issues.md](open-issues.md) §5; fixed in `tools/codegen/cpptb_codegen/build.py` | Every port's first build |
| C2 | Rebuilds **do not track sources added through `verilator_args`**. | [open-issues.md](open-issues.md) §6 | Editing a `.cc` listed there |
| C3 | `.svh` is **rejected in the source list**. | [open-issues.md](open-issues.md) §7 | Wrapper includes |
| C4 | **Fixed.** `ReadWrite`/`ReadOnly`/`NextTimeStep` needed a timing backend the default build did not link. Every cpptb-build project now links a timing backend unconditionally — `verilator-direct` by default, `vpi` via `timing_backend = "vpi"` or `--timing-backend` — so phase awaits work out of the box under both. | [open-issues.md](open-issues.md) §8; fixed in `tools/codegen/cpptb_codegen/build.py`, `project.py` | Porting a testbench that samples in a phase |
| C5 | Adding or removing a coroutine **changes the stimulus**, because it changes the order the response sequences draw their delays. Not a defect, but it voids any A/B timing that gates a coroutine rather than its body — it produced a wrong +18.7% before the fixed-work check caught it. | `fcov_sampler` in [`ports/core_ibex_cpptb/testbench.cpp`](ports/core_ibex_cpptb/testbench.cpp) | Measuring what coverage sampling costs |
| C6 | cpptb has **no coverage `default` bin** and no type reflection for auto-binned coverpoints, so both are declared explicitly in the port. | [`ports/core_ibex_cpptb/fcov.hpp`](ports/core_ibex_cpptb/fcov.hpp) | Porting `uarch_cg` |
| C8 | **Fixed.** Generated headers were not clang-clean at Ibex scale, two ways: DUT parameters wider than 64 bits were emitted as `int64_t` decimal literals — a ~157-bit `StatePerm`, 13 sites — which clang hard-errors on and GCC quietly truncates; and the generated dispatch if-chains were deep enough to segfault clang 18's parser (`ParseIfStatement` recursion). Wide parameters now emit as little-endian word arrays and the hierarchy dispatch is a sorted-table binary search over flat `if constexpr` arms; `CXX=clang++` builds the Ibex port clean (6.4 s for the lookup TU) with zero truncation warnings under GCC. | Fixed in `tools/codegen/cpptb_codegen/generate_dpi_bindings.py`; verified with clang 18.1.8 on `ports/core_ibex_cpptb` | Cross-compiler soak of the access-set parity gate |
| C7 | `with (expr)` in a cross select is **matched at bin granularity**, not value granularity. Exact whenever the expression is constant across each bin, which holds for every use in Ibex; `where()` records the SystemVerilog it stands for. | `include/cpptb/coverage.hpp` | Porting cross filters |

---

## Ours, found and fixed

Worth listing because each was a measurement that would have been wrong.

| Finding | Where |
| --- | --- |
| The ePMP linker-script overlay was rewritten under a lock `ld` read outside, corrupting about one entry in several hundred. | `ports/core_ibex_uvm/run_directed.py` |
| A shared `build/directed/<test>/sim.log` between two runs made records not match their logs, producing two stale passes and an inflated total (914, not 912). | `ports/core_ibex_uvm/run_directed.py` |
| `run_tests.py --compare` matched a log line **by line number**, silently reporting the baseline's `insns/item` as zero after an overlay moved it. | `ports/ibex_icache_uvm/run_tests.py` |
| Our own overlay left `back_line` with no `new_seed` constraint, so its scoreboard ended holding 919 seeds and accepted a fetch matching any of them. | `ports/ibex_icache_uvm/build_tb.py` |
| A blocked A/B timing measurement charged a load trend to the delta and reported +15.8%; interleaving gives a different answer. | `docs`, and the method note in `ports/core_ibex_cpptb/RESULTS.md` |
| **The headline speedup compared two different testbenches.** The reported 62x set this port's own decomposition against the whole UVM environment: the port was missing `irq_agent`, which the UVM env builds for every test and whose monitor samples five pins every cycle, and it fused four processes per bus where UVM has separate components and a per-transaction object. Measured like for like -- `IBEX_ARCH=faithful` against `build_tb.py --no-irq-agent` -- it is **48.8x**, so about 21% of the ratio was the difference between the testbenches rather than between the frameworks. | `ports/core_ibex_cpptb/RESULTS.md` §"What that ratio is actually comparing" |
| Two more timing measurements were void before they were right. Gating coverage by not spawning the sampler changed the stimulus and reported +18.7% for what is about +4.7%; and the two architectures do not draw their delays in the same order, so their runs are compared as a rate rather than as fixed work. Both were caught by checking cycle counts rather than trusting them. | C5, and `ports/core_ibex_cpptb/RESULTS.md` |
| An intermediate result read as a new Verilator constraint-solver bug -- `std::randomize` with an `inside` exclusion returning all-zeros -- and was **z3 missing from `PATH`**, the same failure mode as three earlier times. The reproducer that printed `solver failed` is what caught it. | The trap is documented in `ports/core_ibex_uvm/run_directed.py`'s `run_one` |
