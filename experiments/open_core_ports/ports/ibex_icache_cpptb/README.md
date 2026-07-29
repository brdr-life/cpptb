# Ibex's icache testbench on cpptb

`ports/ibex_icache_uvm` runs Ibex's `dv/uvm/icache` unmodified on Verilator 5,
and all ten of its tests pass. This is the same block-level testbench written
against cpptb: the same design, the same RTL sources from the same fusesoc
description, the same stimulus generator, and the same scoreboard.

All ten tests are ported. Measurements and the side-by-side comparison are in
[RESULTS.md](RESULTS.md).

## Building and running

```sh
export PATH="$HOME/.local/bin:$PATH"          # fusesoc 2.4.3
python3 fusesoc_setup.py --check              # cpptb.toml matches the graph
uv run --frozen cpptb build --project .
python3 run_tests.py                          # ten tests, three seeds
python3 run_tests.py --compare                # and the UVM baseline beside it
python3 replay.py                             # the baseline's stimulus, here
```

`--compare` runs `ports/ibex_icache_uvm` as well, which needs a built baseline
and z3 on `PATH`; `run_tests.py` puts z3 there itself through `local_deps.py`,
and refuses to report anything if it cannot find one. A UVM_HIGH log runs to
45 MB and lands in `ports/ibex_icache_uvm/build/results`.

Rebuilding takes about fifteen seconds and the ten tests take about a second
between them.

## The design under test

`ibex_icache` cannot be elaborated on its own. It has tag and data RAM ports
that something has to answer and a scrambling-key port that something has to
service, so upstream's `dv/uvm/icache/dv/tb/tb.sv` wraps it in two
`prim_ram_1p_scr` instances per way plus a key latch.
`ibex_icache_tb_top.sv` here is that module with the UVM removed: the same DUT
instantiation, the same key logic, the same RAMs, and the core-side,
memory-side and key pins lifted to the boundary so the cpptb testbench drives
them where the UVM agents drive interfaces.

Three deliberate differences from `tb.sv`:

* the parameter override is spelled `TweakInfection`. `tb.sv` writes
  `.ICacheTweakInfection`, which `ibex_icache` does not have, so `tb.sv` cannot
  elaborate as vendored on any simulator. `ports/ibex_icache_uvm` patches the
  same thing.
* `ibex_icache_ram_if` is not instantiated. What it does for `ibex_icache_ecc`
  -- exclusive-or a sparse mask into each way's RAM read data on the way to the
  cache -- is two mask inputs on the wrapper that `testbench.cpp` drives, sitting
  where the interface's `ic_*_rdata_err` mux sits. Its clocking block, its
  `dist` draws and its SVA belong to the UVM environment.

  The masks are `bit` rather than `logic`, and flat vectors rather than one
  entry per way: cpptb's code generation carries a port wider than 32 bits only
  if it is two-state and not an `inout`, and carries packed vectors rather than
  unpacked arrays. So the data mask is one 156-bit `bit` port, two ways of
  `LineSizeECC` = 78, and the tag mask is one 56-bit port, two ways of
  `TagSizeECC` = 28. Neither costs anything: a mask is a bag of bits with no
  meaning beyond which of them are set, exactly as the 128-bit key was. The
  widths are named in `testbench.cpp` as well, so a change to `ibex_pkg` stops
  the port compiling rather than quietly corrupting the wrong bits.
* the key, nonce and valid bit are inputs rather than fields of a
  `push_pull_if`. The key device is a coroutine in `testbench.cpp`.

The RTL list comes from fusesoc, as it does for the UVM baseline.
`ibex_icache_cpptb.core` is a CAPI=2 core depending on
`lowrisc:ibex:ibex_icache` and the primitives the wrapper instantiates, and
`fusesoc_setup.py` resolves it and writes `cpptb.toml`. That file is committed
so `cpptb build` needs no wrapper script, and `--check` fails if it no longer
matches what fusesoc resolves. The resolution is **77 sources, one include
directory and five Verilator control files**.

Two dependencies had to be named that upstream does not name:
`lowrisc:prim:ram_1p_adv` declares neither `lowrisc:prim:mubi` nor
`lowrisc:prim:flop`, although `prim_ram_1p_adv.sv` imports `prim_mubi_pkg` and
`prim_ram_1p_scr.sv` instantiates `prim_flop`. Nothing notices upstream because
every flow that reaches `ram_1p_scr` also reaches a core that does depend on
them.

## What is ported

| in `testbench.cpp` | upstream |
| --- | --- |
| `core_stimulus` | `ibex_icache_core_base_seq` and `ibex_icache_core_back_line_seq`, and the ten virtual sequences' knobs |
| `drive_branch`, `drive_req`, `read_insn`, `lower_req`, `maybe_invalidate` | `ibex_icache_core_driver` and `ibex_icache_core_if` |
| `core_monitor` | `ibex_icache_core_monitor` |
| `mem_monitor` | `ibex_icache_mem_monitor` and `ibex_icache_mem_resp_seq` |
| `mem_grant_driver`, `mem_responder` | `ibex_icache_mem_driver` and `ibex_icache_mem_if` |
| `key_device` | `push_pull_agent` in Pull/Device mode |
| `ecc_corrupter` | `ibex_icache_ram_if`'s `gen_tag_err` and `gen_data_err` |
| `apply_reset` | `dv_base_vseq::dut_init`, `clk_rst_if::apply_reset`, the drivers' `reset_signals` and the core sequence's `reset_ifs` |
| `run_child` | `ibex_icache_base_vseq`'s `pre_start` and `body` |
| `combo_body` | `ibex_icache_combo_vseq` and `ibex_icache_reset_vseq` |
| `check_oldval` | `ibex_icache_oldval_test::check_phase` |
| `hash32`, `read_data`, `is_mem_error` | `ibex_icache_mem_model` |
| `Scoreboard` | `ibex_icache_scoreboard` |

The scoreboard is the point of the exercise, and it is ported whole:

* **`check_compatible`.** Every returned fetch is checked against every memory
  seed the model still holds, with `is_fetch_compatible_1` for aligned,
  errored and compressed fetches and `is_fetch_compatible_2` over every pair of
  seeds for a misaligned uncompressed one. The seed list is grown on every new
  seed and truncated at the first branch after an invalidation, and `no_cache`
  narrows the search to the seed in force at the last branch whenever the cache
  has been disabled since before it.
* **the address sequence.** Every fetch must arrive at the address the model
  expected, which advances by 2 or 4 according to the returned opcode bits and
  is reset by a branch.
* **the busy line.** Checked whenever busy falls and just before a pending
  memory request is answered, which is where upstream checks it and for the
  reason its comment gives.
* **the caching ratio.** 850 fetches inside a 250-word range with the cache
  enabled, no error and no invalidation completes a window, and the window then
  requires `(reads * 53/32) / insns <= 67%`. Same window length, same width
  bound, same arithmetic.
* **old-value tracking.** `possible_old` and `actual_old` are counted, and
  `ibex_icache_oldval` requires 1000 of the first and at least 5% of them in the
  second, which is `ibex_icache_oldval_test::check_phase`.
* **the reset hooks.** `start_reset` forgets the expected address, every seed
  but the newest, the branch index and the outstanding-transaction count;
  `reset` clears the caching window. Both are what the two stress tests need.

The stimulus draws are taken from `ports/ibex_icache_uvm/build_tb.py` rather
than from upstream's constraint code, because that is the stimulus the baseline
actually runs: Verilator gets `dist` wrong in three separate ways, so every
distribution in that environment is already drawn directly there, with the same
buckets and the same weights. `base_addr`, the branch target, `enable`,
`invalidate`, `new_seed`, `num_insns`, `req_low_cycles`, the invalidate pulse
length, the grant delay, the response delay, the ECC masks and the combo
sequence's reset timer are all reproduced from it.

Two places where the port follows upstream's constraints and the baseline did
not are both in `ibex_icache_core_back_line_seq`, which overrides `run_req`
rather than extending it and so did not get the draws `build_tb.py` moved out
of the item. `new_seed` and `num_insns` are both drawn here as the item's
`dist` constraints describe. RESULTS.md has what the baseline was doing instead
and what it cost; `build_tb.py` now draws them there too.

## Timing

The design samples on the rising edge. `co_await RisingEdge` resumes before the
design has evaluated that edge, so it yields the value `@(posedge clk)` reads
in the Active region and is where the monitors read. Driving there would be
wrong, because `set()` is immediate and the value would be captured by the edge
being awaited.

Everything that drives a pin therefore works from a **drive point**: the
instant just after a falling edge. A value written there is captured by the
next rising edge and by no earlier one, which is exactly what upstream's
`default output negedge` clocking blocks produce. Every driving task is entered
at a drive point and returns at a drive point, and none of them opens with a
wait; a task that re-anchored itself would place its first write one cycle
later than the UVM driver does. Each of them was traced against its upstream
counterpart edge by edge, and `run_tests.py`'s `cycles/item` is what would
notice if one of them were off.

## Replay

Until this was built, the two harnesses had only ever been compared on the
agreement of their per-item rates, because each draws from its own random
stream. That is a statement about two distributions. `replay.py` makes a
statement about one run: the baseline writes down everything its environment
does that the DUT can see, this port drives the same thing at the same pins,
and the DUT's outputs are compared cycle for cycle.

```sh
python3 replay.py                       # eight tests, one seed, both modes
python3 replay.py --combo --seeds 5     # all ten, five seeds
python3 replay.py ibex_icache_ecc --keep
```

**The recording is made by the baseline and replayed here**, which is the
direction that tests the port against the reference. The other direction would
test the reference against the port, and would need a UVM sequence and driver
that read a file where the recording here needs one `always @(posedge clk)`
block.

`+icache_record=<prefix>` is added to `ports/ibex_icache_uvm` by two overlays in
its `build_tb.py`, and without the plusarg neither does anything at all. It
writes three files:

| file | written by | contents |
| --- | --- | --- |
| `<prefix>.pins` | `tb.sv` | one line per posedge of `clk`: every input of the testbench wrapper and every output of it |
| `<prefix>.items` | `ibex_icache_core_driver.sv` | the core item stream, and one line per new memory seed |
| `<prefix>.seq` | `ibex_icache_core_base_seq.sv` | `base_addr` and `constrain_branches`, one line per sequence |

Everything in the pin trace is read in the Active region of the posedge, which
is the value the design samples at that edge and the value a `monitor_cb` input
sees. So a replay that drives those inputs at its drive point, half a cycle
earlier, presents the design with the same stimulus, and one that reads the
outputs at the edge itself reads what the recording read. The format is one
line per cycle:

```
# C cycle in branch_addr instr_rdata out core_rdata core_addr instr_addr
C 4123 013 76e76f7c 000061ce 0049 000061ce 76e76f7c 76e76f80
```

`in` and `out` pack the one-bit signals; the file's header names the bits. The
scrambling key and the two ECC corruption masks are wide and change rarely, so
they get `K` and `M` lines only when they change and a reader holds the last
value. The masks are recovered as `ic_*_rdata_o ^ ic_*_rdata_in`, which is
exactly what `ibex_icache_ram_if` exclusive-ored in and is zero wherever it
applied none. About 60 bytes a cycle, so 4 MB for a run of the smoke test.

Two modes read it.

**`ICACHE_REPLAY=<prefix>`** drives every input of `ibex_icache_tb_top` from
the recording and compares all twelve of its outputs at the edge the recording
read them on. Nothing here generates stimulus and the memory model is not
consulted: the responses are already on the wire. What this checks is that the
two harnesses present the same interface to the same design, which covers
`ibex_icache_tb_top.sv` against `tb.sv` and this port's drive point against the
baseline's clocking blocks. It
also runs **this port's scoreboard on the baseline's stimulus**, which is the
one thing that had never been done in either direction: `check_compatible`, the
address sequence, the busy line and the caching ratio all applied to the
reference's runs.

The recording begins at the first posedge, by which time `dut_init` has already
driven `rst_n` low, so a replay runs two idle cycles with `rst_ni` high first
and applies cycle 0 as a real falling edge. Without one, the RAMs' four-valued
`rvalid` registers stay at their zero-initialised value, `mubi4_test_true_loose`
reads that as true, and the first cycle disagrees for a reason that has nothing
to do with the stimulus.

**`ICACHE_ITEMS=<prefix>`** replays only the core item stream. Every delay below
the sequence -- the driver's waits, the grant and response timing, the key
device, the ECC masks -- is still drawn here. That holds the item distribution
fixed and lets everything else vary, which is what the `err/resp` question in
RESULTS.md needed.

### What an item replay cannot carry

`ibex_icache_core_base_seq` has exactly one feedback path from the DUT:

```systemverilog
force_branch = rsp.saw_error;
...
force_branch -> req.trans_type == ICacheCoreTransTypeBranch;
```

so the item stream is not a pure function of the sequence's random draws.
Whether a fetch errors depends on which memory seed was in force when the read
was granted, `ibex_icache_mem_resp_seq::take_gnt` drains the seed queue at the
grant, and grant timing is drawn independently by an item replay. Where a
replay sees an errored fetch that the recorded run did not, the sequence that
made the next item would have turned it into a branch.

`ibex_icache_passthru` at seed 123 found this before it was handled: two items
the recording called requests followed an errored fetch, the cache moved on by
four bytes across the errored word where the scoreboard's rule reads the opcode
bits of a zeroed one and says two, and the address check failed. The item
replay now makes the same conversion the constraint does and counts it in
`forced_branches`. The branch target has to be drawn, because a recorded
request item carries none, which is why `base_addr` is in the recording. On
that run it happens twice in 918 items.

The two stress tests are replayed at the pin level only. They change
`mem_err_shift` and the caching-ratio flag between child sequences and neither
is visible at a DUT pin, so a recording cannot drive their scoreboard; that is
stated rather than worked around.

Two counters have no counterpart under a pin replay. `key_answers` and
`key_refusals` cannot be recovered, because a refusal carries the same pins as
an idle cycle, and `ecc_injections` counts recorded mask changes rather than the
negedges on which `ibex_icache_ram_if` drew a mask.

### Showing the comparison is live

`ICACHE_REPLAY_PERTURB=N` moves the first recorded branch target at or after
cycle N by 64 bytes, which is the width of the window the constrained sequences
branch inside, so the cache is redirected somewhere it was never asked to go:

```
$ ICACHE_REPLAY_PERTURB=5000 CPPTB_TEST=ibex_icache_caching \
    ICACHE_REPLAY=build/replay/ibex_icache_caching.123 ./Vdpi_ibex_icache_cpptb
cpptb-icache replay: the branch at cycle 5729 was moved from 0x76e76f0e to
  0x76e76f4e
cpptb-icache replay divergence at cycle 5729:
  recorded out=0019 rdata=b4b7657c addr=76e76fa0 instr_addr=76e76f0c
  replayed out=0799 rdata=b4b7657c addr=76e76fa0 instr_addr=76e76f4c
```

At 1000, 5000 and 20000 it is caught on the cycle the change was made or the
one after it.

### What it found

All ten tests at ten seeds: **the DUT's outputs match the recording on every
one of 4,692,318 cycles**, and this port's scoreboard accepts all 582,812
fetches of the baseline's runs. The item replay reproduces every rate to within
a percent.

`replay.py` also uses `run_tests.py`'s own log parser rather than a second copy
of it, so the pin replay checks that parser: on all 100 runs, the 15 counters
both sides keep were read once out of the baseline's log and once off the wire,
and agree exactly. That check found one defect, introduced by this recording:
the parser matched the driver's item header on `core_driver.sv:32` and the
overlay moved that line, which reads exactly like a run with no items in it.

RESULTS.md has the tables, and has the one thing item replay found that the
rate comparison could not: the residual `err/resp` difference is the item
distribution and nothing else.

## The ten tests

Each is the virtual sequence upstream names for it in
`ibex_icache_sim_cfg.hjson`, built from the knobs that sequence's `pre_start`
sets.

| test | what it does |
| --- | --- |
| `ibex_icache_smoke` | the base sequence at its defaults |
| `ibex_icache_passthru` | branch targets in a 64-byte window, cache held disabled |
| `ibex_icache_caching` | the same window, cache held enabled, invalidation avoided |
| `ibex_icache_invalidation` | the caching sequence with invalidation allowed |
| `ibex_icache_oldval` | constrained branches, enable toggling one item in three, no invalidation, plus `check_phase` |
| `ibex_icache_back_line` | `ibex_icache_core_back_line_seq`: branch on every item, alternating between the window and up to 16 bytes back from the last target |
| `ibex_icache_many_errors` | `mem_err_shift = 1`, so about half of memory errors |
| `ibex_icache_ecc` | the caching sequence with RAM read data corrupted a way at a time |
| `ibex_icache_stress_all` | `ibex_icache_combo_vseq`: 50 to 100 transactions of one of the seven above other than `ibex_icache_oldval`, then the next, resetting between them one time in two |
| `ibex_icache_stress_all_with_reset` | the same with a reset before every child, each child stopped after 100 to 1000 cycles |

One knob in `ibex_icache_core_base_seq` is worth naming because two of the
virtual sequences set it and it does nothing.
`ibex_icache_invalidation_vseq` sets
`gap_between_seeds = 19` and `ibex_icache_passthru_vseq` sets it to 1, and at
the pinned Ibex commit **`gap_between_seeds` is declared and no constraint
reads it**, so neither has any effect on either harness. What actually
separates `ibex_icache_invalidation` from `ibex_icache_caching` is that it does
not set `avoid_invalidation`. `gap_between_invalidations` keeps its default of
49 in every sequence.

## Mid-test reset

Only the two stress tests reset the DUT after `dut_init`, and they are why the
reset paths exist.

`apply_reset` does in one place what upstream spreads over four:
`clk_rst_if::apply_reset` drives `rst_ni` low for 50 to 100 cycles;
`ibex_icache_mem_driver::reset_signals` clears the bus and flushes the pending
responses; `ibex_icache_base_vseq::reset_ifs` drops `req` so nothing escapes
the DUT while it is held; and the scoreboard's `start_reset` and `reset` hooks
fire on the two edges. It is entered and left at a drive point, so the release
is captured by the next rising edge and by no earlier one.

`wait_clks`, `wait_valid`, `read_insn`, `read_insns`, `lower_req` and
`maybe_invalidate` each carry the stop-on-reset path that the matching upstream
task forks, and each restores the pin it owns on the way out, which is what the
upstream early returns do.

There is one structural difference and it is worth being precise about.
Upstream, the core sequence and the core driver are separate processes:
`ibex_icache_combo_vseq::run_sequence` kills the *sequence* when its timer
expires, and the *driver* keeps driving the item it has in hand until the reset
arrives and its early returns unwind it. Here the two are one coroutine, so the
kill and the reset stop the same thing, through `Env::core_stopped`. The window
this collapses is bounded: between `child_seq.kill()` and `o_rst_n <= 1'b0` in
the next child's `dut_init` there is nothing but zero-time object creation and
`apply_reset`'s sub-cycle delay, so upstream's driver has at most one clock
period of item left to drive. Both harnesses reach the reset with the core pins
low and the current item abandoned.

`ICACHE_KEEP_STATE_ON_RESET=1` turns off the scoreboard's two hooks. With it
set, both stress tests fail on the first check that reads state the reset
should have cleared:

```
$ ICACHE_KEEP_STATE_ON_RESET=1 CPPTB_TEST=ibex_icache_stress_all \
    CPPTB_RANDOM_SEED=123 ./Vdpi_ibex_icache_cpptb
cpptb: testbench.cpp:556: busy is high while memory transactions are outstanding
```

That is the hooks doing work rather than merely running.

## ECC

`ibex_icache_ecc` corrupts RAM read data on the way to the cache. The cache's
SECDED spots the corruption, treats the way as a miss and fetches from memory
again, so the data reaching the core is still correct and the scoreboard checks
it exactly as it checks every other fetch. `ecc_corrupter` is
`ibex_icache_ram_if`'s two `always @(negedge clk)` blocks: on every falling edge
where every way is returning read data it redraws `tag_sel_line` and
`data_sel_line` from `dist { 0 :/ 99, [1:$] :/ 1 }` and a mask with one or two
bits set, uniform over every such mask.

Two knobs show the path is live.

`ICACHE_ECC_ERR_PCT=0` corrupts every RAM read rather than one in a hundred. On
seed 123 that takes `mem_grants` from 546 to 9080 -- the cache misses on
essentially every access and refetches -- while all 10020 fetches still return
correct data, and `ecc_errors` goes from 0 to 4213.

`ICACHE_ECC_ALIAS=1` is the ECC counterpart of `ICACHE_CORRUPT_GRANT`. The
difference between two codewords of a linear code is itself a codeword, and the
constant inversion `prim_secded_inv_39_32_enc` applies cancels, so
exclusive-oring a stored word with the codeword of a nonzero payload leaves the
syndrome at zero and changes the data. The cache cannot see it, hands the wrong
instruction to the core, and the scoreboard says so:

```
$ ICACHE_ECC_ALIAS=1 CPPTB_TEST=ibex_icache_ecc CPPTB_RANDOM_SEED=123 \
    ./Vdpi_ibex_icache_cpptb
cpptb: testbench.cpp:547: fetch at 0xc453f07c returned 0xa3cdafd9 (err 0/0),
  which is compatible with none of the 1 available seeds:
  not seed 0x00000000 (expected cmp 0x76ebbd3e; saw 0xa3cdafd9)
```

A sparse mask does not do this. Masks of 4, 6, 8, 12 and 20 bits were all
caught, with and without `ICACHE_ECC_ERR_PCT=0`; the aliasing mask is the only
one of these that gets a wrong word to the core.

## Divergences from the UVM environment

Stated plainly, because a port that quietly checks less is not a comparison.
Three of the seven this document used to list are now closed.

1. **An errored memory response drives zero rather than `'X`.** Verilator has
   no X, so the baseline drives zero too; this is a statement about both
   harnesses rather than a difference between them.
2. **No functional coverage.** Neither side collects any: the baseline compiles
   `ibex_icache_fcov_if.sv` and the agents' covergroups but does not pass
   `--coverage`. Neither has covergroups that run.
3. **No assertions.** `prim_assert.sv` gives Verilator the dummy macros, so the
   design's SVA compiles away on both sides, and the two protocol checker
   modules do nothing in the baseline. This port does not instantiate them at
   all, which is the same amount of checking and less pretence.
4. **`ibex_icache_core_protocol_checker` and `ibex_icache_mem_protocol_checker`
   are not ported.** They are SVA-only and compile to nothing under Verilator,
   so nothing is lost against the baseline, but they are real checks on a
   simulator that runs assertions.
5. **`ibex_icache_ram_if`'s SVA is not ported either, and one of its properties
   is a check neither side makes.** `DataErrChk_A` says that a way whose read
   data was corrupted must raise `ecc_error_o` in the same cycle. Verilator
   compiles it away, so the baseline does not check it. This port counts
   `ecc_error_o` and requires it to have fired at least once on a run that
   injected corruption, which is weaker than the per-cycle form: a corrupted way
   only raises the flag if the lookup reached it, and on `ibex_icache_ecc` at
   seed 123 that is 70 pulses for 85 injections. **Nothing on either side checks
   that a particular injection was reported.**
6. **The reset is 50 to 100 cycles long and begins at a drive point.**
   `clk_rst_if::apply_reset` puts a `$urandom_range(0, clk_period_ps)` delay in
   front of the assertion, which has no counterpart here: this port has no
   sub-cycle drive point. The width is drawn from the same range.
7. **The environment's heartbeat is not ported.** `ibex_icache_env` registers
   both sequencers with a `uvm_heartbeat` and triggers it every 2000 clocks, so
   a run where neither sequencer sends a request for 2000 clocks fails. The
   nearest thing here is the 100 ms simulation timeout, about 5,000,000 cycles,
   and cpptb's wait-graph diagnostic when every coroutine is blocked. That is a
   far looser hang detector, and it is the one place where a *hang* would be
   caught later here than in the baseline.

Two things that were divergences are not any more.

* **The scrambling key device.** `key_device` now randomises the whole 194-bit
  `d_data` per transaction, as the `push_pull_agent` does, so the bottom bit is
  the valid bit and about half of every answer refuses the request and makes the
  cache ask again. The delay is `device_delay inside {[0:device_delay_max]}`
  with `device_delay_max` drawn once for the run from the agent config's five
  buckets, and `zero_delays` takes it out entirely three times in ten. Measured
  on `ibex_icache_smoke` at seed 123: 29 answers and 35 refusals here, 25 and 25
  in the baseline. One thing about it is not reproduced and cannot be: the
  baseline's per-request delay is `inside {[0:max]}` and a constrained
  `randomize()` over an `inside` range is not uniform on this simulator, so its
  delays cluster at the top of the range. See RESULTS.md.
* **The chatty second pass.** A `check_compatible` failure now says why each
  seed did not match, in upstream's words, rather than only how many were
  tried. Upstream makes the whole search a second time with a flag; this asks
  the same predicates for their reason, which is the same information without a
  second traversal, capped at 24 lines because the seed list is unbounded.

Nothing else in the scoreboard is missing.

## What a failure reports

`ICACHE_CORRUPT_GRANT=N` flips one bit of the memory response for the Nth
granted request, which is how the checking above was shown to be live rather
than merely quiet:

```
$ ICACHE_CORRUPT_GRANT=1 CPPTB_TEST=ibex_icache_caching CPPTB_RANDOM_SEED=123 \
    ./Vdpi_ibex_icache_cpptb
cpptb: testbench.cpp:547: fetch at 0xc453f08a returned 0x000061cc (err 0/1),
  which is compatible with none of the 1 available seeds:
  not seed 0x00000000 (expected cmp 0x000061ce; saw 0x000061cc)
```

Two lines, before a debugger: the source location of the check, the fetch
address, what came back, both error flags, how many seeds were tried and what
each of them expected instead. Over grants 1 to 450 of that run, **113 are
caught and 337 are silently correct**; the loudest is caught 296 separate times,
because the corrupted line stayed in the cache and was returned by every later
hit. A corruption that is not caught is one whose fill buffer was discarded
before the word reached the core, which is the cache behaving properly.

The other three knobs are in the sections above: `ICACHE_KEEP_STATE_ON_RESET`
for the reset hooks, `ICACHE_ECC_ALIAS` and `ICACHE_ECC_ERR_PCT` for ECC, and

```
$ ICACHE_CHECK_OLDVAL=1 CPPTB_TEST=ibex_icache_caching CPPTB_RANDOM_SEED=123 \
    ./Vdpi_ibex_icache_cpptb
cpptb: testbench.cpp:1694: an oldval run saw at least 1000 fetches where an old
  value was possible, and saw 0
```

which applies `ibex_icache_oldval_test::check_phase` to a sequence that
invalidates nothing and never disables the cache, so it has no old values to
return. That is the check finding what it is for.

## Files

```
ibex_icache_cpptb.core       CAPI=2 description of the RTL half
ibex_icache_tb_top.sv        tb.sv without the UVM
fusesoc_setup.py             resolves the graph, writes and checks cpptb.toml
cpptb.toml                   generated; do not edit
testbench.cpp                agents, memory model, scoreboard, ten tests
run_tests.py                 runs the port, and the baseline beside it
replay.py                    records the baseline's stimulus and replays it
shims/                       one reduced Verilator case, see RESULTS.md
```
