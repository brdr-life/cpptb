# ibex_icache: cpptb against the UVM baseline

Both harnesses drive the same `ibex_icache`, elaborated from the same 77 RTL
sources at the same pinned Ibex commit under Verilator 5.050. The port is
[README.md](README.md); the baseline is
[`ports/ibex_icache_uvm`](../ibex_icache_uvm/README.md).

**All ten of upstream's tests, ten seeds each, on both harnesses: 200 of 200
runs pass.** A further 400 cpptb runs over 40 seeds also pass. Nothing either
scoreboard checks failed on either side.

## What is comparable and what is not

The two harnesses cannot be given the same stimulus. Each draws from its own
random stream, the same seed means nothing across them, and there is no
transaction log to replay. What is comparable is the generator: both draw the
same fields from the same buckets at the same weights, so **per-item rates**
should agree. Totals scale with `num_trans`, which is drawn independently on
each side and lands anywhere in 800 to 1000.

`run_tests.py --compare` reports both. The UVM numbers are counted out of a
UVM_HIGH log, which is the only place that environment reports per-transaction
activity.

## The comparison

Rates, as means over seeds 123 to 132.

| test | insns/item | fetches/insn | grants/fetch | err/resp | cycles/item |
| --- | ---: | ---: | ---: | ---: | ---: |
| `ibex_icache_smoke` | 10.32 / 12.47 | 0.928 / 0.924 | 0.847 / 0.812 | 0.037 / 0.034 | 75.3 / 84.1 |
| `ibex_icache_passthru` | 10.27 / 12.43 | 0.859 / 0.875 | 0.846 / 0.805 | 0.061 / 0.050 | 73.1 / 81.6 |
| `ibex_icache_caching` | 10.21 / 12.86 | 0.470 / 0.555 | 0.656 / 0.391 | 0.900 / 0.869 | 44.0 / 47.4 |
| `ibex_icache_invalidation` | 10.05 / 12.59 | 0.918 / 0.855 | 0.250 / 0.268 | 0.146 / 0.235 | 59.9 / 62.1 |
| `ibex_icache_oldval` | 10.15 / 12.65 | 0.861 / 0.870 | 0.814 / 0.759 | 0.064 / 0.052 | 69.8 / 78.9 |
| `ibex_icache_back_line` | 1.503 / 1.477 | 0.762 / 0.816 | 1.517 / 1.217 | 0.847 / 0.790 | 11.1 / 9.97 |
| `ibex_icache_many_errors` | 10.48 / 12.50 | 0.624 / 0.573 | 0.741 / 0.748 | 0.309 / 0.320 | 53.5 / 54.6 |
| `ibex_icache_ecc` | 10.21 / 12.27 | 0.470 / 0.549 | 0.667 / 0.425 | 0.882 / 0.842 | 44.3 / 46.9 |
| `ibex_icache_stress_all` | 8.87 / 10.41 | 0.803 / 0.812 | 0.602 / 0.546 | 0.173 / 0.185 | 54.7 / 55.2 |
| `ibex_icache_stress_all_with_reset` | 6.13 / 8.61 | 0.680 / 0.606 | 0.897 / 0.875 | 0.196 / 0.243 | 32.0 / 39.3 |

cpptb first, the UVM baseline second. Six of the forty rates below `insns/item`
are more than 20% apart: three `grants/fetch` that are a mode mix, and three
`err/resp` that are the same sampling effect. The next three sections chase each
to its cause.

The counters that are not rates:

| | cpptb | UVM | |
| --- | ---: | ---: | --- |
| child sequences, `stress_all` | 12.3 | 11.7 | |
| resets, `stress_all` | 7.5 | 6.0 | one per child, half the time |
| child sequences, `stress_all_with_reset` | 12.5 | 12.6 | |
| resets, `stress_all_with_reset` | 12.5 | 12.6 | one per child, always |
| key answers / refusals, pooled over all ten tests | 2294 / 2409 | 2156 / 2196 | |
| answered fraction | 0.488 | 0.495 | the valid bit is one random bit |
| invalidations, `ibex_icache_invalidation` | 17.3 | 17.0 | |
| new seeds, `ibex_icache_invalidation` | 17.3 | 17.0 | one per invalidation |

## The one systematic difference, and what causes it

`insns/item` is 15% to 21% lower on cpptb on the eight tests that run the base
sequence to completion, and 29% lower on `ibex_icache_stress_all_with_reset`
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
documents. The cpptb port draws the range with a uniform generator and gets the
distribution the constraint describes; the baseline gets a skew, and so runs
*more* stimulus than it intends, not less.

`ibex_icache_back_line` is the control. Its `num_insns` is drawn rather than
solved on both sides now (see below), and its `insns/item` is 1.503 against
1.477, which is 2% apart.

## The three `grants/fetch` that are more than 20% apart

They are `ibex_icache_caching`, `ibex_icache_ecc` and
`ibex_icache_back_line`, and all three are the same thing: those are the only
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
| `ibex_icache_caching` | 0.0568 / 0.0463 | 3.503 / 2.852 |
| `ibex_icache_ecc` | 0.0691 / 0.0623 | 3.504 / 2.871 |
| `ibex_icache_back_line` | 0.3572 / 0.3609 | 2.718 / 2.439 |

`ibex_icache_back_line` agrees to 1% once the mode is held fixed, and so does
its `cycles/item`: 5.406 against 5.623 on the clean seeds, where the pooled
figure was 11.1 against 9.97. The residual 20% on `caching` and `ecc` in the
clean mode is `insns/item` again -- the working set is a fixed 17 words, so the
grant count is fixed and `grants/fetch` moves inversely with the number of
fetches: 12.86/10.21 is 1.26 and 0.0568/0.0463 is 1.23.

This is a property of the environment, not of either harness. It means
`ibex_icache_caching` **checks the caching ratio on about 40% of seeds and on
the rest checks only that errored fetches are reported correctly.** Over the ten
seeds, cpptb completed 39 ratio windows and the baseline 62, all on the clean
seeds, and all passed. Upstream's `reseed` of 50 hides this; one seed does not.

## Everything else that differs, and why

* **`err/resp`, 23% on `ibex_icache_passthru`, 23% on `ibex_icache_oldval` and
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
  | `oldval` | .044 .046 .057 .059 .059 .065 .070 .073 .079 .083 | .037 .039 .040 .047 .051 .053 .055 .063 .063 .070 |
  | `invalidation` | .000 .000 .076 .078 .096 .138 .168 .195 .330 .381 | .082 .125 .129 .161 .186 .208 .308 .359 .362 .403 |

  The sign is not the same on all three: cpptb is higher on `passthru` and
  `oldval` and lower on `invalidation`. `invalidation` takes one new seed per
  invalidation and 17 of them per run, against several hundred for the other
  two, so its spread is the widest and its difference of means is 1.6 standard
  errors. `passthru` and `oldval` are about 2.4 standard errors apart, which is
  more than noise comfortably explains, and the mechanism that fits the
  direction is that an errored fetch ends the transaction, so an erroring seed
  is replaced sooner than a clean one, and it is replaced *relatively* sooner on
  the harness whose transactions are longer. That is the baseline. This is the
  one difference in the table that is not chased all the way to the bottom.
* **`insns/item` on `ibex_icache_stress_all_with_reset`, 29% rather than the
  usual 20%.** Every child is stopped after 100 to 1000 cycles, so a larger
  share of the items that run are the first items of a child, which are forced
  branches and therefore draw `num_insns` from the branch distribution rather
  than the shorter request one. The skew above applies to more of the run.
  `cycles/item`, 32.0 against 39.3, follows it.
* **`grants/fetch`, 4% to 10% on the tests that are not bimodal.** A fetch
  discarded because a branch redirected the cache still cost a memory read, so
  shorter runs between branches waste more reads per fetch. cpptb runs about 10
  instructions between transactions where the baseline runs about 12.5, from the
  defect above, so it branches more often per fetch. The direction and the
  magnitude both follow.

None of these needed a fix in the port, and all but the last were chased to a
cause before being reported.

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
| `ibex_icache_smoke` | 0.17 | 23.96 |
| `ibex_icache_passthru` | 0.18 | 23.35 |
| `ibex_icache_caching` | 0.12 | 13.51 |
| `ibex_icache_invalidation` | 0.15 | 14.84 |
| `ibex_icache_oldval` | 0.16 | 23.62 |
| `ibex_icache_back_line` | 0.03 | 13.45 |
| `ibex_icache_many_errors` | 0.12 | 16.69 |
| `ibex_icache_ecc` | 0.11 | 14.50 |
| `ibex_icache_stress_all` | 0.15 | 15.52 |
| `ibex_icache_stress_all_with_reset` | 0.03 | 4.98 |

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
that mask directly, which is why it is 14.5 seconds here. The cpptb port draws
the same distribution the same way, in C++.

Two things to keep in mind before reading anything into the ratio. The two runs
are not the same amount of simulation: the baseline runs about 6% more cycles
per test, for the reason above. And the baseline elaborates `tb.sv` with the UVM
environment, its covergroups and its two protocol-checker modules bound in,
where this elaborates the design and its RAMs. It is a comparison of two
harnesses, not of two simulators.

The whole cpptb side of this document -- 10 tests, 10 seeds -- takes 12
seconds. The UVM side is 27 minutes of CPU, 4.5 minutes on eight jobs, and
writes 2.5 GB of logs.

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
