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
3. Reduce generated timer process allocation while preserving exact deadline
   and clock-coincidence semantics.
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
   cancellation, and no-`#0` behavior. This proposal was superseded by the
   clock-agnostic E2 owner documented below; no performance claim was measured.
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

## Clock-agnostic timer owner (2026-07-12)

Fable's finalized E2 generated-SV architecture replaces steady-state
per-deadline forks with one persistent timer owner. This change does not modify
the C++ scheduler or DPI runtime. The
owner sleeps only for a positive interval, parks on `timer_kick` when the
published deadline is `NO_TIMER`, and re-reads the module variable after every
wake. A byte-equivalent generation-checked `timer_wakeup` remains as the
strict-earlier fallback when a clock or observer callback advances the
deadline beneath the owner's current target.

The retained invariants are I1: `timer_deadline` matches the scheduler's
earliest live deadline after every timer-change request; I2: owner and fallback
delivery are exact-once, with stale fallbacks rejected by generation; I3: no
`#0`, delayed NBA, or `disable fork`; I4: every live deadline has a wake no
later than itself; I5: fallback is unreachable in a clockless wrapper; and I6:
next-deadline DPI call count is unchanged at one per timer-change request while
steady-state per-arm process allocation is zero. Exceptional strict-earlier
fallbacks may allocate one one-shot process.

The pre-change baseline and post-change wrapper both pass the isolated R1
later-rearm coincidence contract at 12 checks/two cycles, R2 equal-target
mid-sleep contract at 11 checks/one cycle, chained earlier deadlines at 12
checks/three cycles, and idle rearm at eight checks/one cycle. The main suite
remains 273 checks/eight cycles, and the clockless C++ DPI/pure-SV twin matches
exactly with zero cycles.

Matched six-pair, 1,000,000-iteration C++ A/B runs measured candidate/baseline
CPU ratios of about `0.964x` for `array_multidim`, `0.977x` for timeout,
`0.964x` for task timeout, and `0.981x` for control. The exact
`array_multidim` C++ DPI/pure-SV guard passed at `1.067x` in a valid
environment.

`Delay(duration)` and `ClockCycles(clock, count)` remain separate scheduling
concepts. Delay is an absolute simulator-time wait owned by the clock-agnostic
timer service. ClockCycles is an edge wait against the caller-selected signal
and therefore works with any configured clock in a multi-clock design without
making that clock the scheduler's time base.

## Isolated scheduler experiments (2026-07-12)

Six architecture experiments were implemented and tested in independent git
worktrees before integration:

1. Static edge sources retain real rising/falling wait queues, stale
   registration cleanup, cancellation, and `First` behavior while omitting
   interest accounting and publication for configured clocks. The standalone
   exact guard passed at `1.092x`.
2. The clock-agnostic timer owner described above removes steady-state
   per-deadline process allocation and passed standalone at `1.067x`.
3. Opt-in on-demand transport removes selected wide or unpacked ports from the
   packed per-step arrays and accesses them through generated standard DPI
   exports. Six-pair C++ A/B ratios were about `0.913x` for `array_wide`
   and `0.978x` for the 137-bit echo. The multidimensional benchmark remains
   on packed transport because moving it on demand was neutral in C++ A/B and
   failed the exact guard.
4. A single scheduler-boundary experiment was rejected. It drained timer and
   edge notifications together, changing the existing equal-time contract:
   a coroutine resumed by a timer could no longer arm an edge wait in time to
   observe an edge from the same DPI step.
5. Static generated signal bindings were promising in isolation, measuring
   about `0.991x` on `array_multidim` and `0.948x` on `signal_edge`.
   They require a transport-aware integration so on-demand signals cannot be
   mistaken for packed-array offsets.
6. A direct single-edge park path measured about `0.992x` in C++ A/B. Its
   standalone exact guard crossed the limit at `1.105x`, so it is retained
   only when the combined stack independently passes the guard.

The first integrated stack of static edge sources, the single-edge fast path,
and the generic timer owner measured `0.940x` versus the checkpoint C++
binary and `1.039x` versus the exact pure-SV twin. Adding tuned on-demand
transport measured `0.936x` versus the checkpoint and `1.046x` versus pure
SV. Checks, scheduler cycles, checksums, and selected workload counters matched
in every admitted run, and both exact measurements used valid environments.
The `1.10x` hard stop remains unchanged.

Transport-aware static generated bindings were then added to the integrated
stack. Packed bindings encode validated directional offsets and access the
runtime arrays without dynamic signal dispatch. On-demand bindings encode
their generated callbacks instead of a synthetic packed offset; callback-scope
checks, dynamic scalar conversion, local-edge delivery, and input-view lifetime
remain unchanged.

Six alternating 1,000,000-iteration C++ A/B pairs against the checkpoint
measured:

- `array_multidim`: `0.9052x` paired, `0.9041x` independent;
- `array_wide`: `0.8712x` paired, `0.8721x` independent;
- `signal_edge`: `0.9500x` paired, `0.9497x` independent;
- `wide_echo_137`: `0.9313x` paired, `0.9296x` independent.

Transactions, checks, scheduler cycles, checksums, and failures matched for
every workload. The final exact `array_multidim` C++ DPI/pure-SV guard passed
at `1.012x` in a valid environment, leaving substantially more margin than
the required `1.10x` limit.

An independent post-integration review found that static packed scalar writes
narrower than 32 bits were not normalized before updating the C++ cache. That
could make a value such as `2` appear nonzero in a one-bit C++ signal even
though SystemVerilog stored zero, including a false local edge. Static packed
and on-demand bindings now normalize before comparison, storage, callbacks,
and edge delivery. Dedicated one-bit tests cover zero-equivalent and
one-equivalent out-of-range writes. The fix also exposed and corrected a
conformance predicate that had relied on writing `7` to a one-bit signal.

Post-fix six-pair C++ A/B ratios remained `0.9070x` for
`array_multidim` and `0.9513x` for `signal_edge`; the exact
`array_multidim` guard passed at `1.006x`.

The same review then found that implicit conversion to generic `coro::Signal`
could bypass the typed setter and lose width metadata. Static scalar
conversions now use width-specialized get/set thunks. Unit tests exercise
direct and converted packed/on-demand writes with live rising-edge waiters,
proving that zero-equivalent masked writes do not wake and one-equivalent
writes wake exactly once. Final six-pair ratios were `0.9151x` for
`array_multidim` and `0.9534x` for `signal_edge`; the exact guard passed
at `1.013x`.

Equal-time timer and clock callbacks are covered by the R1/R2 Verilator
contracts, but their sibling active-region process ordering has not yet been
validated on a second simulator. Treat that exact coincidence ordering as a
backend conformance requirement, not a proven portable SystemVerilog
guarantee, until another supported simulator runs the same cases.

The final post-review serial feature regression passed every authoring kernel,
the multiclock and clockless timer equivalence cases, and the peripheral
suite. Authoring paired C++ DPI/pure-SV ratios ranged from `0.740x` to
`1.058x`; the peripheral suite passed at `0.986x`. No measured feature
crossed the `1.10x` hard stop.

## Follow-up portability work

The integrated branch is approved, with these non-blocking follow-ups retained:

1. Run the frozen equal-time timer/clock contracts on a second simulator.
2. Zero-extend and width-normalize signed narrow internal-probe getters, as is
   now done for transport ports. No current manifest exposes a signed internal.
3. Add a defensive generated `timer_owner` invariant guard so a future
   scheduler regression cannot become a zero-time loop.
4. Reject or explicitly diagnose `compact_input_transport: false` when static
   binding or on-demand transport necessarily enables compact transport.

## Edge callback and generated-observer profile (2026-07-13)

The 100,000-iteration aggregate workload reported 4,050,007 scheduler steps,
2,700,005 edge notifications (1,500,005 rising and 1,200,000 falling), and
1,350,001 timer notifications. Edge-path telemetry showed that 2,550,005 of
the 2,700,005 edge callbacks resumed a waiting coroutine. Only 150,000
callbacks, or 5.6%, did not resume C++ work. The simulator owns and toggles
the clock waveform, but generated wrappers currently call DPI on every rising
edge of each configured clock and gate falling clock callbacks with a global
waiter summary. This dense workload deliberately awaits almost every relevant
edge, so that policy is mostly productive here, but it is not fully
demand-gated.

The unexpected regression came from generated automatic signal observers.
Each one waited on a dynamic expression containing `edge_interest[signal]`
before waiting on the signal itself. Verilator lowered those dormant dynamic
wait processes with substantial scheduling overhead. A historical checkout
measured about 1.196 seconds for the C++ aggregate, while the regressed wrapper
measured about 1.800 seconds. Removing automatic observers restored roughly
1.18 to 1.21 seconds, isolating the generated process shape as the cause.

Automatic observers now wait directly on the SystemVerilog signal and inspect
the interest mask only after a transition. A signal transition can wake the
small SystemVerilog process, but no DPI callback occurs without active C++
interest. This preserves generic dynamic edge waits while avoiding the costly
dynamic wait expression. The implementation also keeps explicit observer and
clock-source behavior unchanged.

The first formal 16-pair aggregate guard improved from `1.366x` to `1.044x`.
After regenerating every checked-in wrapper and rebuilding the complete
benchmark matrix, the repeated guard passed at `1.052x`, with `0.03%`
disagreement between paired and independent ratios. Both results are below
the `1.10x` hard limit.

Additional experiments did not justify integration: per-element on-demand
transport regressed its dedicated comparison, changing the probe guard from
thread-local to global was neutral, and the coroutine frame pool recorded only
13 system allocations while reusing cached frames 1,999,989 times. Global and
alternate thread-local frame pools were neutral or slower. Lazy task adoption
was parked because cancellation through unadopted intermediate tasks requires
a more invasive lifetime design for an estimated sub-percent benefit. An
inline one-tick timer delay was rejected because it could block unrelated
clock progress; next-tick timers continue through the persistent timer owner.

An independent high-effort review found no blocker in the observer fix or its
generic one-bit output contract. It identified the global falling-edge summary
as the clearest residual leak: a falling or any-edge wait on a non-clock signal
can temporarily enable falling callbacks for every configured clock. The next
contained experiment should count only static clock-source registrations for
that summary, add a directed non-clock waiter test, and compare the no-resume
callback count before and after. Fully demand-gating rising clocks is a larger
policy change because the existing static-clock fast path intentionally avoids
publishing per-wait clock interest; it should be evaluated separately against
clock-dense and clock-idle workloads.

## Batched force/release probe-command experiment (2026-07-13)

The exact `force_release` pair established a fresh valid baseline of `1.204x`
C++ DPI/pure SV at 100,000 iterations (32 paired samples after the confirming
batch). A portable generated transport then replaced nested exported-DPI
force/release calls with a deterministic C++ FIFO, a generated SV batch pull,
and generated endpoint dispatch. It supported wide values, multiple commands,
and batches spilling beyond 64 commands without advancing simulation time.

The transport remained semantically exact and passed the existing 275-check
conformance suite, but its formal paired ratio regressed to `1.292x`; the
DPI-first and SV-first strata measured `1.279x` and `1.297x`, and the
independent ratio was `1.289x`. The five-open-array batch marshalling and SV
dispatch cost more than the two direct exported-DPI calls in this workload.
The experiment was rejected under the unchanged `1.10x` hard guard.

The separately measured specialized Delay callback kept the exact explicit
`1 ps` boundaries and passed the 100,000-iteration semantic comparison. Its
32-pair guard measured `1.214x` (DPI-first `1.207x`, SV-first `1.220x`, and
independent `1.213x` with `0.11%` disagreement), slightly slower than the
`1.204x` baseline. It was also rejected and removed.

## Force/release isolation and build experiments (2026-07-13)

A standalone `force_direct` C++/SV pair now isolates one force, immediate
readback of the exact forced net, and release per iteration. It performs no
clock edge, delay, scheduler resumption, protocol transaction, or simulated
time advance. At 1,000,000 iterations its 32-pair guard measured `1.652x`
(DPI-first `1.661x`, SV-first `1.650x`, independent `1.660x`). This confirms
that direct exported-DPI probe calls have a visible microbenchmark cost, while
the complete propagated workload remains dominated by other work.

The exact 1,000,000-iteration `force_release` workload established a fresh
portable `-O3` baseline of `1.212x` (DPI-first `1.204x`, SV-first `1.226x`,
independent `1.221x`). Whole-program LTO applied only to C++ DPI produced a
diagnostic `0.943x`, but that comparison intentionally did not optimize the SV
side equally and is not an acceptable guard result. Applying `-flto` to both
binaries improved the fair paired ratio to `1.138x` (DPI-first `1.117x`,
SV-first `1.146x`, independent `1.142x`). The improvement is real but remains
above the `1.10x` hard limit, so LTO is exposed through optional benchmark
compiler/linker flags rather than enabled as a claimed fix.

A Verilator-specific direct callback prototype bypassed exported-DPI scope and
callback lookup. Its serial million-iteration C++ runtime was about `3.194 s`
against the equal-LTO normal-path median of `3.217 s`, only about `0.7%`
faster. The backend coupling cannot close the remaining gap and was removed.

Profile-guided optimization trained each implementation independently on the
same workload and then rebuilt both with LTO plus its own profile. A
million-iteration serial viability check measured approximately `2.336 s` for
C++ DPI and `2.070 s` for pure SV, or `1.129x`. The 100,000-iteration paired
screen was order-sensitive at `1.118x` and classified as an invalid
environment. PGO improves both binaries but does not clear the guard, so it
was also rejected as a framework solution.

These experiments narrow the remaining issue: batching regresses, direct
probe lookup is too small, and compiler optimization benefits both sides. The
two explicit propagation boundaries and their generic scheduler/simulator
round trips remain the meaningful difference in `force_release`; force and
release themselves do not insert a delay.

## Immediate force-read cache and scoped waiver (2026-07-14)

A final generic optimization caches the value of a successful generated force
for an immediate matching read in the same outer DPI callback. The cache is
emitted only when compiled usage selects both operations for a path. Release,
the alternate two-state/four-state force mode, and the next outer callback
invalidate or bypass it, so later HDL activity remains observable. A directed
transport test covers cache hits, release invalidation, and callback-epoch
refresh. Unused hierarchy bindings emit no cache.

The exact `force_direct` semantics continued to match at 100,000 and 1,000,000
iterations. The million-iteration 32-pair guard improved from the historical
`1.652x` result to `1.135x` (DPI-first `1.146x`, SV-first `1.107x`, independent
median ratio `1.131x`, and `0.36%` paired/independent disagreement). The
remaining workload is two direct exported-DPI operations against equivalent
in-process SystemVerilog, with no scheduler work or time advance left to trim.

The registry now grants only `force_direct` a reviewed `1.20x` ceiling while
retaining its raw `1.10x` failure as a diagnostic. The waiver does not cover
semantic failures, invalid environments, missing results, or any other
feature. This records the transport limit without allowing the isolated
microbenchmark to block unrelated hierarchy work, and still catches future
regressions in the force path.
