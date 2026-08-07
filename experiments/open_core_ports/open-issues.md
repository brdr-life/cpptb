# Open issues from the porting exercise

Things found while porting Ibex's verification flows that are not fixed, with
enough detail to pick up cold. Numbering matches the findings list in
[ibex-coverage.md](ibex-coverage.md).

Nothing here blocks the ports: each has a workaround in use, which is why none
of them stopped the work. That is also why they are easy to forget.

## In cpptb

### 5. The discovery pass runs the testbench — fixed

`cpptb build` used to compile the testbench a second time with
`-DCPPTB_HIERARCHY_DISCOVERY` and execute it to learn which signals are clocks
and which hierarchy paths need a transport. A testbench that returned early
when its environment was empty, which it always is at build time, was
discovered as one that never starts a clock, and every run died at time zero
with `scheduler starvation`.

Fixed: the build now recovers the access set from compile-only object sections
(`cpptb_access`, written by `[[gnu::used, section(...)]]` records) and never
executes the testbench. Clocks are registered at runtime by `start_clock` —
the generated wrapper emits a driver task per writable one-bit signal
(including unpacked-array elements, so interface-member clocks work) and asks
the runtime after `PHASE_INIT` which of them were started. Early returns and
environment-dependent control flow before `start_clock` are now harmless.

### 6. Rebuilds do not track sources added through `verilator_args`

Only `[testbench] sources` and the RTL list are fingerprinted. A `.cc` file
added through `build.verilator_args` is compiled but not tracked, so editing it
and rebuilding is a no-op and the fix appears not to work.

Cost more than the other findings put together, because it makes every other
bug look intermittent.

**Repro:** add a `.cc` to `verilator_args`, edit it, run `cpptb build`, observe
`up to date`.
**Workaround:** `--rebuild`, or put the file in `[testbench] sources`.
**Fix direction:** fingerprint file arguments in `verilator_args`, or reject
them with a message pointing at `[testbench] sources`.

### 7. `.svh` is rejected in the source list

`cpptb: RTL source file has unsupported suffix ... (expected .sv, .v)`.
Defensible, since `.svh` is conventionally an include, but upstream fusesoc
cores do compile `.svh` files directly when they hold DPI import declarations,
as `dv/cosim/cosim_dpi.svh` does.

**Workaround:** `ports/riscv_arch_tests/configure.py` copies it in as `.sv`.
**Fix direction:** accept `.svh` in the source list, or say in the error that
copying is the intended workaround.

### 8. Edge semantics and the missing timing backend — fixed

`co_await RisingEdge` resumes before the design evaluates that edge, so it is
right for sampling and wrong for driving: `set()` is immediate and a value
written there is captured by the edge just awaited. `ReadOnly` and `ReadWrite`
express this directly but could not be selected from a project.

Fixed: every cpptb-build project links a timing backend unconditionally
(`verilator-direct` default, `vpi` via `timing_backend`/`--timing-backend`),
and `deferred_writes = true` gives `set()` cocotb's writes-apply-in-ReadWrite
semantics. The translation recipes live in
[Coming from cocotb](../../docs/coming-from-cocotb.md).

The original analysis is in
[Scheduling](../../docs/scheduling.md#sample-on-the-edge-drive-off-it).

## In Verilator

`ports/ibex_icache_uvm` lists five Verilator randomization defects, each with a
reduced case in its `shims/`. The comparison in `ports/ibex_icache_cpptb` found
a sixth.

### 18. A constrained randomize() over an `inside` range is not uniform

Found by comparing `ports/ibex_icache_cpptb` against `ports/ibex_icache_uvm`.
Verilator's constrained `randomize()` puts about half of every draw over
`inside {[1:20]}` into `[16:20]`, where a quarter belongs, whether the range is
a hard constraint or a soft one. Mean 13.2 against 10.5.

This is a sixth entry in the list `ports/ibex_icache_uvm` keeps, and it is the
whole of the difference between that harness's stimulus and the cpptb port's:
`ibex_icache_core_base_seq` picks its instruction-run length that way, so the
UVM baseline runs about 25% more fetches per transaction than its own
constraints ask for.

**Repro:** `shims/verilator_inside_range_uniformity.sv` in
`ports/ibex_icache_cpptb`, three ways of picking a number in `[1:20]`.
**Workaround:** draw the value with `$urandom_range` and constrain nothing,
which is what `ports/ibex_icache_uvm` already does for every `dist` in that
environment for three other reasons.
**Fix direction:** belongs on `verilator/verilator` rather than on Ibex. Not
filed.

## In upstream, not filed

### 14. `tb_cs_registers` is vacuous on Verilator 5.050

It reports `TEST PASSED` having driven zero transactions, and without `-c` never
terminates. The driver object is constructed, since its `OnFinal` prints, but
`driver_tick`'s lookup in `reg_dpi.cc` never finds it.

The most serious thing found here: a green result that checked nothing.

**Repro:**
```sh
fusesoc --cores-root=. run --target=sim --tool=verilator \
  --build-root=build-csr lowrisc:ibex:tb_cs_registers
./build-csr/lowrisc_ibex_tb_cs_registers_0/sim-verilator/Vtb_cs_registers -c 500000
```
**Not root-caused.** Ibex pins `VERILATOR_VERSION=v4.210` in `ci/vars.env`; this
was seen on 5.050 and has not been confirmed against 4.210. Confirming it
against the pinned version is the first step before filing, since the report
should say whether it is a regression or has never worked on Verilator 5.

### 15. The pinned Spike never registers `CSR_MENVCFGH`

`riscv/processor.cc` on lowRISC's `ibex_cosim` branch adds `CSR_MENVCFG` to
`csrmap` and has no entry for the high half, so on RV32 a write to `menvcfgh`
traps where Ibex correctly implements it as read-only zero. A model with the low
half and not the high half is not a coherent RV32 configuration.

Belongs on `lowRISC/riscv-isa-sim`, not `ibex`.

**Repro:** build the architectural tests without `-DIBEX_NO_U_MODE` and run any
of them under co-simulation; both harnesses fail identically at the first
`csrw menvcfgh` in the suite's boot code.

### 19. Two prim cores do not declare what they use

`lowrisc:prim:ram_1p_adv` declares neither `lowrisc:prim:mubi` nor
`lowrisc:prim:flop`, although `prim_ram_1p_adv.sv` imports `prim_mubi_pkg` and
`prim_ram_1p_scr.sv` instantiates `prim_flop`. A fusesoc graph that reaches
`ram_1p_scr` and nothing else does not close. Every upstream flow that reaches
it also reaches a core that does depend on them, which is why nothing notices.

**Repro:** a CAPI=2 core depending only on `lowrisc:prim:ram_1p_scr`.
**Workaround:** name them, as
`ports/ibex_icache_cpptb/ibex_icache_cpptb.core` does.

### 20. `ibex_icache_caching` checks the caching ratio on about 40% of seeds

The test takes no new memory seed, so the whole run uses seed 0, whose error
range covers the fetch window whenever `base_addr` lands in the wrong part of
the address space. When it does, every fetch errors, the caching window resets
on each one, and the only check the test exists for never runs. Measured over
40 seeds on the cpptb port and reproduced on the baseline at seed 124.

Upstream's `reseed` of 50 hides this; a single seed does not, and a green
single-seed result can mean the check never fired.

**Fix direction:** let the caching sequence take one new seed at the start,
before the cache is enabled, or draw `base_addr` away from the error range.

### 16, 17. riscv-arch-test gaps

Two limitations in the suite for cores like Ibex, both worked around in
`ports/riscv_arch_tests/build_tests.py` with the reasons recorded there:
`Zicsr` tests cannot be built for a core with U-mode but no F, no V and no
`time` CSR, and the PMP tests assume a granularity of 2 or more. Lower value to
file than the two above, since they affect a narrower set of cores.

## Filed

### 13. Upstream's CSR model does not implement MML write suppression

Independently reported in
[lowRISC/ibex#2242](https://github.com/lowRISC/ibex/issues/2242), open since
January 2025. The code-level cause and a suggested fix are
[a comment there](https://github.com/lowRISC/ibex/issues/2242#issuecomment-5084571451).

This is what stops `ports/ibex_cs_registers` at transaction 1,119. If it is
fixed upstream, that port should reach 10,000 and the stopping point in its
README can go.
