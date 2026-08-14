# Verification components

<!-- api-headers: include/cpptb_vc/cpptb_vc.hpp include/cpptb_vc/apb.hpp include/cpptb_vc/ports.hpp include/cpptb_vc/scoreboards.hpp include/cpptb_vc/stream.hpp include/cpptb_vc/transaction_recording.hpp include/cpptb_vc/memory_model.hpp include/cpptb_vc/memory_mapped.hpp include/cpptb_vc/register_model.hpp include/cpptb_vc/register_sequences.hpp include/cpptb_vc/register_coverage.hpp -->

The optional `cpptb_vc` layer: bus components, analysis plumbing,
scoreboards, transaction recording, the sparse memory, and the register
abstraction. Everything lives in `namespace cpptb::vc` (the `cpptb_vc`
name is only the include directory), reached through the umbrella header:

```cpp
#include "cpptb_vc/cpptb_vc.hpp"
using namespace cpptb::vc;
// CMake: target_link_libraries(my_tb PRIVATE cpptb::cpptb cpptb::vc)
```

[Verification components](../verification-components.md) and
[Memory and register models](../memory-register-models.md) are the
guides; this page maps the surface and pins down which calls take
simulation time. Most component types are deliberately non-copyable
(several also non-movable) — declare them where they live.

## Bus masters and monitors

### MemoryStatus

```cpp
enum class MemoryStatus : uint8_t { Okay, SlaveError, DecodeError, Timeout };
enum class MemoryOperation : uint8_t { Read, Write };
enum class AccessPath : uint8_t { Frontdoor, Backdoor };
```

The shared vocabulary: every master returns `MemoryReadResponse<Data>` /
`MemoryWriteResponse` carrying a status and wait-cycle count, and every
observed access is a `MemoryTransaction<Address, Data, ByteEnable>`. A
timeout comes back as `MemoryStatus::Timeout` — a value to check, never an
automatic failure.

### ApbBus
### ApbMaster

```cpp
ApbBus bus{dut.pclk, dut.psel, dut.penable, dut.pwrite, dut.paddr,
           dut.pwdata, dut.prdata, dut.pready, dut.pslverr};  // + pstrb for APB4

ApbMaster master{bus, ApbConfig{...}};        // non-copyable

co_await master.write(address, data);          // Task<MemoryWriteResponse>
co_await master.write(address, data, byte_enable);   // needs a PSTRB signal
co_await master.read(address);                 // Task<MemoryReadResponse<Data>>
master.idle();                                 // immediate
```

Class-template-argument deduction covers APB3 (nine signals) and APB4
(ten, with strobe). Concurrent callers serialize on an internal lock; a
partial write on a strobe-less bus throws.

### ApbMonitor
### ApbProtocolChecker

```cpp
ApbMonitor monitor{test, bus};
test.spawn(monitor.run_forever());             // or run(n_transactions)
auto connection = monitor.observed().connect(subscriber);

ApbProtocolChecker checker{test, bus};
test.spawn(checker.run_forever());             // violations() to read the count
```

Monitors are passive — they never drive — and publish
`TransactionObservation<MemoryTransaction<...>>` through an owned
analysis port.

### ReadyValidDriver
### ReadyValidMonitor
### ReadyValidSink

```cpp
co_await driver.send(value);        // Task<uint32_t>: cycles stalled
co_await sink.receive();            // Task<value_type>
test.spawn(monitor.run(observed_port, n));   // takes an external AnalysisPort&
```

The stream trio for ready/valid handshakes.

## Analysis plumbing

### AnalysisPort

```cpp
template <typename T> class AnalysisPort {    // move-only
    [[nodiscard]] Connection connect(Subscriber& subscriber);
    void write(const T& value) const;          // synchronous, zero-time
};
```

A subscriber implements exactly one thing: `void write(const T&)` — no
base class, no registration. `write()` dispatches to every live
subscriber in connection order without suspending; a subscriber's `write`
must not wait. `connect()` returns a **`[[nodiscard]]` RAII connection**
— dropping the return value silently unsubscribes. Subscribers are
borrowed and must outlive their connections.

### AnalysisBuffer

```cpp
AnalysisBuffer<T> buffer{capacity, AnalysisOverflowPolicy::DropOldest};
port.connect(buffer);               // buffer is a subscriber
T item = co_await buffer.get();     // and a coroutine source
```

The bridge from the synchronous analysis domain into coroutines, with an
explicit overflow policy (`DropNewest`, `DropOldest`, or `Error`, which
throws into the publisher).

### PutPort
### GetPort

```cpp
PutPort<T> input{queue};    co_await input.put(value);   input.put_nowait(value);
GetPort<T> output{queue};   co_await output.get();       output.get_nowait();
```

Non-owning single-consumer endpoints over a `coro::Queue<T>` (or any
backend with the exact signatures).

## Scoreboards

### InOrderScoreboard
### KeyedScoreboard

```cpp
InOrderScoreboard<T> scoreboard{test, "label"};        // non-copyable, non-movable
predicted.connect(scoreboard.expected());
monitor.observed().connect(scoreboard.actual());       // unwraps Completed observations
scoreboard.finalize();                                 // reports unpaired counts

KeyedScoreboard<T, KeyOf> keyed{test, "label", key_of};  // out-of-order, FIFO per key
```

Comparison happens as soon as both sides have an entry, as a nonfatal
`expect_eq` under the scoreboard's label. The `expected()`/`actual()`
inputs accept both bare transactions and monitor observations.

### ReferenceModelAdapter

```cpp
auto model = make_reference_model<Input>(function);
observed.connect(model);            // publishes function(input) on
model.predicted.connect(...);       // its own AnalysisPort<Output>
```

## Transaction recording

### TransactionRecorder

```cpp
TransactionRecorder recorder;                       // non-copyable, non-movable
auto& stream = recorder.stream<MyTransaction>("apb0");   // stable reference
auto connection = recorder.connect(sink);           // [[nodiscard]]
stream.write(observation);                          // immediate
```

Type-erased recording of described transaction types
(`CPPTB_VC_DESCRIBE_TRANSACTION(Type, "name", fields...)`;
`MemoryTransaction` is described out of the box). Sinks implement
`write(const TransactionRecordView&)`; `InMemoryTransactionSink` collects
`RecordedTransaction` values, `JsonLinesTransactionSink` streams one JSON
object per line and must be `finalize()`d. Every call is immediate.

### TransactionMonitor

```cpp
template <typename T> class TransactionMonitor {    // base for protocol monitors
    AnalysisPort<TransactionObservation<T>>& observed();
  protected:
    Task<void> sample(Clock clock);                  // one sample point per cycle
    void publish(SimTime begin_time, T value,
                 TransactionDisposition disposition = Completed);
};
```

What a custom protocol monitor derives from; `ApbMonitor` is the worked
example.

## Sparse memory

### SparseMemory

```cpp
SparseMemory memory;                                // every method immediate
memory.add_region({.name = "ram", .base = 0x8000'0000, .size = 0x10000});

auto read  = memory.read_word<uint32_t>(address);   // MemoryReadResponse
auto write = memory.write_word(address, value, byte_enable);
MemoryStatus status = memory.read_into(address, span);   // zero-alloc hot path

memory.load(address, bytes);   memory.fill(address, size, value);
memory.load_file(address, path);   memory.dump_file(address, size, path);
std::vector<uint8_t> bytes = memory.inspect(address, size);
```

The expected-memory model: region-configured (permissions, byte order,
fill), sparse (untouched fill-value bytes allocate nothing), and entirely
zero-time. Out-of-region access returns `DecodeError`, permission
violations `SlaveError`; the backdoor calls (`load`/`fill`/`inspect`)
throw on a range outside one region. A `MemoryAccessCallback` can observe
or rewrite accesses.

### MemoryPredictor

```cpp
auto predictor = make_memory_predictor<Transaction>(test, memory);
monitor.observed().connect(predictor);   // checks reads against the model,
                                         // applies writes; mismatches() counts
```

## Register abstraction

The generated register model (from SystemRDL, IP-XACT, or RgGen contracts)
is constructed over any bus master and optionally a generated backdoor:

```cpp
peripheral_regs::RegModel<decltype(master)> regs{test, master, base_address};
auto backdoor = peripheral_regs::make_backdoor<decltype(master)>(dut);
```

The complete surface — register, field, and memory handles, staging and
mirroring, the standard sequences, passive prediction, access coverage, and
the extension points — has its own page:
[Register models](registers.md). The semantics live in
[Registers & memory](../memory-register-models.md).

## What takes simulation time

**`co_await`**: master `read`/`write`, monitor and checker `run*`, stream
`send`/`receive`, `PutPort::put`/`GetPort::get`/`AnalysisBuffer::get`,
every register-handle frontdoor operation (`read`/`write`/`update`/
`mirror` and the model-wide `update_all`/`mirror_all`), and the three
standard sequences.

**Immediate, zero-time**: `AnalysisPort::write`/`connect`, every
scoreboard/predictor/coverage `write`, all `*_nowait` forms, all
`peek`/`poke`, model state (`stage`/`predict*`/`reset`), all
metadata accessors, the entire `SparseMemory`, and the entire
transaction-recording API.

## See also

- [Verification components](../verification-components.md) — architecture
  and worked examples for the endpoint/analysis layer.
- [Memory and register models](../memory-register-models.md) — the RAL in
  full, including user effects, address maps, and wide registers.
