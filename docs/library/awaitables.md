# Triggers and phase waits

<!-- api-headers: include/cpptb/coro_runtime.hpp -->

Everything a testbench can `co_await` to move simulation time, and the time
type behind it. All of it lives in `namespace cpptb::coro`; testbenches
conventionally write `using namespace coro;`. [Scheduling](../scheduling.md)
is the authoritative reference for *when* each wait resumes and what state
it observes; this page is the API surface.

Two rules apply to every awaitable here: `co_await` yields `void` unless a
return type is stated, and awaiting outside a scheduler-owned coroutine is
a runtime error that names the wait.

## Edge triggers

### RisingEdge
### FallingEdge
### Edge

```cpp
explicit RisingEdge(Signal signal);
explicit FallingEdge(Signal signal);
explicit Edge(Signal signal);        // either edge

co_await RisingEdge{dut.clk};
```

Resume on the named edge of a one-bit signal. Every constructor is
`explicit`, so the brace form above is the intended spelling. `co_await
RisingEdge{}` resumes *before* the design evaluates the edge — the monitor
instant; a driver that reads DUT outputs follows it with `ReadWrite{}`
(see [Coming from cocotb](../coming-from-cocotb.md#the-three-traps)).

## Phase waits

### ReadWrite
### ReadOnly
### NextTimeStep

```cpp
co_await ReadWrite{};      // evaluation settled; queued writes flush here
co_await ReadOnly{};       // end of the timestep; writing is illegal
co_await NextTimeStep{};   // start of the next timestep
```

The settle points of the current timestep, supplied by the project's
[timing backend](../cpptb-toml.md#build). One ordering rule is enforced at
run time: after `ReadOnly` has been reached in a timestep, awaiting
`ReadWrite{}` or `ReadOnly{}` again in that same timestep aborts with a
diagnostic — await `NextTimeStep{}`, a `Delay`, or an edge first.
`set()` from inside `ReadOnly` fails at the offending call site, exactly
as in cocotb.

## Delays and time

### Delay

```cpp
explicit constexpr Delay(SimTime duration);

co_await Delay{10_ns};
```

### SimTime

```cpp
struct SimTime {
    uint64_t femtoseconds = 0;
    constexpr uint64_t in_femtoseconds() const;
    constexpr uint64_t in_picoseconds() const;   // truncating
    constexpr uint64_t in_nanoseconds() const;   // truncating
};
// ==, <=> ordering, and operator+ are provided; there is no operator-.
```

Femtosecond-resolution value type. The literals are `_fs`, `_ps`, `_ns`,
`_us`, and `_ms` — integer-valued only (`1.5_ns` does not compile), and
there is no `_s`.

### clock_cycles

```cpp
Task<void> clock_cycles(Signal clock, uint64_t count);
```

Awaits `RisingEdge{clock}` exactly `count` times; `count == 0` completes
without suspending.

### wait_until

```cpp
Task<void> wait_until(SignalType signal, Predicate predicate, Signal clock);
```

Resumes when `predicate(signal.get())` holds, sampling on rising edges of
`clock`. The predicate is tested before the first wait, so an
already-true condition returns without suspending.

## Composition and timeouts

### First

```cpp
size_t winner = co_await First{RisingEdge{dut.clk}, Delay{100_ns}};
```

Races two or more *triggers* (edge, delay, or phase waits — not tasks) and
yields the zero-based index of the one that fired first, in argument
order. At least two triggers are required; the losing registrations are
removed when the winner fires.

### with_timeout

Two overloads with different result types. The trigger form accepts an
edge trigger only and yields a plain enum:

```cpp
Task<TimeoutOutcome> with_timeout(Trigger trigger, SimTime timeout);
// Trigger is RisingEdge, FallingEdge, or Edge

const auto outcome = co_await with_timeout(RisingEdge{dut.clk}, 3_ns);
if (outcome == TimeoutOutcome::TimedOut) { ... }
```

The task form wraps any `Task<T>` and yields a result object; on timeout
the child task and any waits nested inside it are cancelled recursively:

```cpp
Task<TimeoutResult<T>> with_timeout(Task<T> task, SimTime timeout);

const auto result = co_await with_timeout(read_response(dut), 100_ns);
if (result.timed_out()) { ... }
else use(result.value());
```

### TimeoutOutcome

```cpp
enum class TimeoutOutcome { Triggered, TimedOut };
```

A bare scoped enum — compare it, it has no member functions.

### TimeoutResult

```cpp
bool triggered() const;    // the task completed in time
bool timed_out() const;    // its negation
explicit operator bool() const;   // same as triggered()
T& value();                // the task's result; aborts after a timeout
```

`TimeoutResult<void>` carries the same predicates without a value. An
exception thrown by the child is rethrown at the `co_await` when the
child actually completed.

## See also

- [Tasks and coordination](coordination.md) — `Task`, `Process`, `Join`,
  and the queue/event/lock/semaphore primitives.
- [Scheduling](../scheduling.md) — the precise resume semantics behind
  every wait on this page.
