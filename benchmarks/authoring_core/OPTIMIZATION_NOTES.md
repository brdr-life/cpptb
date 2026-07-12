# Authoring-core optimization notes

## Implemented

- Compile C++ DPI and pure-SystemVerilog authoring binaries symmetrically with
  `OPT_FAST=-O3`. A 16-pair, 300,000-iteration experiment measured the C++ DPI
  task-value CPU ratio at about `0.952x` relative to pure SV, with exact
  semantic counters and checksum.
- Require a second 16-pair batch before an initial confirmed threshold crossing
  can become a hard failure.
- Copy flat DPI input/output word arrays with `memcpy`, omit testbench-driven
  ports from generated input packing, and apply C++ output words only on
  initialization or when `STEP_OUTPUTS_CHANGED` is set.

## Profile summary

A 10-second macOS sampling profile of a 10,000,000-iteration task-value run
showed that most time is shared Verilator timing and DPI scheduling. The
incremental synchronous `Task<uint32_t>` path consists of a coroutine-frame
allocation plus scheduler adopt, finish, and reclaim bookkeeping.

Matched 300,000-iteration experiments measured:

- C++ task value versus C++ control: about `1.009x` total CPU.
- SV task value versus SV control: about `1.002x` total CPU.
- Net synchronous-task architecture cost: roughly `0.7%`.

## O3 regression snapshot

The full serial feature regression on 2026-07-11 completed without a hard
failure. Valid paired medians ranged from `0.6695x` to `1.0711x` relative to
the matching pure-SV testbenches. Clock cycles (`1.0619x`), wait-until
(`1.0517x`), and channel (`1.0399x`) were `passed_inconclusive` after 32 pairs:
their medians passed, but environmental variance left the one-sided 95% upper
median bounds above `1.10x`.

The focused task-value run exercised the new confirmation path and passed at
`1.0142x` after 32 pairs. Its later full-regression sample passed at `1.0138x`
after the initial 16 pairs, so the saved `latest` artifact contains that newer
measurement.

## Parked

These ideas remain available if future profiles show a meaningful need:

1. Lazily adopt awaited tasks into the scheduler only when they first park,
   allowing synchronous tasks to skip scheduler state registration.
2. Track dirty output spans so generated SV applies only ports changed in the
   current scheduler step. Keep this behind a focused benchmark because it
   adds mask transport and generated control logic.
3. Replace per-deadline generated timer processes with a persistent timer
   service.
4. Add authoring A/A admission checks, randomized pair order, and a secondary
   child-CPU diagnostic alongside the wall-time guard.

Lazy adoption should remain behind a focused microbenchmark because it changes
cancellation and timeout ownership for a small expected gain.

## Backdoor attribution (2026-07-12)

Exact 100,000-iteration C++ DPI/pure-SV twins isolated one indexed memory
probe operation per cycle. Before transport experiments, valid paired medians
were approximately `1.121x` for read, `1.143x` for deposit, and `1.154x` for
read plus deposit; the complete memory-backdoor kernel measured `1.155x`.

Native DPI getter returns (`unsigned int` through 32 bits and
`unsigned long long` through 64 bits) removed the temporary word buffer and
`Bits` conversion. One 32-pair read-only run improved to `1.098x` with a
`1.122x` one-sided 95% upper median bound; a later valid rerun measured
`1.105x` with a `1.111x` bound. Exact workload parity remained `400262`
simulation cycles.

Rejected experiments, all with exact semantic parity:

- dirty output-span transport: about `1.192x` on `mem_backdoor`;
- timer-deadline piggyback/deduplication: about `1.163x`;
- runtime subscription-pruned input packing: no clear gain; the direct
  before/after control runs moved from `0.905x` to `0.914x`, while task-value
  moved from `0.939x` to `0.933x`. The small, opposing changes were within the
  run-to-run variation seen during the experiment;
- queued post-step deposit with one scalar pop import per command: `1.182x`;
- queued deposit commands piggybacked on the existing output array: `1.188x`;
- direct-export NBA deposit attribution rerun: `1.157x` over 32 valid pairs.

The deferred deposit designs were standards-compliant and preserved same-slot
NBA ordering, but queue management and generated drain work outweighed the
nested export cost. They were fully removed.

Deeper generated-code inspection then showed that Verilator implemented each
exported `<=` as a heap-allocated `VlCoroutine` fork waiting on an NBA event.
Defining `deposit()` as an immediate blocking backdoor update removed that
timed-process path. Exact pure-SV twins were changed from `<=` to `=` while
retaining every user-authored `#1ps` observation boundary. Valid paired results:

- deposit-only: `1.096x`, one-sided 95% upper bound `1.105x` (32 pairs);
- read plus deposit: `1.070x`, upper bound `1.081x` (16 pairs);
- full memory backdoor: `1.093x`, upper bound `1.103x` (32 pairs);
- scalar hierarchical probe: `1.008x`, upper bound `1.022x` (16 pairs).

On the same 10,000,000-iteration deposit workload, wall time fell from
`29.449 s` to `27.433 s`, a `6.85%` improvement. The post-change Verilator
profile contains a direct store in the export path instead of the prior
per-deposit coroutine allocation and NBA-event resume. The remaining hot
path is dominated by shared timing-wheel wakeups, DPI input packing, scheduler
queues, and Verilator callback/context bookkeeping. The exact aggregate twin
also passed at `0.937x` (one-sided 95% upper bound `0.957x`), which checks that
the focused improvement did not shift work into another phase of the run.

Verilator requires `-Wno-BLKANDNBLK` when RTL also nonblocking-writes a
deposited variable. Depositing in the same time slot as that RTL write remains
outside the deterministic contract. If an NBA-scheduled backdoor operation is
needed later, it should be a separate primitive rather than hidden inside
`deposit()`.

## Directional transport experiment (2026-07-12)

The exact multidimensional-array twin initially measured `1.196x` C++ DPI over
pure SV at 100,000 iterations. A 10,000,000-iteration profile reproduced the
ratio with `30.204 s` C++ versus `25.153 s` pure SV. Rank-N signal getters and
setters accounted for only 24 of 3,855 exclusive samples; the shared generated
transport and scheduler were the material costs.

The generated ABI used one sparse 104-word signal-ID space for both directions,
although the design has 52 observed and 52 driven words. Compact directional
open arrays preserve global scheduler/signal IDs while carrying only the words
used in each direction. Six alternating 1,000,000-iteration C++ A/B pairs
measured `2,999.3 ms` sparse versus `2,953.2 ms` compact, a `1.54%` reduction.
The exact paired C++/SV guard improved to `1.179x` but remained a valid hard
failure.

Compile-gated telemetry then measured 1,000,007 scheduler steps, 300,002
changed-output steps, and 300,002 runtime output transfers per 100,000
iterations. Splitting `step(in_words)` from a conditional, idempotent
`pull_outputs(out_words)` removed output marshalling from unchanged steps.
Generated-code inspection confirmed that the hot step no longer creates,
zeros, or copies an output temporary. Six compact C++ A/B pairs nevertheless
improved by only `0.64%` (`2,948.8 ms` combined versus `2,929.8 ms` split).
The final exact paired guard measured `1.175x`, still a valid hard failure.

The post-split 10,000,000-iteration profile measured `29.937 s`. Its largest
named framework hotspot was `Runtime::copy_inputs`: compact transport currently
scatters 52 dense words back into the global-ID cache on every step. This cost
offsets much of the simulator-side compaction benefit. The next experiment
should eliminate that scatter, either through a dense runtime cache with a
global-ID-to-directional-offset map or a call-scoped view of the input transport.

A sparse-input control restored the 104-word ABI and one `memcpy`. Six
alternating 1,000,000-iteration pairs measured it `1.45%` slower than the
52-word compact ABI, showing that simulator-side open-array marshalling costs
more than the compact cache scatter it replaced.

Two follow-up changes were retained:

- configured clocks keep their real scheduler wait counts and queues but do
  not publish edge-interest changes back to the generated observer table;
  six alternating pairs improved by `1.52%`;
- compact observed inputs are exposed through a call-scoped view of the DPI
  open array, with global signal IDs translated through a generated offset map;
  six alternating pairs improved by a further `3.71%`. The pointer is restored
  before the DPI import returns and signal getters return values, so no view
  escapes the simulator-defined lifetime.

The exact 32-pair C++ DPI/pure-SV guard then improved from `1.175x` to
`1.113x`. Both order strata and the independent ratio agreed, so this remains
a valid hard failure above the `1.10x` limit rather than an environment
artifact.

A fresh 10-second profile collected 7,667 samples. The input scatter was gone;
the largest remaining framework-exclusive entries were edge-interest change
recording (105 samples), `Runtime::step` (99), wait registration (85), output
gathering (84), ready draining (76), parking (74), time advancement (71), and
edge-interest removal (70). The generated per-deadline timer process also
remained visible together with Verilator coroutine and allocator work.

Rejected residual experiments, all with exact semantic parity:

- dense output storage plus `memcpy`: `0.38%` slower;
- bypassing the disabled clock-publication function call: only `0.18%` faster,
  below the keep threshold;
- compile-only removal of the DPI callback safety scope: `0.46%` slower.

Compiling Verilator's global timing runtime with `OPT_GLOBAL=-O3` made C++ DPI
about `2.8%` faster but made pure SV about `11.9%` slower. It is not counted as
a framework improvement because the symmetric ratio improves mainly by
slowing the reference. Comparing each implementation at its measured best
setting would put this focused C++ workload near `1.103x`, but adopting that
policy requires an explicit benchmark-methodology decision.

The next justified architecture experiment is an interruptible persistent
timer service, followed by small-buffer storage for per-state wait bookkeeping.
The timer design must handle a newly inserted earlier deadline, equal-time
dispatch without `#0`, stale generations, and cancellation without allowing a
`disable fork` to affect unrelated processes.

Both remaining scheduler experiments were then falsified:

- A persistent timer consumer driven by delayed nonblocking generation-token
  assignments passed an expanded 273-check conformance suite, including a new
  case that inserts an earlier deadline while a later timer is already armed.
  Verilator still lowered every delayed assignment to a coroutine and added an
  NBA observer path. Six alternating 1,000,000-iteration pairs measured it
  `3.4%` slower than the existing generation-checked timer task, so the timer
  transport was restored. The earlier-deadline conformance case was retained.
- Replacing each state's retained edge-index vector with two inline slots plus
  overflow storage passed a dedicated three-edge `First` spill test and the
  complete regressions. A noisy 1,000,000-iteration run suggested a slowdown;
  an admitted eight-pair 500,000-iteration rerun measured a `1.011x` paired
  median. The larger per-state object outweighed a one-time allocation whose
  vector capacity was already retained, so the inline container was removed.

After restoring both experiments, a fresh valid 32-pair guard measured
`1.105x` with a `1.109x` independent ratio. This remains a hard failure above
`1.10x`; neither experiment is credited for the run-to-run movement from the
earlier `1.113x` result. Further scheduler work should begin from a new measured
hypothesis rather than revisiting timer process shape or small-buffer
edge-index storage. Any future inline-storage experiment must restore explicit
coverage for a `First` containing at least three simultaneous edge
registrations so its overflow path is exercised.

## Coroutine frame pool (2026-07-12)

A thread-local, size-bucketed pool now reuses coroutine frames up to `2 KiB`,
with at most 32 cached frames per bucket. Larger frames and saturated buckets
fall back to the global allocator. Pool diagnostics are compiled only into the
runtime unit test, so production scheduling does not increment counters.

Fresh 16-pair baselines measured `0.916x` for control and `0.941x` for
task-value. With pooling, control remained effectively flat at `0.913x`, while
task-value improved to `0.916x`. A dedicated 32-pair confirmation measured
task-value at `0.912x`, with `0.45%` disagreement between the paired and
independent ratios. The pure-SV testbench and workload contract were unchanged.

## Scheduler architecture review (2026-07-12)

A max-effort Fable review covered the coroutine runtime, DPI runtime, generated
transport, generated Verilator implementation, exact C++/SV twins, profiles,
and the complete rejected-experiment history. The review agrees with the local
profile: the basic transaction engine is competitive (`control` at `0.913x`
and `signal_edge` at `0.993x`), while the remaining `array_multidim` regression
is the marginal cost of bulk transport, timer dispatch, and bookkeeping added
at each suspension point.

The exact multidimensional workload executes about ten DPI scheduler steps per
iteration: four rising-edge steps, three falling-edge steps, and three delay
steps. It also performs three output pulls. The largest removable costs are:

- clock waits still maintain edge-interest counts, per-state index lists, and
  change tracking even though configured clocks are driven unconditionally and
  their interest publication is suppressed;
- each timer deadline creates generated SV timer processes and additional
  Verilator coroutine/timing-queue work;
- all 52 observed words are packed at every step although the bulk arrays are
  read on only a subset of continuations;
- a single DPI step can cross more than one scheduler boundary before return.

Ranked experiments from the review:

1. **Static-clock wait bookkeeping:** mark configured clocks as static edge
   sources and skip only their interest-count/publication bookkeeping. Keep the
   real edge wait queues, stale-registration accounting, and falling-edge
   counts unchanged. This is materially different from the rejected
   publication-call bypass, which left the counting machinery active. Expected
   improvement is `2-3%`; keep only with at least `0.5%` C++ A/B improvement
   and an improved exact guard.
2. **Primary-clock-fused timer dispatch:** let the existing primary clock
   process sleep until the earlier of its next edge and the runtime deadline,
   retaining the current one-shot timer only as a fallback when another process
   inserts an earlier deadline. This introduces no delayed-NBA token process
   and is therefore distinct from the rejected persistent timer. Expected
   improvement is `1.5-5%`; validate multi-clock, earlier-deadline, equal-time,
   cancellation, and no-`#0` behavior.
3. **Opt-in on-demand bulk transport:** keep hot scalars in the per-step packed
   transport while exposing wide and unpacked ports through generated standard
   DPI export getters/setters. This moves infrequently accessed bulk signals
   off the push-every-step path instead of pruning or sparsifying that same
   path. Expected improvement is `1.5-4.5%` on bulk kernels, but the sign may
   vary by simulator, so selection must remain per-port and benchmarked per
   backend.
4. **One scheduler boundary per DPI step:** split time/edge notification from
   the final drain/cleanup so a step performs one boundary pass. Expected
   improvement is `0.5-1.5%`.
5. **Static signal bindings and a single-edge park fast path:** remove function
   pointer and generic wait-dispatch overhead where generated types make the
   operation known. Each is expected to be below `1%` and should follow the
   higher-confidence experiments.

Before the timer or bulk-transport experiments, compile-gated telemetry should
count steps by phase, timer arms/wakes, deadline queries, and output pulls. A
diagnostic-only hot-word pack can bound the maximum transport gain, and an
inverted-call-tree profile should separate generated timer forks, Verilator
timing queues, DPI context setup, and framework frames.

The next experiment is static-clock wait bookkeeping. It has the strongest
direct profile evidence, is runtime-only and simulator-independent, and does
not alter the pure-SV twin. Required tests include suppressed-clock
park/resume/cancel parity, a mixed clock/observer/timer `First`, and at least
three simultaneous edge registrations to retain spill-path coverage. Measure
six or more alternating 1,000,000-iteration C++ A/B pairs before running a
fresh 32-pair exact guard. The existing `1.10x` hard stop remains unchanged.
