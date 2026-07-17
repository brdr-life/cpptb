# Testbench authoring

cpptb testbenches are C++20 coroutines connected to a generated, typed DUT.
The public runtime is under `include/cpptb/`; generated SystemVerilog uses DPI
to exchange signal batches and simulator events with that runtime.

Start with the small counter, then move to the FIFO scoreboard or APB example:

```sh
make cpp-dpi-counter-run
make cpp-dpi-fifo-scoreboard-run
make cpp-dpi-apb-regfile-run
```

See [Running tests](running-tests.md) for the zero-config project command,
multiple registered tests, selection, nonfatal and fatal checks, and structured
results.

The APB example keeps protocol timing in a reusable helper, leaving the test
sequence register-oriented:

```cpp
Task<void> register_sequence(Dut dut, TestContext& test) {
    dut.clk.set(0);
    test.start_clock(dut.clk, 10_ns);

    co_await reset_dut(dut);
    const ApbMaster apb{dut, test};

    co_await apb.write(0x04, 0x1234'5678);
    co_await apb.read_expect("register readback", 0x04, 0x1234'5678);
}

CPPTB_REGISTER_TEST(register_sequence);
```

`CPPTB_REGISTER_TEST` installs the test through a translation-unit initializer.
Register one or more root tests in the same binary; each simulator invocation
selects and runs exactly one of them in fresh simulation state.
Link the testbench translation unit directly into the simulator executable. If
it is packaged in a static archive, link that archive with whole-archive
semantics (or force-link the object) so the linker does not discard its
otherwise unreferenced initializer.

The complete implementation, including `Task<uint32_t> read(...)`, is in
`examples/apb_regfile/testbench.cpp`.

The user-facing primitives are deliberately small:

- `test.start_clock(dut.clk, 10_ns)` to register a periodic input clock before
  the test's first await.
- `test.spawn(task)` to attach concurrent reset, driver, sequence, and monitor
  coroutines and return a process handle.
- `co_await RisingEdge{dut.HCLK}` and `co_await FallingEdge{dut.HCLK}` for
  simulator triggers.
- `dut.signal.set(value)` and `dut.signal.get()` for explicit signal access.
- `dut.block1.block2.name.get()`, `deposit(value)`, `force(value)`, and
  `release()` for any supported object in the inferred RTL hierarchy.
- `co_await helper_task(...)` for reusable bus operations such as APB writes
  and read/check transactions.

Every coroutine that does not return a value is declared `Task<void>`.
`Task<T>` carries a typed result from `co_return` to `co_await`, including
move-only values. Tasks themselves are move-only and can only be awaited as
rvalues; a completed result is moved to the awaiting coroutine. Scheduler
roots passed to `spawn()` or `spawn_detached()` remain `Task<void>`, and every
child passed to `Join` must also be a `Task<void>`.

The coroutine scheduler also supports:

- `co_await Edge{signal}` for either transition.
- `co_await Delay{1_ps}` for a physical simulation-time delay. Integer
  `fs`, `ps`, `ns`, `us`, and `ms` literals are supported and must be
  positive and representable at the wrapper's configured `timeprecision`.
- `co_await First{RisingEdge{signal}, Delay{100_ns}}`, which returns the index
  of the trigger that fired first. `First` accepts two or more triggers.
- `co_await Join{producer(tb), consumer(tb), scoreboard(tb)}` for two or more
  concurrent `Task<void>` children.
- `co_await clock_cycles(clock, count)` to wait for `count` rising edges.
  A count of zero completes immediately without suspending.
- `co_await with_timeout(RisingEdge{signal}, 100_ns)` to race a rising,
  falling, or either-edge trigger against a `SimTime` delay. It returns
  `TimeoutOutcome::Triggered` or `TimeoutOutcome::TimedOut`.
- `co_await with_timeout(operation(), 100_ns)` to race any `Task<T>` against
  a deadline. It returns `TimeoutResult<T>`; `has_value()`, `triggered()`, and
  boolean conversion report completion, `timed_out()` reports timeout, and
  `value()`, `operator*`, and `operator->` access a completed value. The
  `Task<void>` specialization provides the same state queries and a checked
  no-op `value()` for completed operations.
- `co_await wait_until(signal, predicate, clock)` to evaluate the predicate
  immediately and, while it is false, poll it after each rising edge of
  `clock`. The predicate receives the signal's `uint32_t` value.

`Event` is a sticky, manually reset notification. `set()` leaves it set and
wakes all current waiters in FIFO registration order; later waits also finish
immediately until `clear()` is called. Both `co_await event.wait()` and
`co_await event` are supported, and `is_set()` reports the current state.

`Queue<T>` is a FIFO for move-constructible values. Its optional constructor
argument sets the maximum number of queued items; zero means unbounded.

```cpp
Queue<Packet> requests{8};

co_await requests.put(std::move(packet));  // waits only while full
Packet next = co_await requests.get();

const bool accepted = requests.put_nowait(std::move(another_packet));
std::optional<Packet> available = requests.get_nowait();
```

`put_nowait()` returns `false` when a bounded queue is full and does not move
from its argument on that path. `get_nowait()` returns an empty `optional` when
no unreserved item is available. `empty()`, `full()`, `size()`, and `maxsize()`
report the current state. Blocking producers and consumers resume in FIFO
registration order. Move-only payloads are supported by every operation.

## When to use a queue

Use a queue when one coroutine produces typed data that another coroutine must
consume in FIFO order. Common examples are a sequence feeding a driver, a
monitor feeding a scoreboard, or a reference model publishing expected
transactions. A bounded queue is useful when the producer must not outrun the
consumer indefinitely:

```cpp
Task<void> source(Queue<Packet>& packets) {
    for (uint32_t index = 0; index < kPacketCount; ++index) {
        co_await packets.put(make_packet(index));
    }
}

Task<void> driver(Dut dut, Queue<Packet>& packets) {
    for (uint32_t index = 0; index < kPacketCount; ++index) {
        Packet packet = co_await packets.get();
        co_await drive_packet(dut, packet);
    }
}

Queue<Packet> packets{4};
co_await Join{source(packets), driver(dut, packets)};
```

Prefer blocking `put()` and `get()` for normal producer/consumer flow. Use
`put_nowait()` or `get_nowait()` only when failure to transfer immediately is
itself part of the test logic. Avoid polling `empty()` or `full()` around a
blocking operation; another coroutine can change the queue before the next
statement runs.

A queue transfers one value to one consumer. Use `Event` for a payload-free
broadcast notification, `Join` to wait for a fixed group of tasks, `Semaphore`
to limit concurrent resource use, and `Lock` to serialize a short update to
shared C++ state. Queue operations do not advance simulation time and do not
model DUT protocol timing: the driver still writes signals and awaits edges or
delays explicitly.

`Semaphore` controls a count of available permits. `try_acquire()` is the
nonblocking operation, `co_await acquire()` waits in FIFO order, `release()`
returns one or more permits, and `available()` reports permits that have not
already been handed to a waiter.

```cpp
Semaphore credits{4};

co_await credits.acquire();
co_await send_transaction();
credits.release();
```

`Lock` provides `try_acquire()`, `co_await acquire()`, `release()`, and
`locked()`. It performs direct FIFO ownership handoff to a waiting coroutine:

```cpp
co_await scoreboard_lock.acquire();
model.apply(transaction);
scoreboard_lock.release();
```

The lock does not track a coroutine owner. Code that has acquired a lock or
semaphore permit remains responsible for releasing it, including on early
returns. Cancellation safely removes tasks that are still waiting and returns
any queue slot, lock handoff, or semaphore permit reserved for them.

An `Event`, queue, lock, or semaphore must outlive its active waiters;
destroying one with live waiters aborts with a diagnostic. Current waiters must
belong to one scheduler, although the object may be reused with another
scheduler after its wait queue is empty. Scheduler destruction and process
cancellation invalidate their registrations, which are removed during cleanup.
Unused synchronization objects do not register scheduler waits. This is a
lifetime property of the API, not a benchmark claim about hot-path cost.

`TimeoutResult<T>` stores its result in an `optional`, so `T` need only be
move-constructible and does not need a default constructor. Calling `value()`
after a timeout aborts with a diagnostic. A timed-out task is recursively
cancelled, including nested tasks and waits on `Process`, `Event`, or
`Queue`; its frames are destroyed at the existing scheduler cleanup
boundary. If task completion and the deadline share a timestamp, completion
wins when the task's result is present as the parent resumes. Zero and
sub-precision timeout durations are rejected under the same rules as
`Delay`.

These pieces compose into ordinary driver, monitor, and scoreboard code. The
following is the core of the runnable `fifo_scoreboard` example:

```cpp
constexpr uint32_t kWordCount = 24;

Task<void> scoreboard(TestContext& test,
                      Queue<uint32_t>& expected_words,
                      Queue<uint32_t>& observed_words) {
    for (uint32_t index = 0; index < kWordCount; ++index) {
        const uint32_t expected = co_await expected_words.get();
        const uint32_t actual = co_await observed_words.get();
        test.expect_eq("FIFO payload", actual, expected);
    }
}

Task<void> fifo_test(Dut dut, TestContext& test) {
    Event reset_done;
    Queue<uint32_t> expected_words;
    Queue<uint32_t> observed_words;

    co_await Join{reset_dut(dut, reset_done),
                  input_driver(dut, reset_done, expected_words),
                  output_ready_driver(dut, reset_done),
                  output_monitor(dut, reset_done, observed_words),
                  scoreboard(test, expected_words, observed_words)};
}

CPPTB_REGISTER_TEST(fifo_test);
```

## Reusable transaction components

Use the optional component layer when a sequence, driver, monitor, or
scoreboard should depend on a typed endpoint instead of concrete storage.
`PutPort<T>` and `GetPort<T>` are borrowed views: constructing one allocates
nothing, and the connected backend must outlive the port and its active calls.
A `Queue<T>` works directly, and another backend can implement the same
`put`/`put_nowait` or `get`/`get_nowait` methods. A custom blocking `put` must
accept `T` by value so ownership remains in its coroutine frame; the
`PutPort` constructor enforces that signature.

```cpp
Queue<Packet> storage{8};
PutPort<Packet> input{storage};
GetPort<Packet> output{storage};

co_await input.put(packet);
Packet next = co_await output.get();
```

`AnalysisPort<T>` publishes one const transaction synchronously to every live
subscriber in connection order. `write()` is a zero-time C++ call: it neither
suspends nor advances the simulator. Keep the returned move-only connection
for as long as the subscription should remain active; destroying or explicitly
disconnecting it removes that subscriber.

```cpp
AnalysisPort<Packet> observed;
InOrderScoreboard<Packet> scoreboard{test, "packet"};
AnalysisBuffer<Packet> audit{8, AnalysisOverflowPolicy::Error};

auto score_connection = observed.connect(scoreboard.actual());
auto audit_connection = observed.connect(audit);

observed.write(packet);  // immediate fan-out; no hidden await or delay
```

Subscribers are borrowed and must outlive their connections. A subscriber's
`write(const T&)` must not wait. Connect `AnalysisBuffer<T>` when a coroutine
needs to consume observations asynchronously. Its required overflow policy is
explicit: `DropNewest`, `DropOldest`, or `Error`. The two drop policies do not
block or interrupt later subscribers. `Error` throws synchronously and stops
that publication, so connect an Error-policy buffer after subscribers that
must always observe the transaction. Other subscriber exceptions follow the
same connection-order rule. `output()` exposes the buffer's consumer side as
a `GetPort<T>`.

`InOrderScoreboard<T>` accepts expected and actual transactions in either
arrival order, compares pairs with nonfatal `expect_eq()`, and reports any
unpaired values from `finalize()`. It is intentionally noncopyable and
nonmovable because its typed inputs refer back to the owning scoreboard.
`ReadyValidDriver` and
`ReadyValidMonitor` translate transactions to pin activity. Their constructor
requires the sampling delay, and the monitor also requires the sample edge;
they do not reset signals, start clocks, or spawn themselves. `send()` drives
one transfer and deasserts `valid` before returning; repeated calls therefore
include an idle cycle rather than implying a maximum-throughput burst driver.

The complete [component FIFO example](examples/component-fifo.md) shows these
objects alongside the direct [FIFO scoreboard](examples/fifo-scoreboard.md).
Both are runnable, and each has an exact pure-SystemVerilog twin.

A spawned process can be awaited or cancelled explicitly:

```cpp
auto monitor = test.spawn(monitor_bus(dut));
auto driver = test.spawn(drive_packets(dut));

co_await driver;
monitor.cancel();
```

`TestContext::now()` reports the scheduler's current absolute simulation time.
`spawn_detached()` starts a root that needs no handle. Neither signal writes
nor backdoor operations add an implicit delay; drive, wait, settle, and sample
remain visible in user code.

The `TestContext&` passed to the registered root refers to a context stored in
that root's coroutine frame. A child that may outlive the root must not retain
that reference. Pass `TestContext` by value to such a child, especially a
detached child:

```cpp
Task<void> detached_monitor(Dut dut, TestContext test) {
    co_await RisingEdge{dut.alert};
    test.expect_eq("alert payload", dut.payload.get(), 0x42u);
}

Task<void> root_test(Dut dut, TestContext& test) {
    test.spawn_detached(detached_monitor(dut, test));
    co_return;
}
```

Copying `TestContext` copies its handles to the scheduler and result; it does
not schedule work or advance simulation time. Children that are joined before
the root returns may use the reference as long as their lifetime is bounded by
that join.

`Process` is a copyable handle to scheduler-owned process state. Copying a
handle does not copy the coroutine or transfer ownership; all copies observe
the same completion state. The scheduler owns the root coroutine and destroys
its frame when it completes or is cancelled. A `Process` does not extend the
life of its `Testbench`. After the testbench is destroyed, outstanding handles
are invalid: status queries are safe and return false, `cancel()` is a no-op,
and attempting to `co_await` the invalid handle aborts with a diagnostic.

`done()` reports either normal completion or completed cancellation.
`cancelled()` is true only after cancellation has taken effect, so
`cancelled()` implies `done()`. If normal completion wins a race with a pending
cancellation request, the process is done but not cancelled. Cancellation
recursively stops nested tasks and wakes process waiters. A running process may
cancel itself; that request is cooperative and is applied after the coroutine
returns control to the scheduler. Code still executing before that scheduler
boundary observes the process as neither done nor cancelled.

Use `spawn()` when callers need a handle for status, awaiting, or cancellation.
Use `TestContext::spawn_detached()` when user code needs no process handle. It
is still owned by the current test so failures are attributed correctly and
unfinished work is cancelled when that test ends. The lower-level scheduler
has a metadata-free detached primitive, but user testbenches should use the
context method for lifecycle ownership.

`examples/watchdog_timeout/testbench.cpp` shows both forms of `with_timeout()`
and a complete `spawn()`/`cancel()`/await lifecycle. Its deadlines are kept
strictly separate from response times so the expected result is independent of
same-timestamp ordering.

## Cocotb concept map

cpptb follows familiar cocotb testbench structure while keeping signal access
and timing explicit:

| Verification concept | cocotb | cpptb | Pure-SV twin |
|---|---|---|---|
| Timed wait | `await Timer(1, unit="ns")` | `co_await Delay{1_ns}` | `#1ns` |
| Signal edge | `await RisingEdge(dut.clk)` | `co_await RisingEdge{dut.clk}` | `@(posedge clk)` |
| Concurrent work | `start_soon()` / task groups | `spawn()` or `Join{...}` | `fork ... join` |
| FIFO communication | `Queue` | `Queue<T>` | `mailbox` |
| Notification | `Event` | `Event` | `event` |
| Deadline | `with_timeout()` | `with_timeout()` | explicit event/deadline race |

The mapping is informed by cocotb's official documentation for
[testbench structure](https://docs.cocotb.org/en/stable/writing_testbenches.html),
[coroutines and concurrency](https://docs.cocotb.org/en/stable/coroutines.html),
and the [timing model](https://docs.cocotb.org/en/stable/timing_model.html).
In particular, an edge wake does not by itself promise that downstream
sequential or combinational logic has settled. The examples therefore show
the sampling point explicitly with `Delay{1_ps}`, mirrored by `#1ps` in SV.

The [fault-injection example](examples/fault-injection.md) and
[hierarchy guide](hierarchy.md) show the backdoor
operations on a resolved net, a clocked variable, and a memory element. These
operations are immediate and add no implicit delay. `force()` applies until
the matching `release()`; after release, normal RTL drivers can update the
object again. Force is generated for hierarchical objects used by the compiled
testbench, not ordinary DUT ports. In particular, registered input clocks remain owned
by `start_clock()` and cannot safely be forced or paused through the public
API.

Calling `force()` or `release()` on an ordinary port produces an intentional
compile-time diagnostic rather than a generic missing-member error. For a
scheduler-owned clock, the diagnostic directs the user back to
`TestContext::start_clock()` and states that coherent clock pause/override is
not yet supported.

The complete learning path is indexed in `examples/README.md`. Run individual
pairs or all examples through the standard target:

```sh
make feature-test FEATURE=dpi_fifo_scoreboard
make feature-test FEATURE=dpi_apb_regfile
make feature-test FEATURE=dpi_watchdog_timeout
make examples-test
```

## Scheduler ordering and cleanup

Waiters registered on the same edge resume in registration order. Timers with
the same deadline also resume FIFO by registration order. Registrations made
stale by `First`, cancellation, or completion are removed without resuming the
coroutine; this cleanup includes edge queues and falling-edge interest counts,
not only the timer heap.

C++-owned clocks are registered as static edge sources before the first wait.
The first `start_clock()` call selects the primary cycle counter; later calls
add independent domains and may specify a phase. Registered input-clock rising
edges are delivered on every cycle; falling edges are delivered while the
scheduler has a falling- or either-edge waiter. Waits on clock IDs do not
publish dynamic edge-interest masks. DUT-produced and manually driven edges use
interest-gated observers. See [clocking](clocking.md) for the concise user
contract.

Generated DPI wrappers use one persistent, clock-agnostic timer owner. The
module-level `timer_deadline` is the source of truth, `timer_owner_target`
describes only the owner's current positive sleep, and `timer_kick` wakes an
idle owner. A generation-checked one-shot process is retained only when a
non-owner callback inserts a deadline strictly earlier than the owner's stale
sleep target. Clock drivers and observers remain independent, so
`clock_cycles()` is still an edge primitive while `Delay` works with no clocks.

The generated timer contract is:

- **I1:** after `STEP_TIMER_CHANGED`, `timer_deadline` equals the scheduler's
  earliest live deadline or `NO_TIMER`;
- **I2:** the persistent owner and strict-earlier fallback deliver each live
  deadline exactly once; generation checks prevent stale fallback delivery;
- **I3:** timer dispatch uses no zero delay, delayed nonblocking assignment, or
  `disable fork`;
- **I4:** every live deadline has an owner or fallback wake no later than that
  deadline;
- **I5:** the fallback is unreachable in a clockless wrapper because no
  non-owner step can insert an earlier deadline while the owner sleeps;
- **I6:** there remains one next-deadline DPI query per timer-change request,
  and steady-state timer arms allocate no SystemVerilog process. Only the
  exceptional strict-earlier fallback allocates a one-shot process.

Zero-duration delays and delays that cannot be represented at the configured
simulation precision abort with a diagnostic. Awaiting a default-constructed,
expired, or otherwise invalid `Process` also aborts instead of silently
continuing.

Scheduler-facing objects are simulation-thread confined. Do not access a
`Testbench`, `TestContext`, signal handle, synchronization primitive, or
`Process` from a worker OS thread. Authored coroutine processes remain
concurrent in simulation time while executing through the one scheduler that
owns deterministic ordering.

## Scheduler performance

A macOS sampling profile first identified full coroutine-state scans and
hash-table lookups as scheduler hot spots. The scheduler now uses reusable
numeric state slots, direct signal-indexed wait queues, targeted child cleanup,
conditional drains, and an active-coroutine counter.

A second profile of the exact dual-clock C++ DPI/pure-SV comparison showed
that the remaining cost was primarily in generated SystemVerilog timing
processes rather than C++ queue management. Generated periodic clocks use one
absolute-deadline process, falling-edge DPI calls are skipped unless the
scheduler has a matching waiter, and physical delays are scheduled only when
a coroutine awaits `Delay`.

The performance guard uses an initial batch of 16 warmed, adjacent
C++ DPI/pure-SV pairs and alternates execution order. It compares the median of
the paired process-time ratios and hard-fails above `1.10x`. When the median
passes but its one-sided 95% upper confidence bound is inconclusive, the guard
collects one additional 16-pair batch and evaluates the combined samples; it
does not rerun the complete benchmark. A still-inconclusive passing median is
reported with a warning. Result artifacts include raw pairs and the
environment/build metadata needed to interpret them. Close ratios are treated
as noisy measurements, not evidence that either implementation is
directionally faster.

The raw runner applies this calculation to every feature. The registry has one
visible, capped exception for the transport-only `force_direct` microbenchmark;
see [Scoped direct-force waiver](performance.md#scoped-direct-force-waiver).

## Current scope

The end-to-end test suite currently targets Verilator. Scalar signal values use
`uint32_t`; packed values use `uint64_t` through 64 bits and `Bits<W>` above 64
bits. Generated DPI bindings support fixed multidimensional unpacked arrays,
packed enum and struct views, wide values, fixed-point helpers, and generated
hierarchical probes with read, deposit, force, and release operations.
Four-state X/Z propagation remains deferred. The generated
transport uses standard SystemVerilog DPI, but additional simulator backends
have not yet passed the conformance suite.

The Authoring Core sources currently present under
`benchmarks/authoring_core/` exercise typed tasks, cycle waits, edge timeouts,
predicate waits, events, bounded queues, locks, semaphores, wide packed
signals, fixed-point arithmetic, fixed unpacked arrays, and a synchronous
memory front door. Its C++ DPI testbench is
`benchmarks/authoring_core/testbenches/cpp_dpi/testbench.cpp`, the corresponding pure-SV
source is `benchmarks/authoring_core/testbenches/systemverilog/authoring_core_sv_tb.sv`, and the
shared workload contract is `benchmarks/authoring_core/workload.py`. Runtime
API tests are in `tests/unit/coro_runtime_test.cpp`.

## Reusable DPI runtime

`include/cpptb/dpi_runtime.hpp` owns the design-independent DPI host behavior:

- compact directional input/output transport and driven-signal tracking;
- typed signal `get()`/`set()` callbacks and dirty-output detection;
- generated internal-probe `get()`/`deposit()` access for packed variables and
  fixed memories;
- scheduler construction, edge dispatch, and delay deadlines;
- falling-edge interest and precision-aware time transport;
- timeout invocation, elapsed wall time, completion, and result reporting;
- the standard init, step, output-pull, deadline, and edge-interest C exports
  expected by the generated wrapper.

`include/cpptb/test_result.hpp` keeps status, check counts, timing, and
structured failure records independent of DPI. `test_reporting.hpp` writes the
versioned JSON result consumed by the optional launcher, so user-facing
fixtures and embedding harnesses do not include simulator transport headers.

A design supplies a small `DpiAdapter` containing its DUT and result types,
generated signal metadata, binding call, testbench registration call, result
name, and timeout policy. `CPPTB_DEFINE_DPI_RUNTIME(Adapter)` provides the C
entry points. No design transport needs to copy open arrays, decode events, or
format a result line.

The hot scheduler step receives only the compact observed-word array. Driven
words are fetched through a separate idempotent output-pull export on
initialization or after `STEP_OUTPUTS_CHANGED`, so unchanged steps do not carry
an output argument through the simulator ABI.

`deposit()` performs the underlying SystemVerilog blocking assignment
immediately. It does not insert a scheduler delay or observation phase;
testbench code uses an explicit `co_await Delay{...}` when downstream RTL must
evaluate before observation.

## Benchmark

The cocotb comparison benchmark is in `experiments/cocotb_cpp_comparison/`:

```sh
python3 experiments/cocotb_cpp_comparison/run_benchmark.py --iters 1000 --runs 3
```

It runs the same APB event-unit traffic in cocotb and in the C++ coroutine
model, then writes results to `experiments/cocotb_cpp_comparison/results/`.
