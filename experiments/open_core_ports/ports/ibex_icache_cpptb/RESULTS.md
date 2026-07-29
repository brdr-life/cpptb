# ibex_icache: cpptb against the UVM baseline

Both harnesses drive the same `ibex_icache`, elaborated from the same 77 RTL
sources at the same pinned Ibex commit under Verilator 5.050. The port is
[README.md](README.md); the baseline is
[`ports/ibex_icache_uvm`](../ibex_icache_uvm/README.md).

**All ten of upstream's tests, ten seeds each, on both harnesses: 200 of 200
runs pass.** A further 400 cpptb runs over 40 seeds also pass. Nothing either
scoreboard checks failed on either side.

**The two harnesses can now be given the same stimulus, and 180 of 180 replays
pass.** `replay.py` records what the baseline's environment does and drives the
same thing here, and under it the DUT's outputs match cycle for cycle over
4,699,689 cycles of all ten tests at ten seeds. That is "The same stimulus on
both harnesses" below. It settles the `err/resp` difference the rate comparison
could not, and it settles the `grants/fetch` difference the previous version of
this document could not.

## What is comparable and what is not

Without a recording the two harnesses cannot be given the same stimulus. Each
draws from its own random stream, the same seed means nothing across them, and
before `replay.py` there was no transaction log to replay. What is comparable
that way is the generator: both draw the same fields from the same buckets at
the same weights, so **per-item rates** should agree. Totals scale with
`num_trans`, which is drawn independently on each side and lands anywhere in
800 to 1000.

`run_tests.py --compare` reports both. The UVM numbers are counted out of a
UVM_HIGH log, which is the only place that environment reports per-transaction
activity. The pin replay checks that reading, and `replay.py` uses that same
parser rather than a second copy of it: on 100 runs, every one of the 15
counters both sides keep was read once out of the log and once off the wire
while the port replayed the same run, and **they agree exactly, every time**.
Those 15 are `items`, `branch_items`, `insns_requested`, `fetches`,
`fetch_errors`, `branches`, `invalidations`, `new_seeds`, `mem_grants`,
`mem_responses`, `mem_response_errors`, `windows_completed`, `windows_checked`,
`resets` and `cycles`. None of the differences below is a measurement artefact.

Getting that check to pass found one defect in the parser, introduced by the
recording itself: `read_uvm_log` matched the driver's item header on
`core_driver.sv:32`, and the recording overlay moved that line. A header that
stops matching reads exactly like a run with no items in it, so the parser now
matches on the file rather than the line and refuses a log in which any of six
counters is zero.

## The comparison

Rates, as means over seeds 123 to 132.

| test | insns/item | fetches/insn | grants/fetch | err/resp | cycles/item |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ibex_icache_smoke` | 10.31 / 12.46 | 0.928 / 0.924 | 0.847 / 0.811 | 0.037 / 0.034 | 75.4 / 84.1 |
| `ibex_icache_passthru` | 10.26 / 12.44 | 0.860 / 0.875 | 0.847 / 0.805 | 0.061 / 0.050 | 73.0 / 81.7 |
| `ibex_icache_caching` | 10.21 / 12.85 | 0.481 / 0.564 | 2.593 / 1.922 | 0.582 / 0.485 | 44.3 / 47.8 |
| `ibex_icache_invalidation` | 10.05 / 12.59 | 0.918 / 0.855 | 0.251 / 0.264 | 0.146 / 0.235 | 59.6 / 61.9 |
| `ibex_icache_oldval` | 10.14 / 12.55 | 0.862 / 0.868 | 0.814 / 0.765 | 0.064 / 0.054 | 69.6 / 79.1 |
| `ibex_icache_back_line` | 1.504 / 1.476 | 0.765 / 0.816 | 2.291 / 2.030 | 0.572 / 0.471 | 11.1 / 9.99 |
| `ibex_icache_many_errors` | 10.48 / 12.44 | 0.624 / 0.576 | 0.771 / 0.742 | 0.318 / 0.332 | 53.5 / 56.1 |
| `ibex_icache_ecc` | 10.21 / 12.27 | 0.481 / 0.566 | 2.599 / 1.940 | 0.579 / 0.483 | 44.5 / 47.2 |
| `ibex_icache_stress_all` | 8.817 / 10.43 | 0.805 / 0.813 | 0.604 / 0.550 | 0.174 / 0.188 | 54.4 / 55.2 |
| `ibex_icache_stress_all_with_reset` | 6.854 / 9.292 | 0.683 / 0.618 | 0.900 / 0.929 | 0.197 / 0.240 | 35.4 / 41.7 |

cpptb first, the UVM baseline second. Every one of the ten `insns/item` is more
than 17% apart and that is one defect in the reference, chased below. Of the
thirty rates below it, **six are more than 20% apart**: `grants/fetch` on
`ibex_icache_caching` and `ibex_icache_ecc`, and `err/resp` on
`ibex_icache_passthru`, `ibex_icache_invalidation`, `ibex_icache_back_line` and
`ibex_icache_stress_all_with_reset`. The next three sections chase each to its
cause; the first two and `back_line`'s are one mode mix, and the rest are a
sampling effect.

The counters that are not rates:

| | cpptb | UVM | |
| --- | ---: | ---: | --- |
| child sequences, `stress_all` | 12.3 | 11.7 | |
| resets, `stress_all` | 7.5 | 6.0 | one per child, half the time |
| child sequences, `stress_all_with_reset` | 12.5 | 12.6 | |
| resets, `stress_all_with_reset` | 12.5 | 12.6 | one per child, always |
| key answers / refusals, pooled over all ten tests | 2294 / 2409 | 2194 / 2280 | |
| answered fraction | 0.488 | 0.490 | the valid bit is one random bit |
| invalidations, `ibex_icache_invalidation` | 17.3 | 17.0 | |
| new seeds, `ibex_icache_invalidation` | 17.3 | 17.0 | one per invalidation |

## The same stimulus on both harnesses

`replay.py` runs the baseline with `+icache_record`, which writes every value
its environment puts on a DUT pin and every value the DUT answers with, one
line per posedge, plus the core item stream. The port then replays it. The
mechanism is in [README.md](README.md); what it found is here.

**Pin replay: every input of the testbench wrapper driven from the recording
and every output of it compared at the edge the recording read it on. All ten
tests, ten seeds, 4,699,689 cycles, and every output matches on every cycle.**
No divergence anywhere, so the two harnesses present the same interface to the
same design. That covers `ibex_icache_tb_top.sv` against `tb.sv`, this port's
drive point against the baseline's clocking blocks, and the RAM and key wiring
around the DUT.

The twelve outputs compared are `valid_o`, `rdata_o`, `addr_o`, `err_o`,
`err_plus2_o`, `busy_o`, `instr_req_o`, `instr_addr_o`, `scr_key_req_o`,
`ic_tag_rvalid_o`, `ic_data_rvalid_o` and `ecc_error_o`, which is every pin at
the wrapper's boundary. The DUT's RAM request, address and write-data lines are
inside the wrapper and are not compared directly; a difference in them would
have to reach the boundary through the RAM read data to be seen.

That is not a vacuous pass. `ICACHE_REPLAY_PERTURB=N` moves the first recorded
branch target at or after cycle N by 64 bytes, which is the width of the window
the constrained sequences branch inside:

```
$ ICACHE_REPLAY_PERTURB=5000 CPPTB_TEST=ibex_icache_caching \
    ICACHE_REPLAY=build/replay/ibex_icache_caching.123 ./Vdpi_ibex_icache_cpptb
cpptb-icache replay: the branch at cycle 5729 was moved from 0x76e76f0e to
  0x76e76f4e
cpptb: testbench.cpp:1934: the DUT's outputs match the recording on every
  cycle, and they first differ at cycle 5729
```

At 1000, 5000 and 20000 the change is caught on the cycle it was made or the
one after it.

The pin replay also runs **this port's scoreboard on the baseline's stimulus**,
which had never been done in either direction. Over the eight non-combo tests
at ten seeds it accepted 581,244 fetches: `check_compatible` against every seed
the baseline announced, the address sequence, the busy line and the caching
ratio. Given that `ports/ibex_icache_uvm`'s own overlay was recently found to
have weakened what the baseline checked, an independent checker over the
baseline's runs is worth having, and it found nothing.

## What the same stimulus says about the rates

The item replay is the second half of it: only the core item stream comes from
the recording, and every delay below the sequence is still drawn here. So
`insns/item` is pinned to the baseline's and everything else is free.

One run of the port against one recording is a noisy way to read that. A
recording costs about twenty seconds of the baseline and a replay costs a fifth
of a second, so the port is run **twenty-five times against each recording, on
twenty-five different seeds**, and its mean is what the recording's own numbers
are compared with. That takes the port's own sub-item draws out of the
comparison and leaves the recording's. On `ibex_icache_many_errors` the port's
spread over seeds against one recording is 1.2% of `grants/fetch` and the
spread of the comparison across recordings is 1.1%, so averaging the port away
is worth a factor of 2.3 in recordings and costs nothing. `build/sweep.py` in
this directory is the harness; it is scratch, not part of the port.

Paired per-recording ratios, port over baseline, mean and standard error over
the recordings named. `ibex_icache_many_errors` gets sixty because it is the
test the previous version of this document could not settle.

| test | recordings | `fetches/insn` | `grants/fetch` | `grants/insn` | `err/resp` |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ibex_icache_smoke` | 10 | 1.0007 ± 0.0005 | 1.0007 ± 0.0014 | 1.0013 ± 0.0016 | 1.014 ± 0.012 |
| `ibex_icache_passthru` | 10 | 1.0004 ± 0.0006 | 0.9999 ± 0.0008 | 1.0003 ± 0.0007 | 1.014 ± 0.017 |
| `ibex_icache_caching` | 10 | 1.0029 ± 0.0020 | 0.9959 ± 0.0038 | 0.9988 ± 0.0038 | 1.001 ± 0.001 |
| `ibex_icache_invalidation` | 10 | 1.0001 ± 0.0001 | 1.0212 ± 0.0107 | 1.0213 ± 0.0107 | 0.985 ± 0.029 |
| `ibex_icache_oldval` | 10 | 1.0003 ± 0.0005 | 0.9989 ± 0.0032 | 0.9992 ± 0.0035 | 0.995 ± 0.017 |
| `ibex_icache_back_line` | 10 | 1.0053 ± 0.0024 | 0.9961 ± 0.0058 | 1.0013 ± 0.0051 | 1.003 ± 0.002 |
| `ibex_icache_many_errors` | 60 | 1.0004 ± 0.0003 | 1.0041 ± 0.0014 | 1.0045 ± 0.0014 | 1.001 ± 0.006 |
| `ibex_icache_ecc` | 10 | 1.0035 ± 0.0023 | 0.9951 ± 0.0041 | 0.9986 ± 0.0040 | 1.001 ± 0.001 |

`insns/item` is 1.0000 on every row by construction. `cycles/item` is within
two standard errors of unity on seven of the eight; `ibex_icache_back_line` is
0.9651 ± 0.0062, and its items are ten cycles long, so that is a fifth of a
cycle per item.

Inverse-variance weighted over the eight:

| rate | ratio | from 1 |
| --- | ---: | ---: |
| `insns/item` | 1.0000 | exact, by construction |
| `fetches/insn` | 1.00020 ± 0.00008 | 2.5 standard errors |
| `grants/fetch` | 1.00059 ± 0.00059 | 1.0 standard errors |
| `grants/insn` | 1.00112 ± 0.00056 | 2.0 standard errors |

**The residual `err/resp` difference is the item distribution and nothing
else.** With the items held fixed, every one of the eight is within one
standard error of unity, where the previous measurement of `passthru`, `oldval`
and `invalidation` on the port's own stimulus had them 2.4, 2.4 and 1.6
standard errors apart. The mechanism the earlier version of this document
guessed at is confirmed by the size of it: an errored fetch ends the
transaction, so an erroring seed is replaced after fewer memory reads than a
clean one, and relatively sooner on the harness whose transactions are shorter.
On `ibex_icache_passthru` the port's own `err/resp` is 1.250 times the
baseline's and the baseline's `insns/item` is 1.212 times the port's; on
`ibex_icache_oldval` it is 1.289 against 1.248. That is the whole of it, and its
cause is the Verilator `inside`-range defect in the section below, which is a
defect in the reference.

`ibex_icache_caching`, `ibex_icache_ecc` and `ibex_icache_back_line` come back
to unity for a different reason: those are the seed-0 tests, whose whole
behaviour turns on one draw of `base_addr`, and the item replay takes
`base_addr` from the recording. Their `own` ratio of 0.802 ± 0.201 is that
bimodality and not a difference between the harnesses.

## Where `grants/fetch` went

The previous version of this document reported `grants/fetch` under item replay
as 0.7% low pooled and **2.2% low on `ibex_icache_many_errors`, two standard
errors of its own**, and left it unexplained. It is chased here.

### Everything drawn below the item stream

An item replay fixes the item stream and lets everything under it be drawn
again on each side. This is all of it.

| drawn | baseline draws it with | port draws it with | differ |
| --- | --- | --- | --- |
| core driver, ready across a branch | `$urandom_range(16) == 0` | the same | no |
| core driver, wait for valid first | `$urandom_range(9) == 0` | the same | no |
| core driver, cycles before ready | `$urandom_range(3)` | the same | no |
| core driver, `req_low_cycles` | four buckets, `$urandom_range` per the overlay | the same buckets | no |
| core driver, invalidate pulse | `1 :/ 10, [2:20] :/ 1`, `$urandom_range` per the overlay | the same | no |
| memory grant delay | three buckets, `$urandom_range` per the overlay | the same | no |
| memory response delay | three buckets, `$urandom_range` in `pre_randomize` per the overlay | the same | no |
| memory error and read data | a pure function of the seed and the address | the same function | no |
| memory seed in force at a grant | the seed queue drained at the grant | the same | no |
| key device, `zero_delays` | `dist { 0 := 7, 1 := 3 }` in a class constraint | 3 in 10 | no |
| key device, `device_delay_max` | five buckets in a class constraint | the same buckets | no |
| key device, per-request delay | **`device_delay inside {[0:max]}`** | **uniform on `[0:max]`** | **yes** |
| key device, the answer's valid bit | bit 0 of a 194-bit random `d_data` | one random bit | no |
| ECC way select and mask | `$urandom_range` per the overlay | the same | no |
| reset width | `$urandom_range(50, 100)` | the same | no |

Every `dist` in that list is drawn with `$urandom_range` on both sides, because
`ports/ibex_icache_uvm` already had to replace them: Verilator ignores `dist`
weights under `std::randomize`. Checking each one against its overlay is where
the enumeration was spent, and exactly one entry came back different.

### The one that differed, and what it was worth

`push_pull_base_seq::randomize_item` asks for `device_delay inside
{[cfg.device_delay_min : cfg.device_delay_max]}` through a constrained
`randomize()`, and on this simulator that honours the range and is not uniform
over it. Read off the baseline's own logs before the fix, at
`device_delay_max` 131 over 37 draws the mean was 94 where the range's mean is
65; pooled over twenty runs the key device waited 1.18 times as long as the
same range drawn uniformly.

Its sign is the one the previous version could not work out. While the cache
waits for a scrambling key it is in `AWAIT_SCRAMBLE_KEY`, where
`inval_block_cache` gates both `lookup_actual_ic0` and `fill_cache_new`, so
every fetch in that window goes to memory and nothing is allocated. **A longer
key wait means more of the run runs uncached, so more memory reads per fetch.**
The baseline's delays were the longer ones, so the baseline's `grants/fetch`
was the higher one and the port read low. The sign is right.

The size is not. `ICACHE_KEY_DELAY_SCALE` multiplies the port's drawn delay and
`ICACHE_KEY_DELAY_MAX` replaces `device_delay_max`, so the question is a
measurement over the same twenty recordings of the unfixed baseline:

| the port's key delay | pooled `grants/fetch`, `many_errors` |
| --- | ---: |
| forced to zero | 0.7403 |
| `device_delay_max` forced to 50 | 0.7438 |
| as drawn | 0.7478 |
| 1.2 times as drawn, which is the defect | 0.7488 |
| `device_delay_max` forced to 200 | 0.7541 |
| `device_delay_max` forced to 1000 | 0.7926 |

**The defect is worth 0.14% of `grants/fetch` on `ibex_icache_many_errors`**,
not 2.2%. Measured the same way on each test it is 0.91% on
`ibex_icache_invalidation`, 0.10% on `ibex_icache_oldval`, 0.03% on
`ibex_icache_smoke` and **exactly zero** on `ibex_icache_passthru`,
`ibex_icache_caching`, `ibex_icache_back_line` and `ibex_icache_ecc`: the last
three never invalidate, so they ask for a key once out of reset and never
again, and `ibex_icache_passthru` holds the cache disabled, where blocking it
changes nothing. Averaged over the eight it is 0.14%, and weighted by how
precisely each is measured, 0.03%.

`ports/ibex_icache_uvm` now draws that delay with `$urandom_range`, which is
what it already did for every other distribution Verilator gets wrong, and the
baseline's mean delay at `device_delay_max` 131 went from 94 to 69 against a
uniform 65. Every table above is measured against that baseline. It is the same
`inside`-range defect the `num_insns` section below records, at a second
site.

### What the rest of it was

`grants/fetch` is `(grants/insn) / (fetches/insn)` identically, and the two
columns in the table above differ by exactly that. Measured against the
baseline before its key delay was fixed, `grants/insn` -- memory grants per
instruction the recorded item stream asked for -- was 0.99945 ± 0.00069
weighted over the eight, 0.8 standard errors from unity, while `grants/fetch`
was 0.99857 ± 0.00066, 2.2 standard errors from it. **Nearly all of what the
pooled `grants/fetch` was short was its denominator**: `fetches/insn` is
1.00020 ± 0.00008 and that carries straight through.

### `ibex_icache_many_errors`, and why it looked worst

It did not replicate. Three readings of the same quantity:

| how it was measured | `grants/fetch`, port over baseline |
| --- | ---: |
| 10 recordings, one port seed each, old baseline | 0.978 ± 0.011 |
| 20 recordings, 25 port seeds each, old baseline | 0.998 ± 0.005 |
| 60 recordings, 25 port seeds each, key delay fixed | 1.004 ± 0.001 |

The sign changed. Against the unfixed baseline a single paired ratio on this
test has a standard deviation of about 3%, so ten of them give a standard error
of about 0.9% and a reading 2.2% away is 2.4 standard errors. The item-replay table
compares eight tests on four free rates, which is 32 numbers, and the chance
that at least one of 32 lands two standard errors out when nothing is wrong is
0.77. **The 2.2% was that.**

To resolve a real effect of 0.7% on this test at two standard errors takes 73
recordings at one port seed each, and 144 for 80% power at the 5% level. Ten
were used. What made `ibex_icache_many_errors` the worst of the eight is that
it has the widest spread: its memory error range covers half the address space,
so each of its 30-odd memory seeds makes almost every read in the branch window
error or almost none, and `grants/fetch` is an average over that many coin
flips rather than over hundreds of independent reads. Its per-recording spread
is 20% where `ibex_icache_smoke`'s is 1.2%.

### What is left

`grants/insn` is 1.00112 ± 0.00056 weighted over the eight. It is flat on the
six tests that barely invalidate -- 1.00037 ± 0.00061 pooled over those six --
and it is above unity on the two that invalidate with the cache enabled:
**1.0045 ± 0.0014 on `ibex_icache_many_errors` and 1.021 ± 0.011 on
`ibex_icache_invalidation`.** The port makes about 21 more memory grants per
run than the baseline on the first of those, out of 4830.

That is the same shape as the key device's sensitivity, and it is not the key
device. `push_pull_agent_cfg` is randomised once per run, so its delay budget is
one heavy-tailed draw against the port's average over twenty-five seeds;
`--match-key-cfg` reads `zero_delays` and `device_delay_max` out of each
recording's own log and forces them on the port. That is worth 0.3%, from
1.0078 ± 0.0025 to 1.0049 ± 0.0021 over the same thirty recordings, and the
sixty-recording figure above already has it. What it does not do is close the
gap: on the 22 of those 60 where the baseline drew `zero_delays`, so that the
key device answers in one cycle on both sides and can account for nothing at
all, it is still 1.0039 ± 0.0018. Also ruled out: the item replay's forced branches
(runs that forced none show the same 1.0056), the number of key request
episodes (26.82 against the baseline's 26.75 once the config is matched), a
poll cycle in `push_pull_driver::get_and_drive` (worth under 0.01%), the
invalidate pulse width (91.4% one cycle against an intended 90.9%, the rest
uniform on [2:20] with mean 10.7), and `cycles/item`, which is 0.999 ± 0.010.

So `ibex_icache_invalidation` and `ibex_icache_many_errors` make about half a
percent more memory reads per instruction than the baseline does on the same
items, and the two tests that do it are the two that spend the most of the run
with the cache blocked by an invalidation. That is a fifth of what this section
set out to explain, in the opposite direction, measured against sixty
recordings rather than ten. It is stated rather than explained.

## The one systematic difference, and what causes it

`insns/item` is 17% to 21% lower on cpptb on the eight tests that run the base
sequence to completion, and 26% lower on `ibex_icache_stress_all_with_reset`
where the children are cut short. This is the same defect the three-test version
of this document found. `fetches/insn` -- how much of what was asked for the
cache actually delivered -- agrees to within 10% on seven of the ten, and the
three that do not are the three whose mode mix differs; that is the rate that
would move if a driver were mistimed.

`num_insns` is drawn from `dist { 0 :/ 5, [1:20] :/ 20, [21:100] :/ 1 }` for a
branch transaction. `ports/ibex_icache_uvm` already draws the *bucket* directly
because Verilator ignores `dist` weights, and then applies it as

```systemverilog
soft num_insns inside {[d_insn_lo:d_insn_hi]};
```

against the item's own hard `num_insns inside {[0:100]}`. The bucket weights
come out right. The value inside the bucket does not. The reduced case is
`shims/verilator_inside_range_uniformity.sv`, three ways of picking a number in
`[1:20]`, 4000 draws each:

```
hard inside [1:20]            q1=744 q2=631 q3=602 q4=2023  mean=13.18 (uniform 10.50)
soft inside [1:20] of [0:100] q1=778 q2=645 q3=648 q4=1929  mean=12.97 (uniform 10.50)
$urandom_range(20, 1)         q1=990 q2=981 q3=1023 q4=1006  mean=10.58 (uniform 10.50)
```

**A constrained `randomize()` over an `inside` range is not uniform over that
range**, whether the range is a hard constraint or a soft one. This is a sixth
Verilator randomization defect on top of the five `ports/ibex_icache_uvm`
documents, and the same defect reaches the scrambling key device, where "Where
`grants/fetch` went" above measures it. The cpptb port draws the range with a
uniform generator and gets the distribution the constraint describes; the
baseline gets a skew, and so runs *more* stimulus than it intends, not less.

`ibex_icache_back_line` is the control. Its `num_insns` is drawn rather than
solved on both sides now (see below), and its `insns/item` is 1.504 against
1.476, which is 2% apart.

## The seed-0 tests, and the mode mix

`ibex_icache_caching`, `ibex_icache_ecc` and `ibex_icache_back_line` are where
`grants/fetch` and `err/resp` are furthest apart, and all three are the same
thing: those are the only
tests that take **no new memory seeds at all**, so the whole run uses seed 0,
and the memory model makes seed 0 error over `[0xdeadbeef, 0xfeadbeef)` after
`address ^ 0xf00dbeef`. Whether that covers the 64-byte window the branches are
confined to depends only on `base_addr`, and `0 ^ 0xf00dbeef` is inside the
error range, so about half of all seeds put the window inside it. When they do,
every access errors, the cache never gets to cache anything, and `grants/fetch`
goes from about 0.05 to about 3.

Over the ten seeds here, cpptb drew the errored mode on 6 and the baseline on 5.
That is the whole of the difference. Split by mode:

| test | clean seeds, grants/fetch | errored seeds, grants/fetch |
| --- | ---: | ---: |
| `ibex_icache_caching` | 0.0569 / 0.0463 | 4.284 / 3.797 |
| `ibex_icache_ecc` | 0.0691 / 0.0623 | 4.286 / 3.818 |
| `ibex_icache_back_line` | 0.3569 / 0.3613 | 3.581 / 3.699 |

`ibex_icache_back_line` agrees to 1% once the mode is held fixed, and so does
its `cycles/item`: 5.423 against 5.623 on the clean seeds, where the pooled
figure was 11.1 against 9.99. The residual 20% on `caching` and `ecc` in the
clean mode is `insns/item` again -- the working set is a fixed 17 words, so the
grant count is fixed and `grants/fetch` moves inversely with the number of
fetches: 12.85/10.21 is 1.26 and 0.0569/0.0463 is 1.23. In the errored mode
they are 13% apart, which is the same `insns/item` skew running the other way:
nothing is cached, so the grants follow the reads and the reads follow the
fetches.

This is a property of the environment, not of either harness. It means
`ibex_icache_caching` **checks the caching ratio on about 40% of seeds and on
the rest checks only that errored fetches are reported correctly.** Over the ten
seeds, cpptb completed 39 ratio windows and the baseline 62, all on the clean
seeds, and all passed. Upstream's `reseed` of 50 hides this; one seed does not.

## Everything else that differs, and why

* **`err/resp`, 23% on `ibex_icache_passthru`, 18% on `ibex_icache_oldval` and
  38% on `ibex_icache_invalidation`.** Whether a memory read errors depends on
  the seed in force and the address, and all three of these confine their branch
  targets to a 64-byte window, so for a given seed almost every read in the
  window errors or almost none does. `err/resp` is then an average over a
  handful of coin flips weighted by how much traffic each seed saw, and the
  per-seed spread is wide. The two harnesses' distributions overlap on all
  three:

  | test | cpptb, per seed | UVM, per seed |
  | --- | --- | --- |
  | `passthru` | .043 .046 .050 .055 .059 .060 .064 .073 .074 .089 | .041 .044 .045 .046 .048 .049 .054 .055 .056 .058 |
  | `oldval` | .044 .046 .057 .059 .059 .065 .070 .073 .079 .083 | .037 .042 .043 .047 .053 .058 .059 .063 .063 .075 |
  | `invalidation` | .000 .000 .076 .078 .096 .138 .168 .195 .330 .381 | .082 .106 .130 .160 .188 .244 .308 .359 .368 .400 |

  The sign is not the same on all three: cpptb is higher on `passthru` and
  `oldval` and lower on `invalidation`. `invalidation` takes one new seed per
  invalidation and 17 of them per run, against several hundred for the other
  two, so its spread is the widest and its difference of means is 1.5 standard
  errors. `passthru` and `oldval` are 2.4 and 1.6 standard errors apart, which
  is more than noise comfortably explains for the first of them.

  **The item replay settles it.** Given the baseline's own item stream, the
  port's `err/resp` lands on the baseline's on all three, and the residual is
  the `insns/item` skew below. The mechanism is the one guessed at here: an
  errored fetch ends the transaction, so an erroring seed is replaced after
  fewer memory reads than a clean one, and relatively sooner on the harness
  whose transactions are shorter. The numbers are in "What the same stimulus
  says about the rates" above.
* **`insns/item` on `ibex_icache_stress_all_with_reset`, 26% rather than the
  usual 20%.** Every child is stopped after 100 to 1000 cycles, so a larger
  share of the items that run are the first items of a child, which are forced
  branches and therefore draw `num_insns` from the branch distribution rather
  than the shorter request one. The skew above applies to more of the run.
  `cycles/item`, 35.4 against 41.7, follows it.
* **`grants/fetch`, 4% to 10% on the tests that are not bimodal.** A fetch
  discarded because a branch redirected the cache still cost a memory read, so
  shorter runs between branches waste more reads per fetch. cpptb runs about 10
  instructions between transactions where the baseline runs about 12.5, from the
  defect above, so it branches more often per fetch. The direction and the
  magnitude both follow. Under the item replay it comes back to 1.0006 ± 0.0006
  weighted over the eight tests, so essentially all of it is that defect. What
  is left is in "Where `grants/fetch` went" above.

None of these needed a fix in the port, and all of them were chased to a cause
before being reported.

## One thing the comparison found in the baseline

`ibex_icache_core_back_line_seq` overrides `run_req` rather than extending it,
and `ports/ibex_icache_uvm`'s overlay had moved two of the item's `dist`
constraints into the *base* sequence's `run_req` because Verilator cannot solve
them. `ibex_icache_back_line` was the one test that did not get them.

Measured on the baseline at seed 123 before the fix: **all 918 items drew a
nonzero `new_seed`**, where `c_new_seed_dist` gives a nonzero seed weight zero
for an item with the cache enabled and no invalidation -- which every
`back_line` item is. The scoreboard ended the run holding 919 memory seeds, with
nothing ever truncating the list because the test never invalidates, and a fetch
counted as correct if it matched any of them. `num_insns` went the same way:
uniform over `[0:5]` instead of half of it zero.

`build_tb.py` now draws both in `back_line`'s own `run_req`, with the same
weights the item's constraints carry. `ibex_icache_back_line` still passes, and
the two harnesses now agree on it to within 2% on every rate once the seed-0
mode is held fixed.

The cpptb port implements upstream's constraints, so it never had this.

## Speed

Not the point of this port, and recorded because it is free. Means over ten
seeds, wall seconds per run.

| test | cpptb | UVM baseline |
| --- | ---: | ---: |
| `ibex_icache_smoke` | 0.18 | 23.55 |
| `ibex_icache_passthru` | 0.18 | 28.81 |
| `ibex_icache_caching` | 0.11 | 16.52 |
| `ibex_icache_invalidation` | 0.14 | 16.25 |
| `ibex_icache_oldval` | 0.17 | 22.64 |
| `ibex_icache_back_line` | 0.04 | 12.09 |
| `ibex_icache_many_errors` | 0.13 | 15.57 |
| `ibex_icache_ecc` | 0.10 | 13.61 |
| `ibex_icache_stress_all` | 0.13 | 14.27 |
| `ibex_icache_stress_all_with_reset` | 0.03 | 5.27 |

The baseline figures are UVM_HIGH runs, which is what the comparison needs and
is not how it would normally be run; logging costs about 5% of it. What makes it
slow is that the baseline is a 47 MB binary carrying the whole of UVM, that
every constrained `randomize()` is a pipe round trip to `z3`, and that it
simulates about 40 microseconds of model time per wall second where this port
does about 6 milliseconds.

`ibex_icache_ecc` is the one where that would have been much worse.
`ibex_icache_ram_if` asks the solver for a `$countones(mask) inside {[1:2]}`
over a 28-bit tag or a 78-bit line, per way, on every negedge where all the ways
are returning data; measured, that was four `z3` round trips per cycle and the
test had not finished after 18 minutes. `ports/ibex_icache_uvm` already draws
that mask directly, which is why it is 13.6 seconds here. The cpptb port draws
the same distribution the same way, in C++.

Two things to keep in mind before reading anything into the ratio. The two runs
are not the same amount of simulation: the baseline runs about 6% more cycles
per test, for the reason above. And the baseline elaborates `tb.sv` with the UVM
environment, its covergroups and its two protocol-checker modules bound in,
where this elaborates the design and its RAMs. It is a comparison of two
harnesses, not of two simulators.

The whole cpptb side of this document -- 10 tests, 10 seeds -- takes 12
seconds. The UVM side is 28 minutes of CPU, 4 minutes on eight jobs, and writes
2.5 GB of logs.

## ECC, and what neither side checks

`ibex_icache_ecc` corrupts one or two bits of a way's RAM read data one time in
a hundred. Over ten seeds the port injected 62 corruptions per run and saw
`ecc_error_o` fire 42 times, and every one of the 4268 fetches per run returned
data the scoreboard accepted: the cache spots the corruption, treats the way as
a miss and refetches. The two stress tests reach the ECC child on 7 and 8 of
ten seeds, and once they do the corruption stays on for the rest of the run,
which is what upstream does -- `ibex_icache_ecc_vseq::pre_start` sets
`enable_ecc_errors` and nothing clears it.

The port requires `ecc_error_o` to have fired at least once on any run that
injected corruption. That is weaker than `ibex_icache_ram_if`'s `DataErrChk_A`,
which requires it per way and per cycle, and the baseline does not check it at
all because Verilator compiles the SVA away. **Nothing on either side checks
that a particular injection was reported.** See README.md.

What the port can show is that the check around it is live.
`ICACHE_ECC_ALIAS=1` corrupts a line with the difference between two SECDED
codewords, which the cache cannot see; the wrong instruction reaches the core
and the scoreboard reports it. `ICACHE_ECC_ERR_PCT=0` corrupts every RAM read
and takes `mem_grants` from 546 to 9080 on seed 123 with all 10020 fetches still
correct.

## What this does not cover

* **The caching ratio is checked on about 40% of seeds**, as above, and
  `ibex_icache_smoke`, `ibex_icache_passthru`, `ibex_icache_oldval` and
  `ibex_icache_ecc` complete zero ratio windows on both harnesses by
  construction. They prove that thousands of fetches returned correct data and
  prove nothing about caching.
* **Assertions and functional coverage run on neither side**, including the two
  protocol checkers and `ibex_icache_ram_if`'s properties.
* **The heartbeat is not ported.** A run where neither sequencer sends a request
  for 2000 clocks fails in the baseline; here it would run to the 100 ms
  simulation timeout. It is the one place a hang would be caught later.
* **Ten seeds per test is not a statement about seed sensitivity.** Upstream's
  `reseed` is 50. Ten seeds are used for the comparison here and 40 for the
  cpptb-only pass rate.
* **The two stress tests are replayed at the pin level only.** They change
  `mem_err_shift` and the caching-ratio flag between child sequences and
  neither is visible at a DUT pin, so a recording cannot drive their
  scoreboard. Their DUT outputs are compared cycle for cycle like every other
  test's; their `check_compatible` is not run on the baseline's stimulus.
* **`grants/insn` is half a percent high on the two tests that invalidate
  with the cache enabled**, `ibex_icache_many_errors` and
  `ibex_icache_invalidation`, and that is not explained. It is flat on the
  other six. "What is left" above has the list of causes ruled out. The 0.7%
  and the 2.2% the previous version of this document reported for
  `grants/fetch` were noise, and are settled in the same section.
