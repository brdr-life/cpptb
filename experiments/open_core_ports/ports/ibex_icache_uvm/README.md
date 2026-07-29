# Ibex's icache UVM testbench on Verilator 5

`dv/uvm/icache` is Ibex's second UVM testbench and its only block-level one:
the DUT is `ibex_icache` on its own, with a core agent on the fetch side, a
memory agent on the bus side, a push-pull agent supplying the scrambling key,
and a scoreboard that models the cache against every seed the memory has held.
Upstream signs it off with VCS and runs it through OpenTitan's dvsim.
`dv/ibex_icache_sim_cfg.hjson` names `vcs`, and the Makefile beside it defaults
to `xcelium`. Verilator is not one of the options.

**Status: it builds, and all ten tests in upstream's own list pass on
Verilator 5.050.** The last one to get there was
`ibex_icache_stress_all_with_reset`, which failed on twelve sequencer messages
that are `UVM_INFO` on the UVM version upstream runs and `UVM_WARNING` on the
one this port has to use; see "UVM 1.2 against 1800.2". The whole list runs in
about two and a half minutes at `--jobs 4`.

This is a baseline in the same sense as `ports/core_ibex_uvm`: an upstream
verification environment running unmodified stimulus, so that a cpptb port has
something to be compared against.

## Building and running

```sh
python3 ../../fetch.py uvm_core     # Accellera's UVM, pinned in sources.toml
python3 build_tb.py --jobs 4
eval "$(python3 ../../local_deps.py --env)"   # puts z3 on PATH
python3 run_tests.py --jobs 4
```

A build from an empty `build/` is about three minutes at `--jobs 4` and the
binary is 47 MB. An SMT
solver is needed at run time, not build time: Verilator does not solve
constraints itself, it pipes to `z3 --in`, and without one every `randomize()`
in the testbench fails with the real explanation only in a warning at the top
of the run.

`run_tests.py --list` prints the ten tests with the UVM class and virtual
sequence each one uses. `--regression smoke` runs the nine in upstream's smoke
regression. `--reseed N` runs N seeds per test; upstream's `reseed` is 50 and
the default here is one, with a fixed seed.

## Resolving the build description

`dv/uvm/core_ibex` has `.f` file lists. This testbench does not: it is
described by FuseSoC `CAPI=2` `.core` files, with `dv/ibex_icache_sim.core`
(`lowrisc:dv:ibex_icache_sim:0.1`, target `sim`, toplevel `tb`) at the root, and
`dv/ibex_icache_sim_cfg.hjson` naming that core for dvsim to build from.

**`corelist.py` walks the graph in place rather than driving fusesoc.** fusesoc
is not installed here, it is not a standard library module, and every tool in
this directory is standard library only. It also copies the whole tree into a
work directory, which would put a second copy of the sources between the build
and `deps/` and make the overlay discipline harder to see. Walking the files
gives absolute paths into `deps/ibex` and nothing else moves.

What the walker implements is the part of CAPI=2 this graph uses:

* `filesets`, each with `depend`, `files` and a default `file_type`
* per-file attributes, `{is_include_file: true}` and `{file_type: ...}`
* `targets`, each naming filesets, with `tool_x ? (fileset)` conditions
* `virtual:`, by which `lowrisc:prim_generic:ram_1p` provides
  `lowrisc:prim:ram_1p`

Generators, parameters, `filters` and tool sections are not implemented and
are not reached: nothing in Ibex's tree has a `generate:` section at all. A
dependency that cannot be resolved is an error rather than something skipped.
Virtual dependencies resolve to the `prim_generic` provider, which is what
`common_sim_cfg.hjson` asks for with
`sv_flist_gen_flags: ["--mapping=lowrisc:prim_generic:all:0.1"]`.

The YAML reader is written out rather than taken from PyYAML, in the same
spirit as the testlist reader in `ports/core_ibex_uvm/run_directed.py`. It
covers block mappings and sequences, flow mappings as a value, and anchors and
aliases. To keep an unrelated `.core` file from failing the build, the index of
every core in the tree is built with a regex over `name:` and `virtual:`, and
only the files actually walked are parsed in full.

The result is **38 cores, 101 compiled sources, 14 include directories and 5
Verilator control files**, and `build_tb.py --list-sources` prints all of it
with the core each file came from. The command is:

```
verilator --binary --timing --vpi -Wno-fatal --no-skip-identical -j 4
          --top-module tb --timescale 1ns/1ps -o ibex_icache_tb
          +incdir+<overlay> +incdir+<uvm> +incdir+... x14
          +define+UVM +define+UVM_NO_DEPRECATED
          +define+UVM_REG_ADDR_WIDTH=32 +define+UVM_REG_DATA_WIDTH=32
          +define+UVM_REG_BYTENABLE_WIDTH=4 +define+SIMULATION
          +define+DUT_HIER=tb.dut
          <5 .vlt files> <uvm_pkg.sv> <101 sources>
          shims/uvm_dpi_verilator.cc -CFLAGS -I<uvm>/dpi
```

The defines are `common_sim_cfg.hjson`'s, which is what dvsim would apply, with
one exception: `UVM_REGEX_NO_DPI` is in upstream's list and is left out here,
because `shims/uvm_dpi_verilator.cc` compiles UVM's own `uvm_regex.cc` and the
DPI regex is the faster path.

`--vpi` is not optional. UVM's plusarg list comes from `vpi_get_vlog_info`, so
without it `uvm_cmdline_processor` sees no arguments and `+UVM_TESTNAME` never
reaches `run_test()`. `shims/uvm_dpi_verilator.cc` is a copy of the same file
in `ports/core_ibex_uvm`: it compiles UVM's vendor-neutral DPI and supplies the
plain-VPI HDL backend that `uvm_hdl.c` refuses to pick. Nothing in this
testbench uses UVM's HDL access, so no `verilator.vlt` of public signals is
needed here.

`--no-skip-identical` is there for the reason `ports/core_ibex_uvm` documents:
Verilator's skip-identical check notices neither a source moving into
`build/overlay/` nor an include path, so an overlay silently has no effect
until the object directory is cleared.

## What it took

**Forty exact-text edits across fifteen files**, applied to copies under
`build/overlay/` and never to `deps/`. Each one must match exactly once or the
build stops with the file named, so an upstream change is something to look at
rather than something that quietly changes what is built.
`build_tb.py --show` prints the resulting command and writes the overlay
directory without compiling anything.

Nine of the forty are shared with `ports/core_ibex_uvm` and were diagnosed
there: two in `clk_rst_if.sv`, which excludes its own UVM includes under
`` `ifndef VERILATOR `` and declares `logic o_rst_n;` with no initialiser so
that the first `apply_reset()` produces no edge, and seven in
`csr_utils_pkg.sv` for the two IEEE 1800 13.2.2 violations where an automatic
task writes its output arguments after a timing control. `csr_utils_pkg.sv` is
in this compile only because `dv_lib_pkg` imports it; nothing in
`dv/uvm/icache` names either task.

The rest are new, and they fall into three groups.

### 1. Two upstream defects

**`tb.sv` overrides a parameter the DUT does not have.** It writes

```systemverilog
  ibex_icache #(
      .ICacheECC            (ICacheECC),
      .ICacheTweakInfection (ICacheTweakInfection),
```

and `ibex_icache`'s parameter is called `TweakInfection`.
`ICacheTweakInfection` is the name the wrapper parameter carries in
`ibex_if_stage.sv`, `ibex_core.sv` and `ibex_top.sv`, which is presumably where
it was copied from. No simulator can elaborate `tb.sv` as vendored; Verilator
says "Parameter not found: 'ICacheTweakInfection'" and stops.

**A race between `set_active()` and the clock generator.** `tb.sv` calls
`clk_rst_if.set_active()` as the first statement of its initial block, and
`clk_rst_if`'s own initial block blocks on `@set_active_called`. Both are at
time 0 with nothing between them, so which runs first is arbitrary. Here
`tb`'s runs first, the event fires with nobody waiting on it, and the clock
generator never starts: time steps straight from the reset assertion to the UVM
timeout with no clock edge at all.

Upstream knows about this one testbench over: `core_ibex_tb_top.sv` has
`#0; // needed for dsim` immediately before its own `set_active()` call. The
same `#0` here yields to the interface's initial block and the clock starts.
`dv/uvm/icache/dv/tb/tb.sv` has no `#0`.

Two smaller things, neither fixed because neither breaks anything:
`ibex_icache_fcov_if.sv:219` assigns to `fill_has_ext_req`, which is never
declared -- `fill_awaiting_ext_req` is declared beside it and never used -- so
an implicit net is created. And `ibex_icache_core_base_seq::gap_between_seeds`
is set by `ibex_icache_passthru_vseq` and `ibex_icache_invalidation_vseq` and
is read nowhere.

### 2. A UVM version difference

The full comparison of the two UVM versions is in "UVM 1.2 against 1800.2"
below. This is the one that stopped the build coming up at all.

`dv_base_test::run_phase` reads `uvm_phase::phase_done` directly on its first
statement. UVM 1.2 creates that objection in the phase constructor; Accellera's
1800.2-2020.3.1, which is what `deps/uvm_core` is and what Verilator has to be
given because it ships no UVM, creates it on demand in `get_objection()`.
Nothing has called that by the time a component's `run_phase` starts, so the
member is null and the run stops with "Null pointer dereferenced". Calling
`get_objection()` returns the same object and creates it if it is not there.
This is the only direct use of `phase_done` in lowRISC's DV library.

Not a Verilator defect, and worth separating from the ones that are.

### 3. Verilator defects

Six, five of them new here, each with a reduced case in `shims/` or in
`ports/ibex_icache_cpptb/shims/`. Four are about randomization and two about
clocking blocks and `soft`.

| defect | reduced case | effect here |
| --- | --- | --- |
| a `dist` is applied as an equality against a pre-drawn sample | `verilator_dist_plus_equality.sv` | any other constraint on the same variable makes `randomize()` fail |
| `std::randomize(x) with { x dist {...} }` ignores the weights | `verilator_dist_std_randomize.sv` | uniform over the support instead |
| a constrained `randomize()` over an `inside` range is not uniform on it | `ports/ibex_icache_cpptb/shims/verilator_inside_range_uniformity.sv` | the key device's delays cluster at the top of their range |
| a `solve ... before ...` disables every `soft` constraint in the class | `verilator_soft_with_solve_before.sv` | four delay minima come out arbitrary |
| a `soft` nested inside an `if` is not dropped when it conflicts | (in the same files) | only top-level `soft` is usable |
| a one-cycle pulse through a clocking block output is lost | `verilator_clocking_pulse.sv` | no branch is driven after a mid-test reset |

Each shim is a standalone module: build it with `--binary --timing -Wno-fatal`
and run it.

#### A dist and an equality on the same variable

`dv_base_env_cfg` constrains `clk_freq_mhz` with `` `DV_COMMON_CLK_CONSTRAINT ``,
a weighted `dist` over 5..100 MHz, and `ibex_icache_env_cfg` derives from it and
adds `clk_freq_mhz == 50`. 50 is inside the distribution's support, so the pair
is satisfiable and any commercial simulator solves it. Verilator draws a sample
from the distribution first and then asserts equality against that sample, so
the whole solve is unsatisfiable unless the sample happened to be 50.

Measured over 25 seeds of this testbench, **one got past
`` `DV_CHECK_RANDOMIZE_FATAL `` in `dv_base_test`**; the other 24 died at time 0
with "Randomization failed!" and no diagnostic. The reduced case puts numbers
on it:

```
dist alone             : 200/200 succeeded, 14 drew 50
dist + derived equality: 13/200 succeeded
dist + inline equality : 10/200 succeeded
range + equality       : 200/200 succeeded
```

13 successes against 14 draws of 50 is the mechanism stated exactly. `soft` on
the dist does not help, and `%Warning-UNSATCONSTR` is printed by the reduced
case but not by the real testbench, so in the testbench the only symptom is the
fatal.

This shape is everywhere in this environment, because a `dist` for the common
case plus an implication for the special case is how the whole thing is
written. `base_addr` in the core sequence carries a three-bucket `dist` and a
separate alignment constraint, and together they fail about half the time,
which is the first randomize of every test. `run_req` writes `dist`s on
`branch_addr`, `enable` and `invalidate` in a block that also constrains all
three by implication.

#### std::randomize ignores dist weights

```
class-scope dist         1s: 985/1000 (want ~990)
inline dist on an object 1s: 993/1000 (want ~990)
std::randomize           1s: 516/1000 (want ~990)
non-constant weights     1s: 501/1000 (want ~990)
zero-weight value gone   1s: 1000/1000 (want 1000)
```

A `dist` written as a class constraint, or inline on an object's
`randomize()`, is weighted correctly. The same distribution through
`std::randomize` is not weighted at all: the support is honoured, including a
zero weight, and the pick is uniform over the values in it. lowRISC's DV
library reaches that form through `` `DV_CHECK_STD_RANDOMIZE_WITH_FATAL ``,
which is `std::randomize` by definition. Six sites in this testbench put a
`dist` through it.

The worst of the six is the core driver deciding how long to hold the request
line low:

```
req_low_cycles shape: v>=1000 1456/2000 (want ~71)
```

The weights put about 3.5% of transactions in the 1000..1200 cycle bucket. A
uniform pick over the support puts 73% of them there.

The ECC error rate is the second: `tag_sel_line dist {0 :/ dis_err_pct, [1:$] :/
100 - dis_err_pct}` with `dis_err_pct` defaulting to 99 becomes a coin flip, so
`ibex_icache_ecc` would corrupt half of all RAM reads instead of one in a
hundred.

#### A solve-before disables soft constraints

```
soft + plain second var : broken 0/200
soft + if/else          : broken 0/200
soft + solve before     : broken 197/200
```

Three classes differing in one line, with nothing competing with `soft s == 0`
in any of them. `push_pull_agent_cfg` is written in exactly that shape: four
`soft x_min == 0` constraints and four `solve zero_delays before x_max`. The
minima come out arbitrary -- `ack_lo_delay_min = 2048` against
`ack_lo_delay_max = 35` in one run -- and `push_pull_base_seq` then randomizes
`ack_lo_delay inside {[min:max]}` over an empty range and reports
"Randomization failed" part way into every run.

A second, separate defect: a `soft` constraint nested inside an `if` is
honoured when it is satisfiable and is **not** dropped when it conflicts, which
is the opposite of what soft means. Only top-level soft constraints behave
correctly, which is why the one soft constraint this port adds is at the top
level.

#### A clocking-block pulse is lost unless it starts on an edge

```systemverilog
cb.pulse <= 1'b1;   // due at the next clocking event
@(cb);              // wakes at that event
cb.pulse <= 1'b0;   // due at the event after it
```

Verilator applies the second write to the same clocking event as the first, so
the two collapse and the signal never goes high:

```
posedge t=10 level=0 pulse=0 pulse_skew=0 pulse_align=0
posedge t=30 level=1 pulse=0 pulse_skew=0 pulse_align=1
posedge t=50 level=1 pulse=0 pulse_skew=0 pulse_align=0
```

`level`, written once, appears exactly where it should. `pulse` never appears.
Neither does `pulse_skew`, the same pulse through a `default output #2ns`.
`pulse_align`, which waits for a clocking event before the first write, is
correct.

Upstream never sees this because both agent interfaces open their driver
clocking block with `default output negedge`, which puts the drive half a cycle
after the event and separates the two writes. Verilator does not implement that
skew at all -- "Unsupported: clocking event edge override" -- so it has to go,
and once it is gone the pulse goes with it.

Waiting for a clocking event before the first write restores the pulse with
upstream's timing exactly. Off an edge, upstream's negedge drive lands after
the *next* posedge and so does the aligned version; on an edge, both produce a
pulse at the following edge. The wait has to compare `$realtime` and not
`$time`: `$time` is rounded to the module's 1 ns time unit, so a caller a
picosecond off an edge compares equal to it, which is precisely the case that
needs the wait.

### What the fixes to those look like

Every `dist` that this simulator gets wrong is drawn with `$urandom_range`
instead, keeping the same buckets and the same weights. That is the approach
`ports/core_ibex_uvm` arrived at, and here it is applied to `base_addr`, the
five decided fields of the core request item, the two driver delays, the two
ECC way selects, the two ECC masks, the combo sequence's reset timer and the
memory response delay.

The `inside` range goes the same way. `push_pull_base_seq::randomize_item`
asks for `device_delay inside {[cfg.device_delay_min : cfg.device_delay_max]}`
and three more like it, and this simulator honours the range and is not
uniform on it. Measured on this testbench's own key device, with
`device_delay_max` at 131 over 37 draws, the mean came out 94 where the
range's mean is 65; drawn with `$urandom_range` it comes out 69. That was the
last distribution below the core item stream that `ports/ibex_icache_cpptb`
did not share with this testbench, and it is worth about 0.15% of that
comparison's `grants/fetch`. See its RESULTS.md.

Two of them are worth reading twice.

**`new_seed`'s weight expression is load-bearing.** The item says

```systemverilog
new_seed dist { 0 :/ 1, [1:32'hffffffff] :/ (invalidate ? 1000 : enable ? 0 : 1) };
```

and the zero in the middle is the only thing stopping a new memory seed
reaching an enabled cache, which the scoreboard would see as a multi-way hit.
The weights are not constant, and non-constant weights are the same defect
again, so that rule cannot be left to the solver. It is drawn in `run_req` from
the enable and invalidate values decided a few lines above.

**`num_insns` keeps its solve.** It is the one field the inline constraints
genuinely narrow (`num_insns <= 100 - insns_since_branch`), so instead of an
equality it gets its bucket drawn with the dist's weights and applied as a
top-level `soft`, with the support the dist implied left as a hard constraint.
A bucket the caps rule out is dropped rather than failing the call.

`solve trans_type before branch_addr` goes with them. It existed to stop branch
transactions being weighted 2^32 times higher than the others, which cannot
happen now that `run_req` draws `trans_type` itself, and leaving it would stop
the soft bucket being honoured.

**And both of them have to be drawn twice**, because
`ibex_icache_core_back_line_seq` overrides `run_req` rather than extending it.
Moving `c_new_seed_dist` and `c_num_insns_dist` out of the item and into the
base sequence's `run_req` therefore left `ibex_icache_back_line` with no
constraint on `new_seed` at all. Measured on seed 123 before this was noticed:
**all 918 items of that test drew a nonzero `new_seed`**, where the
distribution gives a nonzero seed weight zero for an item with the cache
enabled and no invalidation, which every `back_line` item is. The test never
invalidates, so nothing ever truncated the scoreboard's seed list: it ended the
run holding 919 seeds, and a fetch counted as correct if it matched any one of
them. `num_insns` went the same way, uniform over `[0:5]` instead of half of it
zero.

The overlay now makes both draws in `back_line`'s own `run_req`, with the
weights the item's constraints carry -- for `num_insns` that is the dist
restricted to `num_insns <= 5`, so weight 5 on zero and weight 1 on each of 1
to 5, because `[1:20] :/ 20` spreads its weight over twenty values.
`ibex_icache_back_line` still passes, and it now agrees with
`ports/ibex_icache_cpptb` to within 2% on every rate. This was found by that
port's comparison; see its RESULTS.md.

### The solver is the run time, twice

Verilator solves constraints by piping to `z3 --in`, so a constrained
`randomize()` is a round trip over a pipe. Two places in this testbench do one
per transaction or worse.

`gen_tag_err` and `gen_data_err` in `ibex_icache_ram_if.sv` ask for a
`$countones(mask) inside {[1:2]}` over a 28-bit tag and a 78-bit line, once per
way, on every negedge on which every way's `rvalid` is set. **`ibex_icache_ecc`
spent 18 minutes at 1 second of CPU and 41,000 voluntary context switches,
blocked on the pipe, without finishing.** The constraint admits N one-bit masks
and N(N-1)/2 two-bit masks, all equally likely, so picking uniformly between
those two populations and then within one reproduces it exactly.

With that fixed the same test still sat with z3 at 90% of a core after three
minutes: the memory response item randomizes a delay for every bus
transaction. `delay` was the item's only `rand` field, so dropping `rand` and
drawing it in `pre_randomize` takes the solver off the response path
completely -- Verilator makes no solver call for a `randomize()` with nothing
to solve.

`ibex_icache_ecc` now finishes in 16 seconds, and the whole ten-test list in
about two and a half minutes at `--jobs 4`. The same lesson as
`ports/core_ibex_uvm`, in a testbench less than half the size: a constrained
solve per transaction is affordable on a commercial simulator and is a pipe
round trip here.

## Results

Ten tests, one seed each, `run_tests.py --jobs 4`. Upstream's own pass and fail
patterns from `common_sim_cfg.hjson` decide the verdict, so a `UVM_WARNING` is
a failure here as it is there.

| test | sequence | result | seconds |
| --- | --- | --- | ---: |
| `ibex_icache_smoke` | `ibex_icache_base_vseq` | pass | 26 |
| `ibex_icache_passthru` | `ibex_icache_passthru_vseq` | pass | 24 |
| `ibex_icache_caching` | `ibex_icache_caching_vseq` | pass | 16 |
| `ibex_icache_invalidation` | `ibex_icache_invalidation_vseq` | pass | 19 |
| `ibex_icache_oldval` | `ibex_icache_oldval_vseq` | pass | 25 |
| `ibex_icache_back_line` | `ibex_icache_back_line_vseq` | pass | 10 |
| `ibex_icache_many_errors` | `ibex_icache_many_errors_vseq` | pass | 17 |
| `ibex_icache_ecc` | `ibex_icache_ecc_vseq` | pass | 16 |
| `ibex_icache_stress_all` | `ibex_icache_combo_vseq` | pass | 21 |
| `ibex_icache_stress_all_with_reset` | `ibex_icache_reset_vseq` | pass | 6 |

`ibex_icache_oldval` runs `ibex_icache_oldval_test` rather than
`ibex_icache_base_test`; the other nine all run the base test.

`ibex_icache_stress_all_with_reset` finishes in a fraction of the time the
others take, and that is upstream's own accounting rather than anything
missing. `ibex_icache_combo_vseq::body` credits `trans_now` transactions per
iteration whether or not the child sequence was killed part way through, so
with `random_reset` set the loop reaches `num_trans` after twelve short
sequences instead of twelve long ones. Twelve killed sequences is also exactly
the number of dropped responses below.


### Why `ibex_icache_stress_all_with_reset` used to fail

Twelve UVM warnings, one per killed child sequence, no errors and no fatals:

```
UVM_WARNING @ 55857590 ps: uvm_test_top.env.core_agent.sequencer [Sequencer]
  Dropping response for sequence 7, sequence not found.
  Probable cause: sequence exited or has been killed
```

`ibex_icache_reset_vseq` is `ibex_icache_combo_vseq` with `random_reset` set,
which means `run_sequence` starts a child sequence, waits a randomised 100 to
1000 cycles, and then calls `child_seq.kill()`. The core driver is normally
part way through an item at that point, and when it finishes it calls
`seq_item_port.item_done(rsp)` for a sequence that is no longer there. UVM
warns, `dv_report_server` counts warnings as failures, and the run ends "TEST
FAILED CHECKS" having found nothing wrong with the cache.

This is structural, not a race: **it happens on every seed, always first on
sequence 7, and the count is exactly the number of sequences killed.** Three
further seeds were run and all three fail the same way, while
`ibex_icache_stress_all` -- the same virtual sequence with `random_reset`
clear -- passes on all three.

Nothing here is Verilator-specific: the driver has no way to know its sequence
was killed, and upstream's own fail patterns treat a UVM warning as a failure.
What decides the verdict is which UVM the message comes from.
`uvm_sequencer_param_base::put_response` reports it with `uvm_report_info` on
1.2 and `uvm_report_warning` on 1800.2, so upstream never sees a warning at
all. `build_tb.py` restores the 1.2 severity and the test passes; the reasoning
and the scope of that override are in "UVM 1.2 against 1800.2" below.

An earlier failure in both stress tests was fixed rather than reported, and is
worth recording because of what it was. Both died on

```
UVM_ERROR @ 318210000 ps: (ibex_icache_scoreboard.sv:175) Check failed
  item.address == next_addr (2456026534 [0x9263fda6] vs 0 [0x0])
```

which is a fetch arriving after a mid-test reset with no branch in front of it.
The cause was the lost clocking-block pulse above: `branch_to()` drove no
branch at all, because `dv_base_vseq::dut_init` ends with `#1ps` and so the
first item of every child sequence after a reset is driven a picosecond off the
clock grid. The cache carried on prefetching from `prefetch_addr_q`, which
`ResetAll = 0` leaves unreset, and delivered an address the scoreboard had
never been told about. With the alignment in place `ibex_icache_stress_all`
passes and `ibex_icache_stress_all_with_reset` gets past that error to the
warnings above.

## UVM 1.2 against 1800.2

Upstream selects UVM through the simulator: `-ntb_opts uvm-1.2` for VCS,
`-uvmhome CDNS-1.2` for Xcelium. Verilator ships no UVM, so both ports compile
Accellera's `uvm-core` (1800.2-2020.3.1) from `deps/uvm_core`, pinned in
`sources.toml`. That substitution has already moved a verdict twice, in
opposite directions, so the whole surface was compared rather than left to turn
up one red row at a time. The comparison used the 1.2 release tree; `deps/`
holds only 1800.2.

Two differences change a verdict here, and both are fixed:

| what | 1.2 | 1800.2 | effect | handling |
| --- | --- | --- | --- | --- |
| `uvm_sequencer_param_base::put_response`, "Dropping response for sequence N" | `uvm_report_info` | `uvm_report_warning` | `ibex_icache_stress_all_with_reset` failed on a message upstream never sees | severity restored in `build_tb.py` |
| `uvm_phase::phase_done` | created by the phase constructor | created on demand by `get_objection()` | null dereference at the first statement of `dv_base_test::run_phase` | overlay calls `get_objection()` |

The `put_response` override is `set_report_severity_id_override(UVM_WARNING,
"Sequencer", UVM_INFO)` on every component. It is keyed on the severity as well
as the ID, and in 1800.2 that call site is the only `UVM_WARNING` anywhere in
the library carrying the ID `Sequencer`. The other three are `uvm_report_fatal`
in `uvm_sequencer_base` and an override keyed on `UVM_WARNING` cannot touch
them, so the workaround cannot hide an internal sequencer error.

### One difference that goes the other way, unfixed

**1800.2 recovers silently from a sequence killed after it won arbitration;
1.2 does not.** `uvm_sequencer::try_next_item` in 1.2 reports
`UVM_ERROR TRY_NEXT_BLOCKED` whenever the granted sequence fails to deliver an
item within an NBA delay, with no special case for a killed one:

```systemverilog
  // 1.2, uvm_sequencer.svh
  if (!m_req_fifo.try_peek(t))
    uvm_report_error("TRY_NEXT_BLOCKED", {"try_next_item: the selected sequence '",
      seq.get_full_name(), "' did not produce an item within an NBA delay. ", ...
```

1800.2 checks the sequence's process first and re-arbitrates without a message:

```systemverilog
  // 1800.2, uvm_sequencer.svh
  if (selected_sequence_request.process_id.status inside {process::KILLED,process::FINISHED}) begin
    if (arb_completed.exists(selected_sequence_request.request_id)) begin
      arb_completed.delete(selected_sequence_request.request_id);
    end
    selected_sequence = m_choose_next_request();
  end
  else begin
    ... `uvm_error("TRY_NEXT_BLOCKED", ...)
  end
```

`get_next_item` has the same shape. 1.2 grants and then blocks in
`m_req_fifo.peek(t)` forever if the winner is killed before it sends; 1800.2
routes it through `uvm_sequencer_param_base::m_safe_select_item`, which forks a
watcher on `process_id.await()` and re-arbitrates.

This is exercised. Every icache test ends `ibex_icache_base_vseq::body` with
`mem_seq.kill()`, and `ibex_icache_combo_vseq::run_sequence` calls
`child_seq.kill()` twelve times in `ibex_icache_stress_all_with_reset`. The
direction is towards a pass: on 1.2 the same kill could produce a `UVM_ERROR`
from a `try_next_item` driver, or hang a `get_next_item` driver until the
harness timeout.

**Left as is, and recorded rather than worked around.** Nothing it can hide is
a statement about the cache: the recovered-from condition is a killed stimulus
sequence, not a DUT response, and the scoreboard's checks are unaffected either
way. Restoring the 1.2 behaviour would mean making the port fail on a UVM
robustness fix.

### New diagnostics in 1800.2 that would be false failures

Each of these is a `UVM_WARNING` or `UVM_ERROR` that 1800.2 issues and 1.2 does
not, so each would fail a test here that upstream passes. **None of them
appears in any log in either port**, but they are the shapes to recognise:

| id | condition | where it could come from |
| --- | --- | --- |
| `DRVCONNECT` | a `uvm_driver` whose `seq_item_port` was never connected, checked in `end_of_elaboration_phase` | 1.2's `uvm_driver` has no `end_of_elaboration_phase` at all |
| `SEQBDYZMB`, `SEQPRTZMB` | the process that called `seq.start()` is killed without killing the sequence | `disable fork` around a running sequence |
| `UVM/SEQ/SP/SET`, `UVM/SEQ/SP/GET` | `starting_phase` written after `get_starting_phase` locked it | not used by lowRISC's DV code |
| `UVM/ABST_RGTRY/CREATE_ABSTRACT_*` | the factory is asked for an instance of an abstract class | `create_seq_by_name` with a bad `+UVM_TEST_SEQ` |
| `UVM/SQR/WFSC` | `wait_for_sequences_count` set below 1 | not set here |
| `NO_DPI_USED` | `UVM_NO_DPI` defined, warned once per run | would fail every test; a reason not to reach for that define on a simulator with no UVM |

`ibex_icache_combo_vseq` is the one place where `SEQPRTZMB` could plausibly
fire: `run_sequence` forks `child_seq.start()` against a timer and ends with
`disable fork`. It does not fire, because `child_seq.kill()` runs before the
`disable fork`.

### What was compared, and found not to matter

* **Every UVM name lowRISC's DV code uses.** 297 identifiers from
  `dv/sv/{dv_lib,dv_utils,csr_utils,dv_base_reg,common_ifs,push_pull_agent,mem_model,str_utils,mem_bkdr_util}`
  and the two testbenches resolve to a declaration in one version or the other.
  Nothing used is removed or renamed: the only name present in 1.2 alone is
  `UVM_DEFAULT_PATH`, which 1800.2 keeps as `parameter uvm_door_e
  UVM_DEFAULT_PATH = UVM_DEFAULT_DOOR`, with `typedef uvm_door_e uvm_path_e`
  beside it.
* **Every report call site.** 945 in 1.2 and 967 in 1800.2, matched on message
  text. 74 sites in 1.2 have no same-severity counterpart and 137 in 1800.2
  have none in 1.2. Excluded as unreachable: everything under `reg/`, because
  neither testbench has a register model (`ibex_icache_env_cfg` sets
  `ral_model_names = {}`; `csr_utils_pkg` is in the compile only because
  `dv_lib_pkg` imports it); every site inside `` `ifndef UVM_NO_DEPRECATED ``,
  because both flows define `UVM_NO_DEPRECATED`; and the field-automation
  `RDONLY`/`STRMTC` paths in `uvm_object_defines.svh`, which belong to the
  auto-configuration operation. Every one of the twenty files that uses
  `` `uvm_field_* `` extends `uvm_object` or `uvm_sequence_item`, never
  `uvm_component`, and auto-configuration runs on components only; those
  objects are printed and nothing else, never copied, compared or packed. What
  is left is the table above.
* **The bodies of the 274 methods lowRISC calls that both versions define.**
* **Identical in substance:** the objection core (`m_raise`, `m_drop`,
  `set_drain_time`, `get_objection_count`), `uvm_report_handler::initialize`
  and `process_report_message`, so default severity actions and the point at
  which a severity override is applied are the same; the whole of
  `uvm_report_catcher` except an added `file` argument on `summarize`;
  `uvm_heartbeat`, which this environment uses in `UVM_ANY_ACTIVE` mode;
  quit-count handling and `UVM_COUNT` to `UVM_EXIT`, which matters because
  `dv_base_test` sets `max_quit_count = 1`; `uvm_root::run_test` and its
  `+UVM_TESTNAME` resolution; `uvm_default_factory::create_object_by_name`,
  which still returns null after a `BDTYP` warning; `uvm_config_db` set/get,
  rewritten to delegate to `uvm_config_db_default_implementation_t` but keeping
  the build-phase `default_precedence - get_depth()` rule; and
  `start_phase_sequence`, which the scrambling key agent depends on.
* **`uvm_re_match`.** Rewritten in C, but the SV wrapper's new `deglob`
  argument defaults to 0, so a pattern is still compiled unanchored exactly as
  in 1.2. `dv_report_catcher` depends on that.
* **The logs.** Across 1,032 `core_ibex` runs and these 10, there is not one
  `UVM_WARNING` or `UVM_ERROR` message line: the only occurrences of those
  tokens are report-summary counters. Five message IDs in the whole corpus come
  from the UVM library rather than from `` `gfn ``: `RNTST`, `UVM/RELNOTES`,
  `UVM/REPORT/SERVER`, `UVM/REPORT/CATCHER` and `Sequencer`. The catcher
  reports zero demotions, so `dv_report_catcher` is not hiding anything either.

## How much of this is really exercised

The scoreboard is not a stub and it is not idle. Counted from a `UVM_HIGH`
run of each test, on the same seed as the table above:

| test | fetches checked | memory transactions | new seeds | invalidations | hit-ratio windows completed |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ibex_icache_smoke` | 10,704 | 17,151 | 310 | 29 | 0 |
| `ibex_icache_caching` | 11,831 | 1,086 | 0 | 0 | 13 |
| `ibex_icache_many_errors` | 6,186 | 7,546 | 82 | 21 | 1 |
| `ibex_icache_ecc` | 11,243 | 1,408 | 0 | 0 | 0 |

Every one of those fetches goes through `check_compatible`, which is a real
check: it walks the list of memory seeds the model has held and asks whether
the instruction data the cache returned matches any of them, taking
compressed, misaligned and errored fetches separately, and raises a
`uvm_error` if none matches. Each fetch is also checked for being at the
address the model expected, and the busy line is checked against the count of
outstanding memory transactions.

Three things that a green result does **not** cover:

* **The caching ratio check only fires when a window completes.** It needs 850
  fetches inside a 250-word address range with the cache enabled and no error
  and no invalidation, and the window resets on any of those. On
  `ibex_icache_caching`, which exists for it, it completed 13 times and passed
  13 times, and the address range and instruction count were checked each
  time. On `ibex_icache_smoke` it completed **zero** times, because that
  sequence toggles the enable line: the smoke test proves the environment runs
  and that 10,704 fetches returned correct data, and proves nothing at all
  about caching. `ibex_icache_ecc` completes zero windows by design --
  `ibex_icache_ecc_vseq::body` sets `disable_caching_ratio_test`.
* **`ibex_icache_ecc` does not check that ECC errors were detected.** It
  injects a corrupted way on about one RAM read in a hundred and then checks
  that the data the cache returns is still correct, which it is because the
  cache treats a detected error as a miss. Nothing counts the errors or checks
  that the ECC logic reported them; `ecc_error_o` is wired to `ram_if.ecc_err`
  and read by one SVA that the dummy assertion macros compile away. The
  evidence that anything happened at all is indirect: the ECC sequence does 30%
  more memory transactions than the caching sequence it derives from, for the
  same number of fetches.
* **`ibex_icache_oldval` is the one test whose pass is a positive statement.**
  `ibex_icache_oldval_test::check_phase` requires at least 1000 fetches where
  an old value could have been returned, and that at least 5% of them actually
  returned one. It passes, so the cache is demonstrably holding data across a
  disable and enable cycle. That check is the reason the test has its own UVM
  test class.

One more thing worth stating about what the numbers mean. Every distribution
in the stimulus is now drawn by the port rather than by the solver, with the
same buckets and the same weights, and the arithmetic for each is in
`build_tb.py` next to the constraint it replaces. Two of them are not
faithful to upstream in a way that cannot be fixed from outside: the
`num_insns` bucket is a `soft` constraint rather than a `dist`, so when the
`constrain_branches` cap rules the bucket out the value falls back to anywhere
in the support instead of being redrawn with the weights; and `base_addr`,
`branch_addr` and the ECC masks are drawn before the solve rather than as part
of it, which is equivalent here only because nothing else constrains them.

## Recording the stimulus

`+icache_record=<prefix>` makes a run write down everything its environment
does that the DUT can see, so that `ports/ibex_icache_cpptb` can drive the same
thing and compare the design's answers cycle for cycle. Three overlays add it,
and **without the plusarg none of them does anything at all**: no file is
opened and no branch is taken beyond one `$value$plusargs` per sequence.

| file | written by | contents |
| --- | --- | --- |
| `<prefix>.pins` | `tb.sv` | one line per posedge of `clk`: every input of the testbench wrapper and every output of it |
| `<prefix>.items` | `ibex_icache_core_driver.sv` | the item stream the driver was handed, and one line per new memory seed |
| `<prefix>.seq` | `ibex_icache_core_base_seq.sv` | `base_addr` and `constrain_branches`, one line per sequence start |

Everything in the pin trace is read in the Active region of the posedge, which
is the value the design samples at that edge and the value a `monitor_cb` input
sees. The ECC corruption masks are recovered as
`ic_*_rdata_o ^ ic_*_rdata_in`, which is exactly what `ibex_icache_ram_if`
exclusive-ored in and is zero wherever it applied none. About 60 bytes a cycle.

The format and what the comparison found are in
[`ports/ibex_icache_cpptb`](../ibex_icache_cpptb/README.md) and its RESULTS.md.
The short version: all ten tests at ten seeds, 4,692,318 cycles, every DUT
output matching. It also puts an independent scoreboard over this baseline's
own runs, which is worth having given that an overlay here was found to have
weakened what it checked.

## What is not done

* **Functional coverage.** `ibex_icache_fcov_if.sv` and the covergroups in the
  two agents compile and bind, and the build does not pass `--coverage`, so
  nothing is collected. Verilator would ignore a good deal of it if it did:
  23 `COVERIGN` warnings, mostly `intersect` in a cross select expression,
  explicit cross bins, and `iff` on a cross.
* **Assertions.** `prim_assert.sv` gives Verilator the dummy macros, so the
  design's SVA is compiled out, and the two protocol checker modules
  (`ibex_icache_core_protocol_checker`, `ibex_icache_mem_protocol_checker`) are
  instantiated but their properties do nothing. Upstream's flows run with
  assertions on and they are part of what those flows check.
* **Reseeding.** Upstream runs each test 50 times with random seeds. One fixed
  seed per test is what is measured here, so nothing below is a statement about
  seed sensitivity.
* **The killed-sequence recovery in 1800.2.** Recorded above, not worked
  around: this port is more tolerant of a killed stimulus sequence than the UVM
  upstream runs. It cannot mask anything the scoreboard would have found.

## Bugs worth reporting

In Ibex:

* `dv/uvm/icache/dv/tb/tb.sv` overrides `.ICacheTweakInfection` on
  `ibex_icache`, whose parameter is `TweakInfection`. The testbench cannot
  elaborate as vendored on any simulator.
* the same file calls `clk_rst_if.set_active()` with no `#0` in front of it, so
  whether the clock ever starts depends on initial-block ordering. Ibex's other
  UVM testbench has the `#0` and a comment saying which simulator needed it.
* `ibex_icache_fcov_if.sv` assigns to an undeclared `fill_has_ext_req` and
  never uses the `fill_awaiting_ext_req` declared beside it.
* `ibex_icache_core_base_seq::gap_between_seeds` is set by two virtual
  sequences and read by nothing.

In lowRISC's shared DV library:

* `dv_base_test::run_phase` reads `uvm_phase::phase_done` directly, which is
  null on UVM 1800.2 until `get_objection()` has been called.

In Verilator, five, all with reduced cases in `shims/`: `dist` applied as an
equality against a pre-drawn sample; `std::randomize` ignoring `dist` weights;
`solve ... before ...` disabling every `soft` constraint in the class; a `soft`
inside an `if` not being dropped when it conflicts; and a one-cycle pulse
through a clocking block output being lost when the first write is not on a
clocking event. The last one has no workaround inside the LRM feature set --
`default output negedge` is what the code is written against and is not
implemented, and a numeric output skew loses the pulse the same way.
