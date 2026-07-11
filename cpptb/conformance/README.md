# Scheduler conformance suite

This suite defines the portable behavioral contract between the C++ coroutine
scheduler, generated SystemVerilog DPI wrapper, and a simulator. It is kept
separate from examples and performance benchmarks so a backend can pass the
same contract before it is advertised as supported.

The suite covers:

- generated, C++-driven, and DUT-derived clocks;
- rising, falling, and any-edge delivery;
- simultaneous clock events without specifying cross-clock callback order;
- exact picosecond/nanosecond delays, timeout races, and stale registrations;
- variadic `First`, nested task continuations, and variadic `Join`;
- spawned process completion, awaiting, status, cancellation, and recursive
  nested-task cleanup;
- process-handle lifetime after scheduler destruction and invalid-process
  awaits;
- cooperative self-cancellation and cancellation/completion races;
- deterministic FIFO registration order for same-edge waiters and timer ties;
- stale edge and timer registration cleanup;
- sequential and combinational output settling after an explicit delay;
- typed `Task<T>` value propagation, prompt child-frame reclamation, move-only
  non-default results, and recursive typed-child cancellation;
- zero, one, and many `clock_cycles()` waits on generated, testbench-driven,
  and DUT-derived clocks;
- edge-triggered `with_timeout()` edge and timeout outcomes, including both
  stale loser directions and a same-timestamp edge/deadline tie;
- arbitrary `Task<T>` and `Task<void>` timeouts, move-only non-default values,
  same-deadline task completion, recursive loser cancellation, and wait
  cleanup;
- immediate and delayed `wait_until()` predicates with exact evaluation counts
  and timestamps;
- sticky, reusable `Event` state, FIFO wakeup, and cancelled-waiter cleanup;
- unbounded `Channel<T>` item/waiter FIFO behavior, producer/consumer ordering,
  and cancellation without item loss;
- rejection of writes to DUT outputs.

An edge resumes at the edge's timestamp. A subsequent `Delay{1_ps}` advances
physical simulation time by exactly one picosecond and allows sequential and
combinational logic to settle before the coroutine reads outputs. The wrapper
transports time in its declared `timeprecision`; a duration below that
precision is rejected rather than rounded silently. Zero-duration delays are
also rejected so they cannot accidentally imply delta-cycle semantics.

The equivalent pure SystemVerilog ordering is ordinary procedural code:

```systemverilog
@(posedge clk);
#1ps;
check(dut.response);
dut.request = next;
```

The C++ equivalent is deliberately literal:

```cpp
co_await RisingEdge{dut.clk};
co_await Delay{1_ps};
check(dut.response.get());
dut.request.set(next);
```

Reads and input writes are legal whenever a coroutine is running. Driving a
DUT output remains an error. The framework does not label a physical delay as
a simulator scheduling phase.

`First` invalidates its losing trigger registration; the suite checks both a
delay timeout and an edge winner whose stale timeout shares a later deadline.
Stale edge registrations are cleaned as well, including their contribution to
falling-edge interest. Same-deadline timers resume FIFO by registration order.

## Authoring Core v1

`Task<T>` moves a completed value through each nested `co_await`. A typed child
frame is reclaimed before its parent continues, while the moved result remains
alive in the parent. The contract includes a move-only, non-default-constructible
result. Cancelling a `Process` whose `Task<void>` root awaits a typed child
recursively destroys the typed child frame, produces no value, and reports the
outcome through the root `Process::cancelled()` status.

`clock_cycles(clock, count)` waits for exactly `count` rising edges. A zero
count completes immediately without registering an edge wait. The suite checks
one generated-clock cycle and multiple cycles across generated,
testbench-driven, and DUT-derived clocks.

`with_timeout(edge, duration)` supports edge triggers in Authoring Core v1 and
returns `TimeoutOutcome::Triggered` or `TimeoutOutcome::TimedOut`. The earlier
event wins, and the losing registration is invalidated. For a simultaneous
edge and deadline, the outcome depends on which callback the simulator delivers
to the host first and is deliberately unspecified. Conformance creates such a
tie and accepts either outcome, while requiring exactly one completion and no
later resume from the stale loser; it does not pin simulator process order.

`with_timeout(task, duration)` returns `TimeoutResult<T>`. A completed result
supports optional-style state and value access; `TimeoutResult<void>` carries
the same completion/timeout state without value storage. The optional-backed
typed result supports move-only, non-default-constructible values. A timed-out
task is recursively cancelled and reclaimed at the normal scheduler boundary,
including nested task frames and `Process`, `Event`, or `Channel` waits. Task
completion wins a same-timestamp race when its result exists by the time the
parent resumes. A stale loser cannot resume later, and invalid tasks, invalid
value access, zero durations, and sub-precision durations abort with explicit
diagnostics.

`wait_until(signal, predicate, clock)` evaluates once immediately, then once
after each rising clock edge while the predicate is false. The delayed case
evaluates exactly at 0 ns, 2 ns, and 6 ns, after a testbench-driven predicate
input changes at 3 ns. A predicate that is initially true evaluates once and
does not register an edge wait.

`Event::set()` is sticky until `clear()`. A waiter arriving after `set()`
completes immediately; `set()` wakes already-registered waiters in FIFO order.
The same event can be cleared and reused. Cancelled waiters are removed and do
not resume on a later set.

`Channel<T>` is an unbounded FIFO in Authoring Core v1. Puts may precede gets or
vice versa, queued items and waiting consumers preserve FIFO order, and
multiple delayed producers preserve production order. Cancelling a consumer
that has been assigned an item transfers availability to the next waiter, so
the item is neither lost nor delivered to the cancelled process. Moving a
`Channel<T>::GetAwaiter` transfers responsibility for its active registration;
destroying the moved-from awaiter neither abandons that registration nor
consumes an item. Reentrant `put_nowait()` and cancellation from a resumed
consumer preserve reserved-item handoff and exactly-once wakeup while external
wakes are being flushed. Bounded channel behavior is outside the v1 surface.

Destroying an `Event` or `Channel<T>` with an active waiter aborts with a
diagnostic. The conformance runner checks both lifetime violations in addition
to sub-precision delay, output-write, zero-delay, and task-timeout negative
cases.

## Process contract

`spawn()` transfers ownership of a root `Task` to the scheduler and returns a
copyable `Process` handle. All copies refer to the same scheduler-owned process
state; a handle neither owns the coroutine frame nor keeps the `Testbench`
alive. The root frame is reclaimed promptly after normal completion or
cancellation. `spawn_detached()` transfers the same task ownership but creates
no process handle or lifecycle record. It is intended for fire-and-forget
roots. Tracking adds work at spawn and final completion, not on each resume.

For a live handle, `done()` becomes true after either normal completion or
completed cancellation. `cancelled()` becomes true only when cancellation has
actually been applied, and therefore always implies `done()`. Cancellation
that loses a race to normal completion does not relabel the completed process
as cancelled. Awaiters wake for either outcome, and awaiting an already-done
process completes immediately.

Cancellation recursively destroys an active root's nested awaited task or
`Join` children. Self-cancellation is cooperative: a running coroutine may
request it safely, continues until it next yields or returns, and is cancelled
at that scheduler boundary. Before the boundary, the request alone does not
make either `done()` or `cancelled()` true.

Destroying the `Testbench` invalidates all surviving `Process` handles without
leaving dangling scheduler access. On such a handle, `valid()`, `done()`, and
`cancelled()` return false and `cancel()` does nothing. Awaiting an invalid
handle aborts with a diagnostic. Delay registration likewise aborts for zero,
sub-precision, or otherwise unrepresentable durations rather than rounding or
silently accepting them.

Run the configured backend:

```sh
make cpptb-conformance-run
```

The exact positive result contract is 210 checks, eight primary generated-clock
cycles, and zero failures. These values and all negative diagnostics are
declared in `scheduler_conformance.dpi.json`.

The simulator and its compile arguments are selected by `simulator` and
`simulator_options` in `scheduler_conformance.dpi.json`. Verilator is the first
implemented runner. Additional simulator runners should consume the same RTL,
generated wrapper, C++ testbench, and exact result contract.
