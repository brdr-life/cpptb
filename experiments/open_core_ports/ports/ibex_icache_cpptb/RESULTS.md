# ibex_icache: cpptb against the UVM baseline

Both harnesses drive the same `ibex_icache`, elaborated from the same 77 RTL
sources at the same pinned Ibex commit under Verilator 5.050. The port is
[README.md](README.md); the baseline is
[`ports/ibex_icache_uvm`](../ibex_icache_uvm/README.md), where all ten of
upstream's tests pass.

**Three tests, three seeds each, on both harnesses: 18 of 18 runs pass.** A
further 120 cpptb runs across 40 seeds also pass. Nothing either scoreboard
checks failed on either side.

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

Means over seeds 123, 124 and 125.

### `ibex_icache_smoke`

The base virtual sequence at its defaults: the enable line toggles, seeds
change, the cache is invalidated from time to time.

| | cpptb | UVM | |
| --- | ---: | ---: | --- |
| items | 867.3 | 933.7 | drawn independently |
| insns requested | 9134.3 | 11713.0 | |
| fetches checked | 8404.3 | 10716.7 | |
| errored fetches | 62.0 | 64.7 | |
| invalidations | 23.3 | 29.3 | |
| new seeds | 329.7 | 279.7 | |
| memory grants | 6952.0 | 8702.0 | |
| memory responses | 6952.0 | 8698.7 | |
| errored responses | 248.3 | 299.0 | |
| ratio windows completed | 0.0 | 0.0 | agrees, and both are zero |
| cycles | 65191.3 | 76780.7 | |
| wall seconds | 0.16 | 36.80 | |
| **insns/item** | **10.5333** | **12.5547** | see below |
| **fetches/insn** | **0.9195** | **0.9152** | 0.5% apart |
| **grants/fetch** | **0.8276** | **0.8121** | 1.9% apart |
| **err/resp** | **0.0360** | **0.0343** | 5% apart |

### `ibex_icache_passthru`

Branch targets confined to a 64-byte window, the cache held disabled.

| | cpptb | UVM | |
| --- | ---: | ---: | --- |
| items | 867.3 | 933.7 | |
| insns requested | 8527.7 | 11526.0 | |
| fetches checked | 7450.7 | 10189.0 | |
| errored fetches | 99.3 | 103.3 | |
| new seeds | 442.3 | 479.7 | |
| memory grants | 6338.0 | 8195.7 | |
| errored responses | 371.7 | 371.3 | |
| ratio windows completed | 0.0 | 0.0 | the cache is off; correct |
| cycles | 60441.3 | 75671.0 | |
| wall seconds | 0.15 | 35.57 | |
| **insns/item** | **9.8216** | **12.3485** | see below |
| **fetches/insn** | **0.8745** | **0.8842** | 1.1% apart |
| **grants/fetch** | **0.8512** | **0.8042** | 5.8% apart |
| **err/resp** | **0.0586** | **0.0453** | 29% apart, three seeds |

### `ibex_icache_caching`

The same window with the cache held enabled and invalidation avoided, which is
what lets a caching-ratio window complete. The means are misleading and the
per-seed numbers are not, so both are here.

| | cpptb | UVM |
| --- | ---: | ---: |
| fetches checked | 3739.7 | 8659.3 |
| errored fetches | 462.7 | 258.3 |
| memory grants | 3020.7 | 1961.0 |
| ratio windows completed | 3.0 | 8.7 |
| wall seconds | 0.10 | 24.90 |

Per seed:

| harness | seed | fetches | errored fetches | grants | grants/fetch | windows |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| cpptb | 123 | 799 | 799 | 4924 | 6.16 | 0 |
| cpptb | 124 | 8486 | 0 | 500 | 0.0589 | 9 |
| cpptb | 125 | 1934 | 589 | 3638 | 1.88 | 0 |
| UVM | 123 | 11831 | 0 | 543 | 0.0459 | 13 |
| UVM | 124 | 2486 | 775 | 4798 | 1.93 | 0 |
| UVM | 125 | 11661 | 0 | 542 | 0.0465 | 13 |

The test is bimodal on both harnesses, in the same two modes, and the mode is
decided by one draw. `ibex_icache_caching` takes no new memory seeds, so the
whole run uses seed 0, and the memory model makes seed 0 error over
`[0xdeadbeef, 0xfeadbeef)` after `address ^ 0xf00dbeef`. Whether that covers
the 64-byte window depends only on `base_addr`, which is drawn with a quarter
of its weight in `[0:15]` -- and `0 ^ 0xf00dbeef` is inside the error range. So
about half the time the window errors on every access, the cache never gets to
cache anything, and no ratio window completes.

Over **40 cpptb seeds, 16 complete at least one window** and 158 windows
complete in total. All 40 pass.

Restricted to the seeds where the window is clean, the two harnesses agree
closely on the thing the test exists to measure: grants 500 against 543 and
542, for a cache that is fetching the same 17 words.

This is a property of the environment, not of either harness, and it belongs
next to the caveat `ports/ibex_icache_uvm` already records for
`ibex_icache_smoke`. `ibex_icache_caching` **checks the caching ratio on about
40% of seeds and on the rest checks only that errored fetches are reported
correctly.** Upstream's `reseed` of 50 hides this; one seed does not.

## The one systematic difference, and what causes it

`insns/item` is 10.5, 9.8 and 10.2 on cpptb and 12.6, 12.3 and 13.0 on the UVM
baseline: the baseline asks for about 25% more instruction fetches per
transaction. Every other rate agrees within a few per cent, and `fetches/insn`
-- how much of what was asked for the cache actually delivered -- agrees to
about 1%, which is the number that would move if a driver were mistimed.

`num_insns` is drawn from `dist { 0 :/ 5, [1:20] :/ 20, [21:100] :/ 1 }` for a
branch transaction. `ports/ibex_icache_uvm` already draws the *bucket* directly
because Verilator ignores `dist` weights, and then applies it as

```systemverilog
soft num_insns inside {[d_insn_lo:d_insn_hi]};
```

against the item's own hard `num_insns inside {[0:100]}`. The bucket weights
come out right. The value inside the bucket does not.

Counted from the baseline's own UVM_HIGH logs, over three seeds, splitting the
`[1:20]` draws into quartiles where a uniform draw would put 25% in each:

| seed | transactions in `[1:20]` | 1-5 | 6-10 | 11-15 | 16-20 | mean |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 123 | 390 | 63 | 41 | 66 | 220 | 13.97 |
| 124 | 450 | 77 | 76 | 80 | 217 | 13.22 |
| 125 | 382 | 88 | 53 | 71 | 170 | 12.34 |

Half of every draw lands in the top fifth of the range. The reduced case is
`shims/verilator_inside_range_uniformity.sv`, three ways of picking a number in
`[1:20]`, 4000 draws each:

```
hard inside [1:20]            q1=744 q2=631 q3=602 q4=2023  mean=13.18 (uniform 10.50)
soft inside [1:20] of [0:100] q1=778 q2=645 q3=648 q4=1929  mean=12.97 (uniform 10.50)
$urandom_range(20, 1)         q1=990 q2=981 q3=1023 q4=1006  mean=10.58 (uniform 10.50)
```

**A constrained `randomize()` over an `inside` range is not uniform over that
range**, whether the range is a hard constraint or a soft one. This is a sixth
Verilator randomization defect on top of the five
`ports/ibex_icache_uvm` documents, and it is the whole of the difference between
the two harnesses' stimulus. The cpptb port draws the range with a uniform
generator and gets the distribution the constraint describes; the baseline gets
a skew.

Which of the two is doing the right thing is not in question -- the constraint
says uniform -- but it is worth being clear about the direction: the baseline
runs *more* stimulus than it intends, not less.

## Everything else that differs, and why

* **`err/resp` on `passthru`, 0.0586 against 0.0453.** Whether a memory read
  errors depends on the seed in force and the address, and `passthru` takes a
  new seed on roughly half its transactions. Over three seeds the two sides
  drew different seeds; `smoke` has the same mechanism and lands 5% apart.
  Nothing separates this from sampling noise at three seeds.
* **`grants/fetch`, 2% on `smoke` and 6% on `passthru`.** A fetch that is
  discarded because a branch redirected the cache still cost a memory read, so
  shorter runs between branches waste more reads per fetch. cpptb runs 10.2
  instructions between transactions where the baseline runs 12.5, from the
  defect above, so it branches more often per fetch. The direction and the
  magnitude both follow.
* **`cycles/item`, 75 against 82 on `smoke`.** Fewer instructions per
  transaction, so fewer cycles per transaction.

None of the three needed a fix in the port. Each was chased to a cause before
being reported.

## Speed

Not the point of this port, and recorded because it is free.

| test | cpptb | UVM baseline |
| --- | ---: | ---: |
| `ibex_icache_smoke` | 0.16 s | 36.8 s |
| `ibex_icache_passthru` | 0.15 s | 35.6 s |
| `ibex_icache_caching` | 0.10 s | 24.9 s |

The baseline figures are UVM_HIGH runs, which is what the comparison needs and
is not how it would normally be run. At UVM_LOW, on the same machine and the
same three jobs, the same three tests take 34.4, 33.4 and 22.1 seconds, so
tens of megabytes of logging cost about 5%: it is not what makes the run slow.
What does is that the
baseline is a 47 MB binary carrying the whole of UVM, that every constrained
`randomize()` is a pipe round trip to `z3`, and that it simulates about 40
microseconds of model time per wall second where this port does about 6
milliseconds.

Two things to keep in mind before reading anything into the ratio. The two runs
are not the same amount of simulation: the baseline runs about 18% more cycles
per test, for the reason above. And the baseline elaborates `tb.sv` with the
UVM environment, its covergroups and its two protocol-checker modules bound in,
where this elaborates the design and its RAMs. It is a comparison of two
harnesses, not of two simulators.

The whole cpptb side of this document -- 3 tests, 3 seeds -- takes 1.2 seconds.
The UVM side takes 5 minutes and writes 275 MB of logs.

## What this does not cover

* **`ibex_icache_ecc` is not ported**, so nothing here says anything about ECC.
  The baseline's own caveat stands and is worth repeating: it injects corrupted
  RAM reads and then checks that the returned data is still correct, and
  **nothing on either side checks that the ECC logic reported an error**.
* **The caching ratio is checked on about 40% of seeds**, as above.
* **`ibex_icache_smoke` completes zero ratio windows** on both harnesses, which
  is the caveat `ports/ibex_icache_uvm` already records. It proves 8404 fetches
  returned correct data and proves nothing about caching.
* **Assertions and functional coverage run on neither side.**
* **One seed per test is not a statement about seed sensitivity.** Upstream's
  `reseed` is 50. Three seeds are used for the comparison here and 40 for the
  cpptb-only pass rate.
