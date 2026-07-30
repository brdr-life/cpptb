# Ibex's core_ibex testbench on cpptb

`ports/core_ibex_uvm` runs Ibex's `dv/uvm/core_ibex` UVM environment on
Verilator 5 and passes **912 of the 944 entries** in
`directed_tests/directed_testlist.yaml`. This is the same testbench written
against cpptb: the same design elaborated from the same fusesoc description with
the same parameters, the same two memory agents, the same RVFI monitor, the same
Spike co-simulation scoreboard, and the same programs, compiled by the same code.

**It passes the same 912, and 943 of the 944 entries reach the identical
outcome.** Measurements and the side-by-side comparison are in
[RESULTS.md](RESULTS.md).

## Scope, and why it is this

943 of the 944 directed entries name `core_ibex_base_test` as their `rtl_test`;
the 944th names `core_ibex_mcounteren_lock_test`, which is thirty lines on top
of the base test. Both are here. Nothing else from `core_ibex_test_lib.sv` is.

That is deliberate. The other eight classes in that file need the interrupt
agent, the debug agent and the integrity/glitch machinery, and they exist to
serve the riscv-dv testlist rather than the directed one.
`ports/core_ibex_uvm/README.md` records what those entries are worth on the
baseline: of the 30 riscv-dv entries it passes, **17 are hollow** -- a plain
random program under a name whose defining `gen_opts` pyflow cannot produce --
and the eight non-base classes contribute one pass each on such a program.
Porting the agents would reproduce tests that do not test what their names
claim. The 944 directed entries are hand-written C and assembly with no
generator in the flow, so there is no generator-fidelity problem in them at all,
and they are where the checking is.

`--config opentitan` and nothing else. Every one of the 944 entries carries
`rtl_params: {PMPEnable: 1}`, so not one of them is applicable to `small`;
`fusesoc_setup.py --config small` will generate that build, and
`run_directed.py --config small` will report all 944 as inapplicable, which is
the comparison upstream's `ibex_cmd.filter_tests_by_config` makes.

## Building and running

```sh
eval "$(python3 ../../local_deps.py --env)"   # z3 and the tool libraries
export PATH="$HOME/.local/bin:$PATH"          # fusesoc 2.4.3
python3 fusesoc_setup.py --check              # cpptb.toml matches the graph
uv run --frozen cpptb build --project .
python3 run_directed.py --group riscv-tests   # 93 entries
python3 run_directed.py --against \
    ../core_ibex_uvm/build/directed/opentitan-all944/results.json
python3 replay.py                             # the baseline's own runs, here
python3 inject.py --bus dmem --count 30 --no-cosim
```

A run needs Spike, which `../../build_spike.py` builds into `deps/`, and does
not need z3: nothing in this testbench solves a constraint. `--against` joins
this run against a `results.json` from `ports/core_ibex_uvm` entry by entry and
prints the disagreements; making one of those, and making a recording for
`replay.py`, needs `python3 ../core_ibex_uvm/build_tb.py --config opentitan`
first, and that build does need z3 at run time.

A rebuild after an edit to `testbench.cpp` is about two minutes and the whole
directed testlist is about two minutes at four at a time. See RESULTS.md.

## The design under test

`core_ibex_cpptb_tb_top.sv` is `dv/uvm/core_ibex/tb/core_ibex_tb_top.sv` with
the UVM removed. Upstream's top instantiates `ibex_top_tracing` and wires it to
eight SystemVerilog interfaces that the UVM agents drive and monitor; this
instantiates the same core with the same parameters and lifts the same signals
to its own boundary, so `testbench.cpp` drives and samples them where the agents
drive and sample the interfaces. The hierarchical `assign`s at the bottom -- the
ID-stage probes, the fetch-stage probes, the PMP fetch error, the CSR interface
and the four misalignment fields -- are upstream's own lines with an output port
named where upstream names an interface field.

Five deliberate differences:

* **The interrupt pins are tied off rather than lifted.** `core_ibex_base_test`
  starts no interrupt sequence, so `irq_if` is held at zero for every run this
  port covers. Tying them says that in one place rather than leaving a bus the
  testbench has to remember to hold low.
* **The memory integrity bits are computed in the wrapper.**
  `ibex_mem_intf_response_seq` writes
  `{req.intg, req.data} = prim_secded_inv_39_32_enc(req.data)` and inverts
  `req.intg` when it wants a bad-integrity response. That is two `assign`s per
  bus here, using the same function: the testbench drives the 32 data bits and
  one `bad_intg` bit. Reimplementing a SECDED parity matrix in C++ would be a
  second copy of something the design already has, and a second place for it to
  be wrong.
* **Ports wider than 32 bits are `bit` rather than `logic`.** cpptb's generated
  transport carries a port wider than 32 bits only when it is two-state and not
  an `inout`. That covers the scrambling key and nonce, `rvfi_order`,
  `rvfi_ext_mcycle` and the two performance-counter vectors, none of which
  carries meaning beyond its value.
* **`rvfi_ext_mhpmcounters` and `rvfi_ext_mhpmcountersh` are packed vectors**
  where `ibex_top_tracing` has unpacked arrays, because cpptb carries packed
  vectors. Each counter is 32 bits and lands on a 32-bit boundary, so
  `testbench.cpp` reads counter *i* as word *i* and no arithmetic is involved.
* **`unused_assert_connected` is driven inside `` `ifdef INC_ASSERT ``.**
  `core_ibex_tb_top.sv` drives it unconditionally, outside the guard that
  declares it, which is why `ports/core_ibex_uvm` has to overlay
  `prim_assert.sv` to get INC_ASSERT defined. Guarding the assign changes
  nothing on a simulator that does define it.

The RTL list comes from fusesoc, as it does for the baseline.
`core_ibex_cpptb.core` is a CAPI=2 core depending on
`lowrisc:ibex:ibex_top_tracing` and `lowrisc:prim:secded`, and
`fusesoc_setup.py` resolves it and writes `cpptb.toml`. That file is committed
so `cpptb build` needs no wrapper script, and `--check` fails if it no longer
matches. The resolution is **136 sources, two include directories and 17
Verilator control files**. The parameters and the four `IBEX_CFG_*` defines come
from `util/ibex_config.py`, which is the same tool `scripts/compile_tb.py` and
`ports/core_ibex_uvm/build_tb.py` call, so a change to `ibex_configs.yaml`
changes what both harnesses build rather than only one of them.

`fusesoc_setup.py` maps each file fusesoc copied back to the tree it came from
by the path inside its core rather than by basename. `deps/ibex` has two files
called `verilator_waiver.vlt`, and a basename map silently handed Verilator the
wrong one.

## What is ported

| in `testbench.cpp` | upstream |
| --- | --- |
| `MemoryModel` | `mem_model_pkg::mem_model` |
| `grant_driver` | `ibex_mem_intf_response_driver::send_grant` |
| `response_driver` | `ibex_mem_intf_response_driver::send_read_data` |
| `bus_monitor` | `ibex_mem_intf_monitor`, `ibex_mem_intf_response_seq::body` and the base test's `test_done_port` subscriber, fused; see "Two architectures" |
| `mem_intf_monitor`, `response_sequence`, `spurious_sequence`, `test_done_subscriber`, `cosim_dside_subscriber`, `response_driver_faithful` | the same, decomposed as UVM decomposes it |
| `irq_monitor`, `irq_driver` | `irq_agent`'s monitor and driver |
| `make_response`, `read_word`, `write_word` | `ibex_mem_intf_response_seq`'s item construction, `read` and `write` |
| `key_device` | `push_pull_agent` in Pull/Device mode |
| `fetch_enable_stimulus` | `fetch_enable_seq` |
| `CosimScoreboard` | `ibex_cosim_scoreboard`, whole |
| `cosim_monitor` | `ibex_rvfi_monitor`, `ibex_ifetch_monitor`, `ibex_ifetch_pmp_monitor` and `core_ibex_scoreboard::double_fault_detector` |
| `CosimBridge` | `cosim_dpi.svh` and `spike_cosim_dpi.cc` |
| `run_core_ibex_test` | `core_ibex_base_test`: `run_phase`, `load_binary_to_mems` and `wait_for_test_done` |
| `mcounteren_lock_stimulus` | `core_ibex_mcounteren_lock_test::send_stimulus` |

The co-simulation scoreboard is the point of the exercise, and it is ported
whole: `run_cosim_rvfi` with its five ordered `set_*` calls and the twenty
performance-counter writes, `run_cosim_dmem`, `run_cosim_ifetch`,
`run_cosim_ifetch_pmp`, `run_cosim_imem_errors` and
`run_cosim_prune_imem_errors`. Spike is linked in directly rather than through
DPI -- `testbench.cpp` holds a `SpikeCosim` and calls its C++ interface where the
UVM environment holds a `chandle` and calls the same functions through
`cosim_dpi.svh`. The construction arguments, the whole-address-space
`add_memory`, and the `static_cast<Cosim *>` that `spike_cosim_dpi.cc` needs
because `SpikeCosim` has two bases, are all the same.

Four processes and two analysis ports collapse into one coroutine per bus, and
six forked tasks into one `cosim_monitor`. That fixes an order UVM leaves to the
scheduler; the order chosen is the order the data flows, and it is stated in the
file.

## Two architectures, and why

That collapse is the reason this port needs a second configuration. It is a
defensible way to write the testbench, but it is not UVM's structure, so a
throughput comparison against UVM charges cpptb for neither the extra processes
nor the per-transaction object. `IBEX_ARCH` selects which decomposition runs:

| | `lean` (default) | `faithful` |
| --- | --- | --- |
| per-bus monitor | fused with the response sequence and the `test_done` subscriber | `mem_intf_monitor`, publishing to two analysis ports |
| response sequence | called inline by the monitor | `response_sequence`, blocked on the address-phase fifo |
| driver handover | the sequence pushes into a deque the driver pops | `get_next_item` / `item_done` through `Sequencer` |
| transaction | `MemItem` by value in a deque | allocated per transaction, published, dropped |
| signature write | called directly from the monitor | `test_done_subscriber`, a second subscriber on the dside port |
| interrupt agent | absent | `irq_monitor` sampling five pins every cycle, plus `irq_driver` parked on a sequencer |

`faithful` mirrors the 18 components `core_ibex_env` builds for every test.
`uvm_analysis_port::write` is a function rather than a task, so its subscribers
run in the instant the monitor writes; cpptb's `Queue` wakes a consumer through
`flush_external_wakes`, which resumes it inside the `put`. The two paths
therefore do the same work at the same point in the same posedge, which is why
the replay below matches on both.

Two things `faithful` does not mirror:

* Upstream's `ibex_mem_intf_response_seq::body` holds the request arm and the
  spurious-response arm in one process. Here they are two coroutines, because
  the arms block on different things -- an analysis fifo and a clock edge -- and
  joining them needs a select over both that this port does not have.
* Mirroring UVM's decomposition reproduces its structure but not the cost of its
  class library executing that structure: the factory, the phasing, and
  `uvm_object`'s own machinery are absent by design. So `faithful` **bounds** how
  much of the measured gap is architecture rather than framework. It does not
  isolate it, and no build of this port can.

The interrupt pins are lifted to the wrapper boundary for this. Nothing under
`core_ibex_base_test` raises an interrupt, so they are zero on every run either
way, and `irq_items` in the report says so as a measurement rather than an
assumption.

## Functional coverage

`dv/uvm/core_ibex/fcov/` declares three covergroups -- `uarch_cg`,
`pmp_region_cg` and `pmp_top_cg` -- with 69 coverpoints and 46 crosses between
them. Neither this port nor the UVM baseline used to sample any of them. The
baseline still cannot, and that is worth stating precisely rather than as "off
by default":

```
$ python3 ../core_ibex_uvm/build_tb.py --config opentitan --fcov
%Error: Internal Error: core_ibex_fcov_if.sv:589:28: ../V3Ast.h:1061:
        AstNode is not of expected type, but instead has type 'ENUMITEMREF'
   589 |       bins out_of_reset = (RESET => BOOT_SET);
```

That is one hard error, and fixing it would not be enough. The same compile
discards 456 coverage constructs with COVERIGN warnings:

| ignored | count |
| --- | ---: |
| `intersect` in a coverage select expression | 208 |
| `&&` in a coverage select expression | 131 |
| explicit coverage cross bins | 71 |
| `iff` in a coverage cross | 21 |
| `with`, `\|\|`, `binsof`, `sequence`, `bins` array size | 25 |

A cross whose select expression is dropped keeps every combination its
coverpoints allow, which is a much larger bin set than the source describes. So
Verilator cannot produce Ibex's functional coverage, correct or otherwise, and
there is no reference run to compare this port against.

### How equivalence is checked instead

Against the specification, mechanically. `fcov_model.py` parses the covergroups
out of the SystemVerilog -- all 69 coverpoints and all 46 crosses, and it
checks that count against the declarations rather than trusting its own
parser -- and `--diff` compares them with the model this port declares:

```sh
IBEX_COVERAGE_JSON=build/cov.json python3 run_directed.py --only add-01
python3 fcov_model.py --diff build/cov.json
```

```
=== uarch_cg ===
  coverpoints  14 of 39 ported
  crosses      0 of 27 ported
54 disagreement(s) between the SystemVerilog and the port
```

That number is meant to be read. Most of what is missing needs the 318 lines of
derived-signal logic in `core_ibex_fcov_if.sv` and about 28 more hierarchical
signals lifted to the wrapper; the crosses need those coverpoints first. A
coverpoint added upstream shows up here as missing rather than going unnoticed.

### What the framework grew

cpptb's coverage API could express single-range bins, two-way crosses over
every combination, and two-state transitions. Ibex's coverage needs more, so
`include/cpptb/coverage.hpp` gained bins over value lists, wildcard bins written
as the pattern (`"1?????"`), bin arrays, crosses of any arity, cross bin
selection with `binsof`/`intersect` and its `&&`, `||` and `!`, `iff` guards on
coverpoints and crosses, and `illegal_bins = default sequence`.
`tests/unit/coverage_sv_semantics_test.cpp` checks each against IEEE 1800-2023
clause 19, because there is no simulator here to check them against.

One construct is not exact. SystemVerilog's `with (expr)` filters a cross select
by an expression over coverpoint *values* where cpptb selects whole bins. The
two agree whenever the expression is constant across each bin, which holds for
every `with` in Ibex's coverage, and `where()` records the SystemVerilog it
stands for so the translation stays checkable. Where an expression varies within
a bin, no bin-level predicate can match it.

### What implementing `default sequence` found

`cp_controller_fsm` and `cp_controller_fsm_sleep` both end with

```systemverilog
// TODO: VCS does not implement default sequence so illegal_bins will be empty
illegal_bins illegal_transitions = default sequence;
```

Implemented as IEEE 1800 19.5.2 describes it, that bin catches every transition
no listed bin accepts -- and `DECODE => DECODE` is such a transition. The
controller sits in `DECODE` for most of every program, so across the directed
testlist these bins fire **15,080,448 times in 14,987,388 sampled cycles**.

Upstream's comment is the reason nobody has seen it: the coverpoint reads as
harmless only because no simulator they run implements the construct. It is
counted here and reported as `coverage_illegal`, and deliberately not treated as
a testbench failure -- it is a statement about the covergroup, not about Ibex.

## Timing

The design samples on the rising edge. `co_await RisingEdge` resumes before the
design has evaluated that edge, so it yields the value `@(posedge clk)` reads in
the Active region and is where every monitor here samples. Driving there would
be wrong, because `set()` is immediate and the value would be captured by the
edge being awaited.

Everything that drives a pin therefore works from a **drive point**: the instant
just after a falling edge. Upstream's drivers write through `@(posedge clk)`
clocking blocks with the default output skew of zero, which means the pin changes
in the NBA region of a posedge and is sampled by the DUT at the *following*
posedge. A value written at the drive point between those two posedges reaches
the DUT at exactly the same edge. So:

* a statement upstream writes at `@(cb)` -- posedge *p* -- is written here at the
  drive point after *p*;
* a statement upstream writes after `wait_neg_clks()` -- a negedge between
  posedge *p* and *p+1* -- is written here at the drive point after *p+1*,
  because the clocking block defers it to the next clocking event.

The second is why `grant_driver` waits one falling edge more than `send_grant`
appears to. Getting it wrong costs a cycle of grant latency on every bus access,
and `replay.py` is what would notice.

## Replay

Until this was built the two harnesses had only ever been compared on whether
they reached the same verdict on the same program, which is a statement about two
runs of a testbench. `replay.py` makes a statement about one run: the baseline
writes down everything its environment does that the DUT can see, this port
drives the same thing at the same pins, and the DUT's outputs are compared cycle
for cycle.

```sh
python3 replay.py                     # nine entries across the three groups
python3 replay.py --only add-01 --keep
python3 replay.py --perturb 5000      # show the comparison is live
```

**The recording is made by the baseline and replayed here**, which is the
direction that tests the port against the reference. `+core_ibex_record=<prefix>`
is added to `ports/core_ibex_uvm` by one overlay in its `build_tb.py`, and
without the plusarg it does nothing at all, so the binary that produces a
recording is the binary the 912 was measured with. It writes one line per posedge
of `clk` -- every input of the cpptb wrapper and thirteen fields of its outputs,
about 105 bytes a cycle -- with a `K` line for the scrambling key and nonce only
when they change.

Everything in the recording is read in the Active region of a posedge, which is
the value the design samples at that edge. So a replay that drives those inputs
at its drive point, half a cycle earlier, presents the design with the same
stimulus, and one that reads the outputs at the edge itself reads what the
recording read.

Three things are checked per entry:

* every cycle of the recording is replayed and all thirteen output fields match
  on every one of them;
* **this port's co-simulation scoreboard runs on the baseline's stimulus**, and
  the number of instructions it steps Spike through is compared against the
  number the baseline's own scoreboard reported. That is the same check made
  twice by two different pieces of code on the same run, and it is the one thing
  that had never been done in either direction;
* the recording carries only integrity bits that are the SECDED encoding of the
  response data or its inverse, which is what the wrapper can drive. Anything
  else would be a response this port cannot reproduce, and it is reported rather
  than approximated.

The recording begins at the first posedge, by which time `core_ibex_tb_top`'s
initial block has already driven `rst_n`, so a replay runs two idle cycles with
`rst_ni` high first and applies cycle 0 as a real falling edge. Without one the
design's asynchronous resets never fire.

**Nine entries at 375,100 cycles: every field matches on every cycle, and the
26,090 instructions the baseline retired are accepted here, with the two
scoreboards' instruction counts agreeing exactly.** No divergence has been seen.
RESULTS.md has the table.

`IBEX_REPLAY_PERTURB=N` moves one bit of the first instruction returned at or
after cycle N, and the divergence is reported 1 to 24 cycles later:

```
$ IBEX_REPLAY_PERTURB=5000 CPPTB_TEST=core_ibex_base_test \
    IBEX_REPLAY=build/replay/default9/add-01/add-01 ... ./Vdpi_core_ibex_cpptb
cpptb-core-ibex replay: the instruction returned at cycle 5653 was changed
  from 0x00b50633 to 0x00b50632
cpptb-core-ibex replay divergence at cycle 5677:
  recorded out=001 iaddr=800007c8 daddr=fffffc00 be=8 wdata=f7fffffb
    order=465 pc=800007b0 rd=0a rdw=fffff7fe
  replayed out=009 iaddr=800007c8 daddr=fffffc00 be=8 wdata=f7fffffb
    order=465 pc=800007b0 rd=0a rdw=fffff7ff
```

### What a replay cannot carry

A recording of a long entry is large: 105 bytes a cycle is 24 MB for the
227,000-cycle ePMP entries and would be 500 MB for the longest. `--record-max`
bounds it, and an entry recorded to a prefix of its run is replayed to that
prefix and says so. The nine default entries are all recorded whole.

## Showing the checks are live

`inject.py` flips one bit of one memory read response per run, over a range of
responses, and counts how many of those runs the co-simulation caught.
`IBEX_NO_COSIM=1` re-runs the same injections with the reference model off,
which is the control.

```
$ python3 inject.py --only add-01 --bus imem --count 40 --no-cosim
38 of 38 injections caught, 0 silent
2 runs crashed the reference model rather than reaching a verdict
with the reference model off: 29 of 40 caught

$ python3 inject.py --bus dmem --count 30 --no-cosim
17 of 30 injections caught, 13 silent
with the reference model off: 0 of 30 caught
```

The data-bus column is the one to read. Nothing in the program's own checking
caught a single corrupted load; the co-simulation caught 17 of 30, and the 13 it
did not are loads whose value never reached an architectural state the program
looked at. What a caught one says:

```
cpptb: testbench.cpp:1184: Cosim mismatch DUT generated load at address
  80000360 with data 5 but data 4 was expected with byte mask 1
```

The injection index counts read responses only. A write response carries a
constant with matching integrity that the load/store unit discards, so
corrupting one is invisible by construction and counting them would make the
index mean nothing.

`IBEX_INJECT_IMEM_ERROR=N` and `IBEX_INJECT_DMEM_ERROR=N` are a different thing:
they call `ibex_mem_intf_response_seq::inject_error` on the Nth read request,
which is a real part of the agent that no directed entry reaches, because only
`memory_error_seq` calls it. Without them the error path and the iside-error
tracking in the co-simulation scoreboard would be ported and dead. RESULTS.md
has what they show.

Two of the forty instruction-bus injections crash Spike with
`*** stack smashing detected ***`. `ports/core_ibex_uvm/README.md` records the
same crash on two of its own riscv-dv runs, so it is the reference model rather
than the port, and `inject.py` reports it as itself rather than as a catch or a
silent pass.

## Divergences from the UVM environment

Stated plainly, because a port that quietly checks less is not a comparison.

1. **Eight of the ten test classes are not ported**, and neither is the
   interrupt agent nor the debug stimulus. See "Scope" above. Nothing in the 944
   directed entries reaches any of them.
2. **No functional coverage.** Neither side collects any: `ports/core_ibex_uvm`
   compiles with `DV_FCOV_DISABLE` defined and the three fcov sources out of the
   build, because Verilator 5.050 dies with an internal error on the FSM
   transition bins. Neither has covergroups that run.
3. **No assertions.** `prim_assert.sv` gives Verilator the dummy macros, so the
   design's SVA is compiled out on both sides. This port does not write the
   `NoAlertsTriggered` assertion `core_ibex_tb_top.sv` carries, which is the
   same amount of checking and less pretence. It does mean **neither harness
   notices an alert**, and the alert outputs are compared by `replay.py` but by
   nothing else.
4. **The wrapper always drives well-formed integrity bits.** Upstream's driver
   puts `'x` on `rdata` and `rintg` between responses, which Verilator makes
   zero; the wrapper computes the encoding of whatever is on `rdata`, so between
   responses the two differ by seven bits nothing samples. `fixed_data_write_response`
   is honoured -- it is tied to `SecureIbex` upstream, and `ibex_load_store_unit`
   checks the integrity of write responses as well as read ones -- so on the
   `opentitan` build every response either side drives is well formed.
5. **The reset is exactly 100 cycles and begins at a drive point.**
   `clk_rst_if::apply_reset` puts a `$urandom_range(0, clk_period_ps)` delay in
   front of the assertion, which has no counterpart here: this port has no
   sub-cycle drive point.
6. **The backdoor load happens a hundred cycles later in simulated time.**
   Upstream forks `apply_reset` and runs `run_phase`'s hundred-cycle wait beside
   it; here the reset completes first and the wait follows. It is a zero-time
   backdoor write and the core cannot fetch until `fetch_enable` goes on after
   it, so nothing observes the difference -- but it is a difference.
7. **The ordering inside a clock edge is fixed rather than left to the
   scheduler.** Upstream's cosim agent forks six tasks over five clocking
   blocks; here they run in one coroutine, dside first and RVFI last. UVM leaves
   that order undefined and the design tolerates it, but the two harnesses are
   not making the same choice, they are making a defined one and an undefined
   one that happen to agree.
8. **`wait_for_mem_txn` is implemented only for `TEST_RESULT`.** The base test's
   `CORE_STATUS`, `WRITE_GPR` and `WRITE_CSR` arms exist for the classes that
   are not ported. A malformed signature write is reported rather than ignored.
9. **The wall-clock timeout is the runner's rather than the testbench's.**
   `core_ibex_base_test` polls `get_unix_timestamp()` every 1000 us of simulated
   time; here `run_directed.py` applies the entry's own `timeout_s` as a
   subprocess timeout. `scall` is the one entry this changes: it is a wall-clock
   timeout on the baseline and a cycle timeout here, because cpptb reaches
   5,000,000 cycles inside the 300-second budget and the baseline does not.
10. **`+timeout_in_cycles` counts from a slightly different zero.** Upstream
    counts from the start of `wait_for_test_done`; here from the end of the
    reset. At 5,000,000 cycles the hundred-odd cycles between them do not
    matter.

Nothing in the co-simulation scoreboard is missing.

## What a failure reports

```
$ IBEX_CORRUPT_IMEM=40 CPPTB_TEST=core_ibex_base_test IBEX_BIN=add-01/test.bin \
    ./Vdpi_core_ibex_cpptb
cpptb-core-ibex: corrupting imem read response 40 at 0x8000011c
cpptb: testbench.cpp:1184: Cosim mismatch Register write data mismatch to x28
  DUT: 5bffdb7d expected: 40000
cpptb: testbench.cpp:1184: Cosim mismatch DUT generated store at address
  80006008 with data 5bffdb7d but data 40000 was expected with byte mask f
cpptb-core-ibex core_ibex_base_test outcome=cosim-mismatch cycles=249
  retired=41 cosim_steps=41 cosim_matched=39 ...
```

Before a debugger: the source location of the check, Spike's own description of
the disagreement, how far the run got, and how many instructions had matched.
The counters line is what `run_directed.py` records, so a results file carries
the shape of every run as well as its verdict.

## Files

```
core_ibex_cpptb.core          CAPI=2 description of the RTL half
core_ibex_cpptb_tb_top.sv     core_ibex_tb_top.sv without the UVM
fusesoc_setup.py              resolves the graph, writes and checks cpptb.toml
cpptb.toml                    generated; do not edit
testbench.cpp                 memory agents, RVFI monitor, cosim scoreboard,
                              the two test classes, and the replay mode
run_directed.py               the 944 directed entries, importing the baseline's
                              compile so both harnesses run identical binaries
replay.py                     records the baseline's stimulus and replays it
inject.py                     one corrupted response per run, and what is caught
```
