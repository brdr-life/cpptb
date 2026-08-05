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
| V11 | An assertion cannot reach into an **instance from the generate block that holds it**: `Can't find definition of 'tag_bank'`. | `deps/ibex/rtl/ibex_top.sv:701,707` — `tag_bank.key_valid_i`, instance at `:615` | `build_tb.py --assertions`, SVA |

### Not defects, but material

| Finding | Evidence | Hit through |
| --- | --- | --- |
| **456 covergroup constructs are silently discarded** with `COVERIGN` warnings: 208 `intersect`, 131 `&&`, 71 explicit cross bins, 21 `iff`-in-cross, 9 `with`, 8 `\|\|`, 2 `binsof`, 2 `default sequence`, 2 bin-array sizes. A cross whose select is dropped keeps *every* combination, so the bin set is larger than the source describes and the result reads as coverage without being it. | `build/compile_tb_opentitan-fcov.log` | Functional coverage |
| **Verilator 5.050 runs 130 of Ibex's 132 assertions.** The two exceptions are V11. This is a finding *for* Verilator: the guard that excludes them (I1 below) predates the capability. | `build_tb.py --assertions`; `core_ibex_tb_top`'s own `$assertoff` calls resolve real assertion names | SVA |

Branches with reduced cases and fixes live in `~/code/vl-{B,C,K,P,S,T}`.
Upstream issues referenced: #7676, #7963, #8010, #8024.

---

## Ibex and lowRISC DV

| # | Finding | Where | Hit through |
| --- | --- | --- | --- |
| I1 | `prim_assert.sv` sends the Verilator branch to `prim_assert_dummy_macros.svh`, where **every assertion macro expands to nothing**. All 132 assertions compile away, upstream and here. The guard predates Verilator supporting them — see above. | `deps/ibex/vendor/lowrisc_ip/ip/prim/rtl/prim_assert.sv:102` | Trying to enable assertions |
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
| C1 | The `CPPTB_HIERARCHY_DISCOVERY` pass **compiles and runs** the testbench, so a testbench that hangs or exits at time zero breaks the build in a way that reads as a compile failure. | [open-issues.md](open-issues.md) §5 | Every port's first build |
| C2 | Rebuilds **do not track sources added through `verilator_args`**. | [open-issues.md](open-issues.md) §6 | Editing a `.cc` listed there |
| C3 | `.svh` is **rejected in the source list**. | [open-issues.md](open-issues.md) §7 | Wrapper includes |
| C4 | `ReadWrite`/`ReadOnly`/`NextTimeStep` need a timing backend the default build does not link; the roadmap's steps 2–5 are unstarted. | [open-issues.md](open-issues.md) §8, `docs/roadmap.md` | Porting a testbench that samples in a phase |
| C5 | Adding or removing a coroutine **changes the stimulus**, because it changes the order the response sequences draw their delays. Not a defect, but it voids any A/B timing that gates a coroutine rather than its body — it produced a wrong +18.7% before the fixed-work check caught it. | `fcov_sampler` in [`ports/core_ibex_cpptb/testbench.cpp`](ports/core_ibex_cpptb/testbench.cpp) | Measuring what coverage sampling costs |
| C6 | cpptb has **no coverage `default` bin** and no type reflection for auto-binned coverpoints, so both are declared explicitly in the port. | [`ports/core_ibex_cpptb/fcov.hpp`](ports/core_ibex_cpptb/fcov.hpp) | Porting `uarch_cg` |
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
