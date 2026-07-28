# Ibex's icache UVM testbench on Verilator 5

`dv/uvm/icache` is Ibex's second UVM testbench and its only block-level one:
the DUT is `ibex_icache` on its own, with a core agent on the fetch side, a
memory agent on the bus side, a push-pull agent supplying the scrambling key,
and a scoreboard that models the cache against every seed the memory has held.
Upstream signs it off with VCS and runs it through OpenTitan's dvsim.
`dv/ibex_icache_sim_cfg.hjson` names `vcs`, and the Makefile beside it defaults
to `xcelium`. Verilator is not one of the options.

**Status: it builds, and nine of the ten tests in upstream's own list pass on
Verilator 5.050.** The one that does not is
`ibex_icache_stress_all_with_reset`, and it fails on twelve UVM warnings from
the sequencer rather than on anything the scoreboard found. The whole list runs
in about two and a half minutes at `--jobs 4`.

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

A build is about four minutes at `--jobs 4` and the binary is 47 MB. An SMT
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

Five, four of them new here, each with a reduced case in `shims/`. Three are
about `dist` and two about clocking blocks and `soft`.

| defect | reduced case | effect here |
| --- | --- | --- |
| a `dist` is applied as an equality against a pre-drawn sample | `verilator_dist_plus_equality.sv` | any other constraint on the same variable makes `randomize()` fail |
| `std::randomize(x) with { x dist {...} }` ignores the weights | `verilator_dist_std_randomize.sv` | uniform over the support instead |
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
| `ibex_icache_stress_all_with_reset` | `ibex_icache_reset_vseq` | **fail** | 3 |

`ibex_icache_oldval` runs `ibex_icache_oldval_test` rather than
`ibex_icache_base_test`; the other nine all run the base test.


### Why `ibex_icache_stress_all_with_reset` fails

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
Whether upstream sees it depends on whether the kill can ever land between
items, and on the evidence here it cannot avoid landing inside one. It is not
fixed, because the point of the port is to run what upstream ships.

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
* **The two stress tests.** Diagnosed, not fixed.

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
