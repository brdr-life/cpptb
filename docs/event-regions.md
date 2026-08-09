# Simulator event regions

Every rule in these docs about *when* to read or write a signal — the
monitor and driver anchors in [Scheduling](scheduling.md), the
[write model](scheduling.md#the-write-model), the three
[translation traps](coming-from-cocotb.md#the-three-traps) — comes down to
one question: where inside a simulation time slot is your code standing?
SystemVerilog answers that question precisely. IEEE 1800-2017 clause 4
divides every time slot into ordered *event regions*, and every simulator
behavior, testbench framework, and VPI callback is defined by which region
it executes in. This page is the map; the framework pages cite it rather
than re-deriving it.

## A time slot, divided

Simulation time does not flow; it hops between discrete *time slots*.
Within one slot, events are not processed in arrival order — they are
sorted into regions, and the regions drain in a fixed order:

```
                  previous time slot
                           │
                           ▼
  ┌─────────────────────────────────────────────────┐
  │ Preponed      sampling only: the values signals │
  │               held as the slot began            │
  └────────────────────────┬────────────────────────┘
                           ▼
  ┌──── Active region set ──────────────────────────┐
  │                                                 │
  │   Active      blocking assignments, continuous  │◀─────────┐
  │               assignments, module/always code   │          │
  │   Inactive    #0-delayed assignments            │          │
  │   NBA         nonblocking (<=) updates commit   │          │
  │                                                 │          │
  └────────────────────────┬────────────────────────┘          │ any region can
                           ▼                                   │ schedule events
  ┌─────────────────────────────────────────────────┐          │ into an earlier
  │ Observed      concurrent assertions evaluate    │          │ region of the
  └────────────────────────┬────────────────────────┘          │ same slot; the
                           ▼                                   │ slot loops
  ┌──── Reactive region set ────────────────────────┐          │ until every
  │                                                 │          │ region is empty
  │   Reactive    program blocks, assertion action  ├──────────┘
  │               blocks: testbench-priority code   │
  │   Re-Inactive #0 delays in reactive code        │
  │   Re-NBA      nonblocking updates from reactive │
  │                                                 │
  └────────────────────────┬────────────────────────┘
                           ▼
  ┌─────────────────────────────────────────────────┐
  │ Postponed     $monitor, $strobe, read-only      │
  │               callbacks: final, settled values  │
  └────────────────────────┬────────────────────────┘
                           │
                           ▼
                    next time slot
```

The LRM defines more regions than the diagram shows — a PLI hook region
(Pre-Active, Pre-NBA, Post-NBA, and so on) sits between each pair above.
Those exist to give tool callbacks a named place to stand; the nine drawn
here are the ones with language-level meaning.

**Preponed** is sampling only. A value sampled here is the value the signal
held as the slot began, before anything in the slot ran. Clocking blocks
and concurrent assertions take their input samples here — which is why an
assertion never sees a half-updated cycle.

**Active** is where design procedural code runs: `always` blocks whose
triggers fired, continuous assignments re-evaluating, blocking assignments
applying immediately, `$display` printing. The order of simultaneously
runnable processes is *not* defined, which is where every classic
Verilog race lives.

**Inactive** holds `#0`-delayed assignments: work pushed behind the current
Active drain but still inside the slot. The LRM carries it for
compatibility; new code should not need it.

**NBA** is where nonblocking (`<=`) updates commit. This split is the
flip-flop illusion: every `always_ff` evaluates its right-hand sides in
Active, reading pre-edge state, and the writes land together in NBA. All
the flops in a design sample one consistent world, then that world updates.

**Observed** evaluates concurrent assertions, over the values sampled in
Preponed, once the Active set has gone quiet.

**Reactive**, **Re-Inactive**, and **Re-NBA** mirror the Active set for
testbench-priority code: `program` blocks and assertion action blocks. The
reactive set exists so verification code runs after the design has settled
within the current iteration — design first, testbench second.

**Postponed** is the end of the slot: `$strobe`, `$monitor`, and read-only
tool callbacks. Values here are final for this slot, and nothing is
permitted to write or to schedule further events into it.

The loopback on the right is the part diagrams usually omit. Reactive code
that writes a design signal schedules new Active-region events *in the same
slot*; the scheduler drains forward again, and the slot ends only when
every region is empty. Two consequences follow. "Settled" is a property of
*position*, not of time — the same simulation time offers different
guarantees depending on the region you read in. And a region can run more
than once per slot, so "after the Active region" means after an iteration
of it, not after the slot's last one; only Postponed sees the slot's final
answer.

## Where the waits anchor

One simulation instant is not one point; it is this whole ladder. The
trigger vocabularies of cocotb and cpptb are names for rungs on it:

| Instant | LRM location | cocotb | cpptb |
|---|---|---|---|
| the edge, before it evaluates | Active, before the edge's processes run — reads see what Preponed sampling sees | no equivalent | `co_await RisingEdge{}` |
| the edge, after it evaluates | value-change callback (`cbValueChange`), after the update | `await RisingEdge()` | `co_await RisingEdge{}` then `co_await ReadWrite{}` |
| settled, writes still legal | read-write synchronize (`cbReadWriteSynch`), after NBA updates commit | `await ReadWrite()` | `co_await ReadWrite{}` |
| end of slot, read-only | read-only synchronize (`cbReadOnlySynch`), Postponed | `await ReadOnly()` | `co_await ReadOnly{}` |
| start of the next slot | next-time callback (`cbNextSimTime`), before any evaluation there | `await NextTimeStep()` | `co_await NextTimeStep{}` |

Two rows deserve a note.

The first row is the instant `always_ff` itself experiences: right-hand
sides evaluated against pre-edge state. cpptb resumes an edge wait there,
which is why a bare `co_await RisingEdge{}` followed by `get()` is a
correct, deterministic monitor — it reads exactly what the design's own
flops are about to sample. cocotb has no name for this instant; its edge
trigger fires from the value-change callback, after evaluation, and how
settled the surrounding logic is at that callback is simulator-dependent —
which is why cocotb's own guidance is to sample after `ReadOnly`.

The write side has the same shape. With the
[write model](scheduling.md#the-write-model) enabled, a cpptb `set()`
queues and flushes at the `ReadWrite` point — your testbench writes behave
like nonblocking assignments made from the reactive world, exactly as a
cocotb assignment does. `set_now()` and `force()` are the blocking-world
escape hatches.

## What a simulator actually executes

No simulator walks this diagram literally. Event-driven simulators merge
and reorder regions where the result is indistinguishable, and Verilator
schedules statically at compile time, evaluating the design to convergence
rather than draining queues. The diagram is the *contract*: whatever the
execution strategy, the observable guarantees at each named point must be
the ones above. That is how cpptb treats it — the timing backends
reproduce the anchor points rather than the machinery, and the conformance
suite plus the backend identity gate (`verilator-direct` and `vpi` held to
identical results) pin them. See
[Scheduling](scheduling.md#timing-backend-support) for what is checked.

## Why this matters here

- [Trap 1](coming-from-cocotb.md#1-risingedge-resumes-before-the-edge-evaluates)
  is the first two table rows read together: a driver that reads protocol
  pins wants the post-evaluation rung, so it anchors on the edge *and* the
  settle point.
- The monitor shape in [Scheduling](scheduling.md#coming-from-cocotb) —
  bare edge, then `get()` — is the Preponed-equivalent rung, and needs no
  `ReadOnly`.
- Deferred writes flushing at `ReadWrite` is the reactive-to-Active
  loopback in the diagram: the write lands after this edge's own updates,
  in time for the next.
- `ReadOnly` forbidding writes is Postponed's rule, not a framework
  restriction.

## Next

[Scheduling](scheduling.md) is the authoritative reference for how each
cpptb wait maps onto these points, and
[Coming from cocotb](coming-from-cocotb.md) applies them to translation,
including [why the edge instant differs](coming-from-cocotb.md#why-the-difference-is-kept)
from cocotb's on purpose.
