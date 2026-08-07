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
(`1.0517x`), and queue (`1.0399x`) were `passed_inconclusive` after 32 pairs:
their medians passed, but environmental variance left the one-sided 95% upper
median bounds above `1.10x`.

The focused task-value run exercised the new confirmation path and passed at
`1.0142x` after 32 pairs. Its later full-regression sample passed at `1.0138x`
after the initial 16 pairs, so the saved `latest` artifact contains that newer
measurement.

## register_coverage A/B against the coverage.hpp restructure (2026-08-07)

The `register_coverage` kernel had no published ratio (the roadmap's "timing
publication awaits a host-load window") when the coverage.hpp Bin restructure
landed, and an early post-restructure sample read `2.376x`, which raised the
question of whether the restructure was the cause. A same-window A/B — the
pre-restructure header from `72515dc~1` rebuilt and measured back-to-back with
the current one — answers it:

| Header | Paired medians | Independent median | Strata disagreement |
| --- | --- | --- | --- |
| pre-restructure | `2.674x` / `2.891x` | `2.752x` | `47.20%` |
| current | `2.682x` / `2.783x` | `2.716x` | `2.22%` |

Both windows were classified `invalid_environment` by CPU corroboration (a
handful of samples on a shared host), so neither number is publishable — but
the comparison is: the restructure did not move the kernel (`2.752x` →
`2.716x`, inside noise), and the elevated absolute ratio exists with the old
header too. Whatever makes this exact pair ~2.7x predates the restructure and
was never published as anything lower. Publishing a certified ratio for this
kernel still awaits an admitted window, and if ~2.7x survives one, the kernel
needs either optimization of the passive-coverage observation path or a
documented waiver — the `hard_1_10` gate will not pass it as-is.

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

## Timing-phase dispatch profile (2026-07-14)

The exact `timing_phases` twin isolates one falling-edge wait, one
`ReadWrite`, one `ReadOnly`, one `NextTimeStep`, two combinational writes, and
two settled reads per iteration. The initial 100,000-iteration release guard
measured `1.675x` C++ DPI/pure SV.

Compile-gated telemetry showed 600,004 runtime steps, 400,000 phase callback
registrations, 400,008 model evaluations, and 200,008 simulator timesteps.
Half of the read/write callbacks were a historical settle barrier made
redundant by the generated `phase_outputs_pending` event. Removing that
barrier reduced the instrumented runtime by `11.7%` and the release guard to
`1.413x` while preserving all checks.

A macOS CPU sample then identified repeated `vpi_register_cb` allocation,
Verilator VPI TLS lookup, and empty generic VPI callback scans as the dominant
framework-specific work. Skipping unused value, timed, start-of-slot, and
end-of-slot callback classes in the framework-owned host loop reduced the
guard to `1.337x`. The opt-in full VPI loop remains available for executables
that load unrelated external VPI clients.

The retained Verilator direct backend polls pending phase bits at the same
host-loop regions and invokes the generated phase dispatcher directly. It
does not change the public waits or generated SV scheduling contract. The
portable backend continues to use standard `cbReadWriteSynch`,
`cbReadOnlySynch`, and `cbNextSimTime` callbacks. Both backends pass the full
275-check conformance suite, all five focused positive cases, and all 14
negative diagnostics.

Direct dispatch reduced phase callback registrations from 300,000 to zero in
the 100,000-iteration workload and moved the exact guard to `0.861x`.
Consolidating three callback-safety thread-local variables into one state
object then reduced repeated macOS TLS resolver calls and produced the final
`0.834x` paired result (`0.823x` DPI-first, `0.834x` SV-first, `0.835x`
independent, `0.17%` disagreement).

The post-change profile is dominated by Verilator model evaluation, the
generated SV clock-delay coroutine, phase-export dispatch, and packed output
copying. Scheduler parking, ready draining, edge resumption, and phase
resumption are visible but individually small. Fully demand-gating rising
clock crossings is parked: configured clocks intentionally use static edge
sources, and suppressing an edge also requires preserving exact clock
readback in later read/write and read-only phases. The measured path already
passes the hard guard, so that broader clock-shadow change is not justified
without a dedicated clock-idle workload and conformance contract.

## Repeated dynamic-spawn profile (2026-07-16)

The exact `dynamic_spawn` pair creates and immediately awaits one independently
owned process per iteration. Its pure-SV twin uses `fork ... join`. At
5,000,000 iterations the initial valid paired result was `1.823x` C++ DPI over
pure SV, exposing a regression that shorter startup-dominated runs had not
made stable.

The generated Verilator implementation explains the unusually large gap. The
zero-time SV child has no timing control, so Verilator emits its check directly
inside the parent loop and creates no child coroutine. The generic C++ path
creates a coroutine frame, process control, scheduler state, provenance,
cancellation ownership, and completion state because the child may suspend or
fail and the returned handle remains observable.

Five retained changes reduced the paired result to `1.439x`:

1. An immediately awaited child starts directly when it is the only remaining
   runnable process. Existing runnable work preserves FIFO order, a child that
   suspends falls back to the ordinary scheduler, and work woken by the child
   runs before its awaiting parent.
2. Successful coroutines carry a null lazy exception state. The comparatively
   expensive `std::exception_ptr` is allocated only after a throw, preserving
   exception attribution and rethrow behavior without taxing passing checks.
3. Empty completion-waiter and single finished-root cases bypass their generic
   vector paths.
4. Test lifecycle ownership uses scheduler-confined non-atomic references.
   This reduced the paired result from `1.500x` to `1.460x` while preserving
   detached-process lifetime and exception attribution.
5. A live-ready counter replaces two ready-state scans in the inline-await
   path. Initial confirmation runs measured `1.455x` and `1.450x`; the final
   post-regression formal batch measured `1.439x`.

The inline optimization is capped at 64 nested host resumptions. A 10,001-level
regression proves deeper zero-time process trees fall back to the ordinary
ready queue without overflowing the C++ stack or changing completion order.

A process-global frame pool improved a 100,000,000-iteration screen by only
about 3% and was rejected because independent schedulers on different OS
threads would share unsynchronized storage. Intrusively unlinking lifecycle
ownership at completion was about 1% slower and was also removed. Keeping
pooled `ProcessControl` objects constructed and resetting them in place
measured `1.486x` and `1.505x` on top of the ownership improvement, versus its
`1.460x` reference, so that experiment was removed as well.

All unit tests, the 275-check conformance suite, focused timing cases, and 14
negative diagnostics pass. The ordinary `test_lifecycle` pair passes at
`0.983x`, the direct-coroutine `task_value` pair at `0.831x`, and the complete
authoring aggregate at `0.797x`. At this stage, the remaining hard failure was
specific to repeatedly requesting an independently owned process for work that
the SV compiler can prove is sequential. Direct `co_await` remains the fast
path for sequential helper composition; `spawn()` remains the correct API for
real concurrency, cancellation, and independent process attribution.

## Exact process-cost decomposition (2026-07-16)

The earlier `dynamic_spawn` profile mixed scheduler mechanics with test
lifecycle ownership. Four exact C++/pure-SV pairs now isolate the layers at
5,000,000 iterations:

| Pair | C++ DPI median | Pure SV median | Paired ratio |
| --- | ---: | ---: | ---: |
| `dynamic_task` | 23.03 ns | 3.26 ns | `7.052x` |
| `dynamic_spawn_scheduler` | 43.88 ns | 40.88 ns | `1.080x` |
| `dynamic_spawn` | 49.88 ns | 41.39 ns | `1.199x` |
| `dynamic_spawn_suspending` | 181.35 ns | 272.55 ns | `0.661x` |

The time columns are independent per-mode medians. The ratio column is the
median of adjacent paired ratios and is therefore not computed by dividing the
two displayed medians.

The low-level scheduler pair and lifecycle-tracked pair use the same passing
`TestContext::expect_eq()` child body. The only C++ difference is independent
test ownership and diagnostics. Their pure-SV twins both use the same
one-child `fork ... join`. The suspending pair uses two processes connected by
an event handshake in both languages. All four pairs match exact counters and
checksums.

The retained architecture removes the lifecycle-owned `Process` vector and
its per-completion scan. The scheduler remains the source of truth for live
process controls. A compact `ProcessProvenance` record carries test ownership
and diagnostic source information, comes from an intrusive free list, and
retains the test state until an exactly-once completion finalizer runs.
Cancellation scans scheduler controls only on the cold finish/error path.

Before that change, the normalized tracked pair measured 56.68 ns against
41.57 ns pure SV (`1.365x`). The retained implementation measures 49.88 ns,
a 12.0% C++ improvement. Its lifecycle layer is now about 6.00 ns over the
43.88 ns core scheduler path. The total remaining delta from pure SV is about
8.49 ns.

The following experiments were removed after paired screens:

| Experiment | Result |
| --- | --- |
| Force TLS `local-exec` | Core scheduler regressed to 48.95 ns (`1.192x`) |
| Namespace `constinit thread_local` pool | Core scheduler regressed to 46.17 ns (`1.144x`) |
| Execution context in every process control | Core scheduler regressed to 46.03 ns |
| Split/pooled process-control sidecar | Core scheduler remained at 46.55 ns and tracked spawn at 52.49 ns |

A lazy scheduler-adoption path was not implemented. The exact core pair proves
that scheduler adoption is already within the `1.10x` guard, so bypassing it
would add semantic complexity while targeting the wrong layer. Matching the
compiler-inlined zero-time SV case would require either weakening lifecycle
and attribution semantics or introducing a specialized sequential operation.
Neither belongs in the general `TestContext::spawn()` contract. Real
suspending concurrency already amortizes the fixed bookkeeping and is faster
than the exact pure-SV twin in this benchmark.

## Lifecycle-owned completion optimization (2026-07-18)

A second profile separated successful completion from exceptional completion
and showed that the remaining tracked-spawn cost came from normal-path test
state finalization, not scheduler adoption. The retained architecture makes
four changes without weakening process semantics:

1. Passing `expect()` and `expect_eq()` calls are always-inline counter updates;
   diagnostic formatting and result construction remain cold failure paths.
2. Lifecycle execution context is embedded only in `OwnedProcessControl`, with
   a separate pool, so the generic scheduler control does not pay for test-only
   provenance fields.
3. Successful lifecycle-owned processes dismiss their completion callback.
   Exceptional completion still reports through the owning test, while a
   lazily installed process-resource hook removes a random stream when one was
   actually used.
4. Test state is owned by `TestContext` handles. If the last handle disappears
   while owned work remains, the scheduler cancels that work and performs one
   deferred owner cleanup after the last process is reclaimed. This replaces a
   retain/release pair on every spawn with teardown-only bookkeeping.

The historical July 18, 2026 serial rerun under the earlier admission policy
produced:

| Pair | C++ DPI median | Pure SV median | Paired ratio |
| --- | ---: | ---: | ---: |
| `dynamic_task` | 21.71 ns | 1.37 ns | `15.764x` diagnostic |
| `dynamic_spawn_scheduler` | 43.60 ns | 40.34 ns | `1.072x` |
| `dynamic_spawn` | 41.22 ns | 38.96 ns | `1.067x` |
| `dynamic_spawn_suspending` | 166.71 ns | 279.08 ns | `0.601x` |
| `test_lifecycle` | 3.58 ns | 3.61 ns | `0.983x` |

All then-hard-gated pairs measured below the `1.10x` limit. These rows predate
the current source/configuration provenance and load-admission policy and are
retained as optimization history, not formal gate results. The direct-task pair
remains a diagnostic control because Verilator compiles its zero-time SV task
into the parent loop while C++ must construct a coroutine frame.

Rejected follow-up experiments included bypassing the coroutine frame pool
(`1.595x`), duplicating a dedicated owned-spawn implementation (`1.492x`), and
removing provenance fields (`1.405x`). These results reinforced that code
layout and pooled allocation matter more than the small metadata copies.

## Fresh-build audit and measurement hardening (2026-07-18)

An independent Fable review found that the formal `1.179x` `dynamic_spawn`
failure had used `--skip-build` on a dirty tree. The measured C++ binary hash
was `22e0fc38...`; both scheduler headers were modified after that executable
was produced. A clean rebuild generated `b0170d2d...`, and exact 100,000-
iteration semantic checks passed for lifecycle-owned and scheduler-only spawn.

The first fresh 5,000,000-iteration lifecycle-owned run improved to `1.131x`.
Its child-CPU ratio was approximately `1.135x` and no half-speed samples were
present, so CPU evidence corroborates a small remaining cost. The host reached
`0.449` normalized one-minute load, however, so the tightened policy does not
accept this as a formal failure. A separate scheduler-only screen measured
`1.085x` wall and `1.086x` child CPU. It confirms that generic process
scheduling remains within the hard limit while formal lifecycle adjudication
waits for an admitted host.

The runner now:

1. fingerprints every declared binary source, build flag, compiler, and
   Verilator version beside the executable during the Make build;
2. rejects `--skip-build` if the source, executable, build configuration, or
   toolchain fingerprint differs;
3. serializes generated-input checks and builds without inherited make job
   state;
4. refuses performance measurement above `0.30` normalized one-minute load;
5. records paired child-CPU statistics beside process wall time; and
6. invalidates the complete run, without filtering samples, if wall and CPU
   ratios differ by more than 5% or a sample differs by more than 25% from its
   implementation's CPU median.

An initial threshold crossing with order-sensitive diagnostics now receives the
same one-time fixed confirmation batch as other threshold decisions. If the
order effect remains, the final result is invalid rather than being mislabeled
as a formal failure. Any invalid environment result exits nonzero, including a
raw ratio that would otherwise pass.

A five-second profile of 200,000,000 lifecycle-owned spawns found distributed
cost in parent coroutine work, inline process startup, scheduler adoption,
thread-local frame-pool access, child destruction, finish, and reclamation.
No remaining local operation accounts for the complete approximately 2 ns
lifecycle premium over the scheduler-only pair. New isolated screens produced:

| Experiment | Experiment / fresh baseline | Decision |
| --- | ---: | --- |
| Move rare multi-waiter overflow storage out of every control | `0.999x` | Remove; neutral |
| Bypass the finished-root queue for inline completion | `1.034x` | Remove; regression |
| Pass the structured child `TestContext` by reference | `0.999x` | Restore stronger by-value lifetime |
| Launch both modes with `taskpolicy -t 0 -l 0` | `1.012x` | Do not use; regression and no affinity guarantee |
| Remove owned-path cold branch hints | `1.018x` | Remove; regression |

An rvalue `Process` await overload was not used as a performance fix. The exact
benchmark awaits a named lvalue handle, so an rvalue overload would not affect
it; borrowing an lvalue across suspension would weaken the existing lifetime
guarantee. The immediate `dynamic_spawn` pair is now a diagnostic fixed-cost
control rather than a release gate. Formal lifecycle adjudication uses
source-stamped, low-load runs of realistic work: `dynamic_monitor` for
persistent observers and `process_pipeline` for finite driver, worker, and
scoreboard processes.

## Long-lived monitor benchmark (2026-07-16)

The `dynamic_monitor` pair adds the realistic case missing from the repeated
process-creation decomposition. It creates two lifecycle-owned observers once,
runs them for 100,000 DUT responses, passes sampled data through a bounded
queue/mailbox, and cancels both processes after foreground traffic completes.
The result schema now records `spawned_processes`, making direct task
composition (`0`), repeated spawn (`N` or `2N`), and persistent monitors (`2`)
distinguishable in every semantic comparison.

The pair matched two spawned processes, 100,000 transactions, 100,000 queue
puts and gets, 100,003 checks, 500,003 cycles, and checksum `2854112901`.
Its final valid 16-pair run passed at `0.744x` C++ DPI over pure SV (`0.742x`
DPI-first, `0.748x` SV-first, `0.742x` independent, and `0.29%` disagreement).
This confirmed that the fixed lifecycle cost amortized cleanly for persistent
verification processes. At that stage, the remaining hard performance concern
was repeated creation of zero-time lifecycle-owned children, not ordinary
monitor usage. The July 18 completion optimization reduced that cost, while
the fresh-build audit above leaves its formal low-load gate pending.

Semantic-only runs now write `semantic.json`, `semantic.md`, and
`semantic.jsonl`. They no longer replace the `latest.*` artifacts used as the
performance baseline.

## Finite process pipeline benchmark (2026-07-18)

The `process_pipeline` pair covers finite, naturally completing verification
processes rather than repeatedly creating zero-time children. A driver publishes
expected responses and drives the DUT, a worker samples each response edge, and
a scoreboard consumes both streams. The three processes communicate through
two capacity-eight queues/mailboxes, suspend throughout the run, and are joined
after 100,000 transactions.

At 10,000 iterations, the exact C++ and pure-SV implementations matched 10,000
transactions, 10,004 checks, three spawned processes, 20,000 queue puts, 20,000
queue gets, 50,003 cycles, checksum `833540549`, and zero failures. This pair is
a hard `1.10x` framework gate. A formal performance result must pass the current
source-provenance, child-CPU, and normalized-load admission checks.

## Transaction analysis fan-out (2026-07-16)

The `analysis_fanout` pair covers synchronous publication from one persistent
monitor to an in-order scoreboard and a bounded audit buffer. At 100,000 DUT
transactions, C++ and pure SV matched 200,000 publications, 300,000 subscriber
deliveries, 100,006 checks, one spawned process, cycle count, and checksum.

The valid 16-pair run passed at `0.712x` C++ DPI over pure SV (`0.719x`
DPI-first, `0.707x` SV-first, `0.716x` independent, `0.54%` disagreement).
This establishes a retained performance baseline for the transaction endpoint
layer under the repository's `1.10x` hard guard.

## Generated register-memory batching (2026-07-18)

The exact `register_memory` pair performs one four-entry semantic backdoor
write, one four-entry read, six checks, and four checksum updates per
iteration. Neither implementation advances simulation time or issues a bus
transaction. A 100,000,000-iteration diagnostic initially measured
`10.741 s` for C++ DPI and about `4.12 s` for pure SV, or roughly `2.61x`.

Sampling attributed the C++ cost to two avoidable layers: every semantic
backdoor operation created and reclaimed a child coroutine, and every four-word
span crossed the exported DPI boundary eight times. The retained changes are:

1. `RegisterMemoryHandle` returns an immediate-ready awaitable for backdoor
   operations and a normal `Task` for frontdoor operations. User code keeps the
   same `co_await memory.read/write(...)` form, including runtime path choice.
2. Generated one-dimensional memories up to 64 bits wide expose standard-DPI
   packed-vector block exports. Each crossing carries up to four adjacent
   elements; larger spans are chunked and tails use an explicit count.
3. Descriptor invariants are validated once at handle construction. Bounds,
   address overflow, missing-backdoor, callback-phase, and nonzero HDL-bound
   diagnostics remain checked, with their formatting moved off the hot path.

The immediate-ready operation reduced the 100,000,000-iteration C++ runtime to
`5.768 s`, about 46% below the original C++ result. The generated packed block
transport and guard cleanup reduced a later high-load diagnostic run to
`2.344 s`. That last raw run is not a formal C++/SV result because host load
exceeded admission limits; the serial paired guard remains authoritative.

A standards-compliant open-array experiment used `svOpenArrayHandle` and
`svGetLogicArrElem1VecVal`/`svPutLogicArrElem1VecVal` callbacks. It preserved
the exact workload but regressed C++ to `18.874 s`, so it was removed. The
retained packed-vector ABI uses no Verilator internals, simulator-specific
hooks, heap allocation, or implicit delay.

Regression coverage includes immediate-ready backdoor awaiters, scalar
frontdoor behavior, partial frontdoor failures, generated block exports,
seven-entry runtime transfers chunked as `4 + 3` through an HDL array declared
`[2:8]`, fallback semantics, exact C++/SV counters, and the unchanged `1.10x`
hard guard. The
default formal workload is 10,000,000 iterations to keep process startup from
dominating this zero-time kernel.

## Register user-effect policy hot path (2026-07-19)

The exact `register_user_effects` pair performs two frontdoor transactions and
four value/validity checks per iteration. A 10,000,000-iteration diagnostic
started at `3.362 s` for C++ DPI versus a `0.400 s` pure-SV median (`8.40x`).
Sampling attributed the initial C++ time to repeated register metadata work,
per-bit virtual policy calls, nested forwarding coroutines, lock bookkeeping,
and coroutine-frame pool TLS lookup.

The retained changes were measured independently before combination:

1. Register descriptors cache validated widths, masks, transfer counts, and
   access flags; frontdoor read/update loops no longer pass through redundant
   forwarding coroutine frames.
2. User-effect policies may process an entire field up to 64 bits per virtual
   call. Default field methods retain source compatibility by delegating to the
   existing bit callbacks.
3. A successful single-transfer update commits the reachability prediction
   already computed before transport instead of invoking the policy a second
   time. Split and failed transfers retain incremental commit semantics.
4. Coroutine frames use one process-wide pool under cpptb's documented
   simulator-thread confinement contract. Multi-simulator embeddings can define
   `CPPTB_CORO_THREAD_LOCAL_FRAME_POOL` to retain per-thread pools.
5. The uncontended register lock bypasses waiter and handoff cleanup.

The packed field callbacks reduced the C++ median from `1.825 s` to `1.202 s`.
The process-global pool reduced it to `1.026 s` (`14.6%`), and single-transfer
prediction commit reduced it to `0.994 s` (`3.1%`). The final C++ kernel is
`70.4%` faster than the original and has a raw `2.49x` ratio to pure SV.

A final five-second sample collected 3,842 on-CPU stacks after all retained
changes. Top-of-stack counts were 705 in register update, 522 in the benchmark
root, 484 in coroutine adoption, 402 in awaited-task reclamation, 394 in
register read, 341 in coroutine finish, and 171 in read prediction. The three
packed policy callbacks together accounted for only 43 samples (`1.1%`), and
thread-local pool lookup no longer appeared. The remaining cost is distributed
coroutine and register-abstraction work; it is not simulator transport or one
new dominant helper.

Rejected experiments include an ELF `initial-exec` TLS hint, which regressed
the C++ median from `1.825 s` to `2.071 s` (`13%`). Lazy task adoption was not
attempted: cancellation currently relies on eagerly adopted parent/child
ownership, so a correct implementation would require parent-chain adoption at
every park, exception, join, timeout, and reclaim boundary.

An independent Fable profile review agreed that the remaining samples are
coroutine scheduling, ownership, and virtual-policy abstraction rather than DPI
transport. It estimated a `2.2x` to `2.9x` floor for this zero-time kernel in its
current abstraction shape. Fable recommended retaining the bare-SV pair as an
absolute abstraction-tax diagnostic, adding an equivalent-abstraction SV peer
for framework-specific comparison, and using timed bus workloads for the
repository-wide `1.10x` goal. A formal paired rerun was rejected before sampling
because normalized one-minute load was `0.934` against the `0.300` admission
limit. The direct timings remain diagnostic and the hard guard is unchanged.

## Isolated experiment provenance (2026-07-25)

The six experiments in *Isolated scheduler experiments (2026-07-12)* were each
developed on their own branch. Those branches were removed once the accepted
work had landed, so this section records what each contained, what became of
it, and where the surviving implementation lives, which the branch names alone
no longer answer.

Each entry was verified against `main` by locating the implementation, not by
trusting the earlier write-up.

| Experiment | Branch (tip) | Standalone result | Where it lives in `main` |
| --- | --- | --- | --- |
| Static edge sources | `exp/static-clock-bookkeeping` (`6da371b`) | guard passed `1.092x` | `static_edge_source` in `include/cpptb/coro_runtime.hpp` |
| Clock-agnostic timer owner | `exp/generic-timer-dispatch` (`a4f4e69`) | `1.067x` | `next_timer_deadline`, `timer_generation`, `discard_stale_timers`, `cpptb_dpi_next_timer_deadline` |
| Opt-in on-demand transport | `exp/ondemand-bulk-transport` (`c01f2fa`) | `0.913x` array_wide, `0.978x` 137-bit echo | `on_demand` paths in `tools/codegen/` |
| Static generated signal bindings | `exp/static-signal-binding` (`8384af6`) | `0.991x` array_multidim, `0.948x` signal_edge | `dpi_static_binding` |
| Single-edge park path | `exp/single-edge-fastpath` (`c97e589`) | `0.992x` A/B, standalone guard crossed at `1.105x` | `single_edge_park_counts_` and `single_edge_park_count()` |
| One scheduler boundary per DPI step | `exp/single-step-boundary` (`ad7199c`) | rejected | not present, deliberately |

### Why the rejected experiment stays rejected

The single-boundary change drained timer and edge notifications together
instead of separately. That is what made it faster, and it is also what broke
the equal-time contract: a coroutine resumed by a timer could no longer arm an
edge wait in time to observe an edge arriving from the same DPI step. The cost
is a silently different scheduling semantic rather than a failing test, so
anyone reaching for this again should expect to redesign the contract, not
merely re-measure. It touched `coro_runtime.hpp`, `dpi_runtime.hpp`, and added
scheduler-boundary counters behind `CPPTB_SCHEDULER_BOUNDARY_DIAGNOSTICS`.

### Two integration lessons worth keeping

Two of the accepted experiments could not be merged as written. Static signal
bindings needed a transport-aware rewrite so on-demand signals could not be
mistaken for packed-array offsets, and the single-edge park path failed its own
standalone guard at `1.105x` and was admissible only because the combined stack
passed. A standalone measurement is therefore evidence that an idea is worth
integrating, not evidence that its implementation is.

The integrated stack measured `0.940x` against the checkpoint C++ binary and
`1.039x` against the exact pure-SystemVerilog twin; adding tuned on-demand
transport moved those to `0.936x` and `1.046x`.

### On reviving any of this

The branches predate the repository reorganisation: they carry paths such as
`cpptb/coro_runtime.hpp` and `cpptb/codegen/`, where `main` now has
`include/cpptb/` and `tools/codegen/`. They also sat 41 commits behind. Between
that and the two rewrites above, reimplementing from the reasoning recorded here
is likely cheaper than rebasing the original commits.
