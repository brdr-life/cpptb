# Tasks and coordination

<!-- api-headers: include/cpptb/coro_runtime.hpp -->

The types that make coroutines cooperate: the task and process handles, and
the four coordination primitives. All in `namespace cpptb::coro`.
[Tasks and concurrency](../testbench-authoring.md) teaches the patterns;
this page is the API surface.

Shared properties of `Event`, `Queue`, `Lock`, and `Semaphore`: all four
are non-copyable, all four take an optional `name` used in wait-graph and
deadlock reports, and destroying one while coroutines are still parked on
it is a runtime error naming the type.

## Handles

### Task

```cpp
template <typename T> class Task;   // the coroutine return type
```

Any coroutine in a cpptb testbench returns `Task<T>` (usually
`Task<void>`). Move-only. `co_await` yields the task's `T` and propagates
an exception thrown inside the child to the awaiting parent. The awaited
task must be a temporary or moved: `co_await helper(dut)` or
`co_await std::move(named_task)`.

### Process

```cpp
bool valid() const;
bool done() const;         // completed or cancelled
bool cancelled() const;    // finished BECAUSE it was cancelled
void cancel() const;
co_await process;          // yields void; rethrows the process's exception
```

The handle `test.spawn(...)` returns. Unlike `Task`, a `Process` is
copyable — a reference-counted handle, safe to store and pass around.
`cancel()` on an already-finished handle is a no-op. Awaiting a
default-constructed handle is a runtime error; check `valid()` first if
one might be empty.

### Join

```cpp
co_await Join{drive(dut), monitor(dut), scoreboard(test)};
```

Runs two or more `Task<void>` children and resumes when all of them have
finished; an exception from any child is rethrown. Children are moved in.
For racing rather than joining, see
[First](awaitables.md#first); for children with independent lifetimes, use
`test.spawn(...)` and await the `Process` handles.

## Coordination primitives

### Event

```cpp
explicit Event(std::string_view name = {});

bool is_set() const noexcept;
void set();       // wakes ALL waiters and latches
void clear();     // clears the latch; wakes nobody
co_await event;             // or:
co_await event.wait();
```

A latching broadcast: after `set()`, waiters resume and the flag stays set
until `clear()`, so awaiting an already-set event completes without
suspending. `event.wait()` and bare `co_await event` behave identically —
`wait()` records the call site in wait-graph diagnostics, the bare form
records the declaration site; prefer `wait()` where the report should name
the waiting line.

### Queue

```cpp
explicit Queue(size_t maxsize = 0, std::string_view name = {});

Task<void> put(T value);        // blocks while full
Task<T>    get();               // blocks while empty
bool put_nowait(T value);       // false instead of blocking
std::optional<T> get_nowait();  // nullopt instead of blocking
size_t size();  bool empty();  bool full();  size_t maxsize() const;
```

Typed FIFO in both directions: items come out in insertion order and
blocked callers are served oldest-first. `maxsize == 0` (the default)
means unbounded — `full()` is then always false and `put()` never blocks.
`T` must be move-constructible. The `_nowait` pair is the only safe form
outside a coroutine. Wrapping `queue.get()` in
[with_timeout](awaitables.md#with_timeout) is safe: an abandoned wait
releases its reservation and wakes the next waiter.

### Lock

```cpp
explicit Lock(std::string_view name = {});

co_await lock.acquire();    // FIFO; no bare co_await form
bool try_acquire();
void release();
bool locked();
```

Binary and non-reentrant, with no owner tracking — any process may call
`release()`, and releasing an unlocked lock is a runtime error. When
waiters are queued, `release()` hands ownership directly to the oldest
waiter, so a third party polling `try_acquire()` cannot barge in between.

### Semaphore

```cpp
explicit Semaphore(size_t permits = 0, std::string_view name = {});

co_await sem.acquire();     // consumes one permit; FIFO
bool try_acquire();
void release(size_t permits = 1);
size_t available();
```

A counting semaphore whose initial permit count defaults to **zero** — the
default-constructed form is an empty credit counter, not a mutex; pass an
initial count for resource-limiting use. `release(n)` gives permits to the
oldest waiters first and banks the surplus only when no waiter is left.

## See also

- [TestContext and checks](test-context.md) — `spawn()`, process
  ownership, and what happens to running processes when a test ends.
- [Triggers and phase waits](awaitables.md) — everything time-shaped that
  these primitives compose with.
