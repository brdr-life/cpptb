# Transaction recording

Transaction recording presents protocol operations as timed, structured data.
It complements signal waveforms rather than replacing them. For example, an
APB waveform can be summarized as:

```text
120 ns - 150 ns  apb.write  address=0x24  data=0x08  status=okay
```

Reading that line beats reading the waveform that produced it, and unlike a
waveform it can be diffed, filtered, or fed to another tool.

:::{note}
Recording lives in the optional `cpptb_vc` package, so no protocol knowledge or
recording policy reaches the core scheduler. The shipped slice covers typed
completed observations, APB monitoring, in-memory retention, and JSON Lines
output. The design-decision record and the deliberately deferred extensions are
tracked on the [roadmap](../roadmap.md#3-reusable-verification-components).
:::

The record comes from a passive protocol monitor that observes DUT signals and
reconstructs what actually completed. A recorder subscribes to the monitor's
typed output; the test does not manually repeat transaction or field names.
Observed transactions are the verification ground truth. Driver-side intent
recording — what the test asked a driver to do, kept as a separately named
stream — is a deferred extension, and the current API records observed traffic
only and does not correlate streams.

## Architecture at a glance

![Transaction recording architecture showing the active driver, DUT signals,
passive monitor, typed analysis fan-out, optional intent stream, recorder, and
output sinks.](../_assets/transaction-recording-architecture.svg)

The solid path is the default: the passive monitor reconstructs completed
transactions from DUT signals and publishes one typed observation. Scoreboards,
coverage, models, and recording consume that same publication. The dashed path
is optional driver intent; it would reach the recorder through a separately
named stream and is not automatically correlated with the observed traffic.

| Layer | Responsibility |
|---|---|
| Protocol monitor | Observe signals, identify boundaries, and construct typed transactions |
| `AnalysisPort<TransactionObservation<T>>` | Fan one observation out to checking, coverage, models, and recording |
| Transaction metadata | Begin/end sample time, completion disposition, named stream, per-stream sequence, and available provenance |
| Typed recording stream | Establish type erasure and explicit component identity once at connection time |
| Recorder | Apply global or per-stream enable policy and route records to sinks |
| Sink | Serialize JSON, retain an in-memory trace, or integrate with a simulator database |

## User experience

For a standard protocol, the VC defines the transaction type and decoding
rules. The user connects generated signals to the monitor, then connects the
monitor to any consumers:

```cpp
const auto bus = ApbBus{
    dut.clk,           dut.apb_select,    dut.apb_enable,
    dut.apb_write,     dut.apb_address,   dut.apb_write_data,
    dut.apb_read_data, dut.apb_ready,     dut.apb_error};

ApbMonitor monitor{test, bus};
using Transaction = typename decltype(monitor)::transaction_type;

TransactionRecorder recorder;
JsonLinesTransactionSink json{"transactions.jsonl"};
InOrderScoreboard<Transaction> scoreboard{
    test, "APB transaction"};

auto json_output = recorder.connect(json);
auto recording = monitor.observed().connect(
    recorder.stream<Transaction>("apb0.observed"));
auto checking = monitor.observed().connect(scoreboard.actual());

co_await Join{
    monitor.run(kExpectedTransfers),
    stimulus(dut, test),
};

json.finalize();
```

The important properties are:

- the monitor attaches to typed signals and understands APB timing;
- `observed()` publishes `TransactionObservation<Transaction>`;
- existing consumers receive its `.value` through constrained overloads, so
  one typed publication can feed recording, scoreboards, coverage, and models;
- recording does not drive signals, advance time, or affect pass/fail results;
- transaction and field names are not authored in the test; and
- the explicit stream string names a component instance for debug output; it
  does not define transaction fields or protocol behavior.

The runnable [APB transaction trace example](../examples/apb-trace.md) records
and checks the same 256 operations in C++ and pure SystemVerilog with this
setup.

## The observation envelope

The user should not define APB fields repeatedly. `cpptb_vc` already has the
protocol-neutral shape needed for APB observations:

```cpp
template <typename Address, typename Data, typename ByteEnable>
struct MemoryTransaction {
    MemoryOperation operation;
    Address address;
    Data data;
    ByteEnable byte_enable;
    MemoryStatus status;
    uint32_t wait_cycles;
};
```

The monitor publishes an envelope whose type is fixed by the API:

```cpp
enum class TransactionDisposition {
    Completed,
    Aborted,
    Incomplete,
};

template <typename T>
struct TransactionObservation {
    coro::SimTime begin_time{};
    coro::SimTime end_time{};
    TransactionDisposition disposition =
        TransactionDisposition::Completed;
    T value{};
};
```

`ApbMonitor::observed()` therefore returns an
`AnalysisPort<TransactionObservation<transaction_type>>&`. Scoreboard,
reference-model, and coverage inputs gain constrained `write(observation)`
overloads that forward `observation.value` to their existing `write(value)`
implementation, so one monitor port serves recording and checking alike.
Those compatibility overloads forward only `Completed` observations. An
`Aborted` or `Incomplete` observation remains available to recorders and
diagnostic subscribers but is not silently compared as a completed value.

### Timestamps

At completion, `ApbMonitor` publishes one observation equivalent to:

```cpp
this->publish(setup_time_, transaction_type{
    .operation = write ? MemoryOperation::Write
                       : MemoryOperation::Read,
    .address = bus_.address.get(),
    .data = write ? bus_.write_data.get() : bus_.read_data.get(),
    .byte_enable = write ? strobe() : all_bytes(),
    .status = error ? MemoryStatus::SlaveError : MemoryStatus::Okay,
    .wait_cycles = wait_cycles_,
});
```

Times represent the instants at which the monitor sampled the protocol.
Under the [standard write model](../scheduling.md) that is the awaited clock
edge itself — the pre-evaluation resume, where the monitor reads the values the
design sampled at that edge. In a legacy immediate-write build the times fall
after the monitor's explicit sampling delay instead.

### Completed records and ordering

Publishing a completed record with explicit begin and end times avoids keeping
an open recorder handle. It also has an important limitation: a transfer that
never completes produces no completed observation, so a slave that never
asserts `PREADY`, a reset in the middle of a transfer, or monitor cancellation
can hide the operation most relevant to a hang. The current APB monitor
publishes completed records only and emits only `Completed`. If `PSEL` drops
before completion, it discards the unfinished setup and resynchronizes at the
next setup phase; it never combines fields from two transfers. `Aborted` and
`Incomplete` are valid envelope dispositions for custom publishers and sinks;
automatic monitor flush and cancellation emission are deferred.

Monitors publish records in completion order. Overlapping and out-of-order
transactions remain self-contained, so their begin times need not be
monotonic. The recorder preserves publication order and assigns a monotonically
increasing sequence number within each named stream. Sinks receive that
sequence unchanged. A viewer may sort by begin time, but consumers must not
assume the sink has done so.

## The monitor base

Generated DUT bindings can discover hierarchy, signal names, dimensions, and
types, but they cannot infer protocol semantics: which fields form one
operation, when a transfer completes, or how phases, wait states, and responses
relate. The recorder therefore never subscribes to arbitrary signals. A
protocol monitor decodes signals into typed transactions, and the recorder
subscribes to that typed output. Known VCs make this automatic; a custom
protocol requires a custom monitor or decoder.

`TransactionMonitor<T>` is the reusable plumbing beneath every such monitor,
not a second protocol layer. It owns `observed()`, anchors the sampling loop,
and provides four protected operations:

| Operation | Meaning |
|---|---|
| `sample(clock)` | Wait for the next rising edge — the pre-evaluation observation point under the standard write model. In a legacy immediate-write build it additionally awaits the configured sampling delay |
| `sample_until(clock, predicate)` | Repeat `sample(clock)` until the typed predicate is true |
| `now()` | Return the current simulation time |
| `publish(started, value)` | Add the completion time and publish one typed observation |

The protocol rules stay visible in the derived monitor: the APB setup, access,
wait-state, response, and error handling remain explicit in `ApbMonitor` (shown
in full in the [comparison below](#the-same-apb-observation-in-other-frameworks)).
The base only removes timestamps, output-port ownership, and the sampling loop
that every passive synchronous monitor otherwise repeats. It uses static
templates rather than virtual dispatch.

### Custom protocol monitors

Custom components use the same typed API, not a string field builder:

```cpp
struct CommandTransaction {
    uint8_t opcode;
    Bits<48> payload;
    bool accepted;
};

class CommandMonitor : public TransactionMonitor<CommandTransaction> {
  public:
    CommandMonitor(TestContext test, CommandBus bus)
        : TransactionMonitor{std::move(test)}, bus_{bus} {}

    Task<void> run() {
    while (true) {
        co_await this->sample(bus_.clock);
        if (bus_.valid.get() == 0 || bus_.ready.get() == 0) continue;

        const auto sampled = this->now();
        this->publish(sampled, CommandTransaction{
            .opcode = bus_.opcode.get(),
            .payload = bus_.payload.get(),
            .accepted = true,
        });
    }
    }

  private:
    CommandBus bus_;
};
```

Serialization for `CommandTransaction` is taught once by a static descriptor
authored beside the component, described under
[the JSON Lines schema](#the-json-lines-schema) below.

## Recorder, streams, and sinks

| API | Purpose |
|---|---|
| `recorder.stream<T>(name)` | Create one uniquely named typed recording endpoint |
| `recorder.connect(sink)` | Route every enabled record to a synchronously called sink |
| `recorder.set_enabled(bool)` | Disable or enable publication without disconnecting the graph |
| `stream.write(observation)` | Publish an explicitly timed `TransactionObservation<T>` |
| `stream.next_sequence()` | Inspect the next per-stream sequence number |
| `memory.records()` | Read retained records in publication order |
| `memory.clear()` | Release all records retained by an in-memory sink |
| `json.finalize()` | Flush and close JSON Lines output with checked errors |

`TransactionRecorder` is neither copyable nor movable. Its stream references
remain valid for the recorder's lifetime, and connections, streams, and sinks
must all remain alive while observations are published. `stream_count()` and
`sink_count()` are available for setup diagnostics; they are not simulation
work counters.

The recorder owns each stream label, rejects every duplicate label even when
the value type matches, and copies the label once at stream creation. Code that
needs the endpoint more than once stores the returned reference. A misspelling
changes a debug label; it cannot change the decoded fields or protocol
behavior. Because the publishing process identifies the passive monitor rather
than the stimulus process that caused the traffic, a named stream such as
`"apb0.observed"` is the primary component identity in recorded output.

The recorder can write to one or more optional sinks:

```cpp
JsonLinesTransactionSink json{"transactions.jsonl"};
auto json_output = recorder.connect(json);

// A future simulator adapter could use the same typed records.
// auto waveform_output = recorder.connect(simulator_transaction_database);
```

Connections follow the existing RAII analysis-port convention. A sink must
outlive its connection, and dropping the returned connection disconnects it.
`finalize()` flushes output and reports errors on a normal test exit; the sink
destructor performs only best-effort cleanup. JSON Lines keeps every completed
line independently readable if simulation terminates before finalization.

Filtering that depends on `T` belongs in a typed analysis subscriber adapter,
not behind the recorder's erased interface; the recorder itself provides only
cheap global and per-stream enables. A filtered adapter must be stored in a
named variable for at least as long as its analysis connection; connecting a
temporary would violate the existing raw-subscriber lifetime contract.

## The JSON Lines schema

The recorder associates each C++ value type with a static descriptor. Export
names such as `operation`, `address`, and `status` exist once in that
descriptor; they are not passed through test code as free-form strings.
`cpptb_vc` supplies a partial descriptor specialization for templated
`MemoryTransaction<Address, Data, ByteEnable>` values, so APB recording needs
no descriptor authoring at all.

C++20 cannot reflect aggregate member names, so a custom type such as the
`CommandTransaction` above needs one static descriptor beside its definition.
Member pointers keep fields typed and refactorable; strings are export labels
only:

```cpp
CPPTB_VC_DESCRIBE_TRANSACTION(
    CommandTransaction, "command",
    transaction_field<&CommandTransaction::opcode>("opcode"),
    transaction_field<&CommandTransaction::payload>("payload"),
    transaction_field<&CommandTransaction::accepted>("accepted"));
```

The macro defines an argument-dependent descriptor function. The equivalent
explicit form is available when a component does not want to use a macro:

```cpp
constexpr auto cpptb_transaction_descriptor(
    std::type_identity<CommandTransaction>) {
    return describe_transaction<CommandTransaction>(
        "command",
        transaction_field<&CommandTransaction::opcode>("opcode"),
        transaction_field<&CommandTransaction::payload>("payload"),
        transaction_field<&CommandTransaction::accepted>("accepted"));
}
```

`TransactionRecorder::stream<T>(name)` is the type-erasure boundary. It owns
the stream name and installs one per-type JSON writer when the stream is
created. Each `write()` constructs a non-owning record view on the stack and
invokes connected sinks synchronously. The view and its referenced transaction
remain valid only for that call. This avoids converting every transaction into
a heap-allocated string-to-value map before a sink asks for serialization.

Built-in value encoders cover integers, finite floating-point values, symbolic
enums, booleans, strings, `Bits`, `LogicBits`, arrays, and nested described
types. Floating-point values use round-trip precision and reject NaN or
infinity, which JSON cannot represent. Arrays and nested descriptors remain
structured JSON; packed and four-state values use the same stable textual form
as diagnostics. Descriptor fields must be direct data members. A field without
a supported encoder fails at the enabled sink with an actionable exception
rather than silently losing data.

## The same APB observation in other frameworks

The examples below focus on the protocol-decoding boundary for the same 32-bit
APB interface. Common clock, reset, construction, and error handling are
abbreviated. The production cpptb-vc component derives its address, data, and
byte-enable types from the generated signal types; the fixed aliases below keep
the comparison compact.

<div class="cpptb-code-tabs" data-tabs="4" data-tab-group="transaction-recording-observation" data-tab-label="Observed APB transaction"></div>

<div class="cpptb-code-tab-label">cpptb-vc</div>

```cpp
// ApbTransaction<Bus> derives its field widths from Bus's signal types.
template <typename Bus>
class ApbMonitor : public TransactionMonitor<ApbTransaction<Bus>> {
    using Base = TransactionMonitor<ApbTransaction<Bus>>;

  public:
    using typename Base::transaction_type;

    ApbMonitor(TestContext test, Bus bus, SimTime sample_delay = {})
        : Base{test, sample_delay}, bus_{bus} {}

    Task<void> run(std::size_t transaction_count) {
        while (transaction_count-- != 0) co_await observe_one();
    }

  private:
    Task<void> observe_one() {
        while (true) {
            co_await this->sample_until(bus_.clock, [&] {
                return bus_.select.get() != 0 && bus_.enable.get() == 0;
            });

            SimTime started{};
            bool write = false;
            transaction_type transaction{};
            const auto capture_setup = [&] {
                started = this->now();
                write = bus_.write.get() != 0;
                transaction = transaction_type{
                    .operation = write ? MemoryOperation::Write
                                       : MemoryOperation::Read,
                    .address = bus_.address.get(),
                    .data = bus_.write_data.get(),
                    .byte_enable = uint8_t{0x0f},
                };
            };
            capture_setup();

            while (true) {
                co_await this->sample(bus_.clock);
                if (bus_.select.get() == 0) break; // resynchronize
                if (bus_.enable.get() == 0) {
                    capture_setup(); // a repeated setup replaces the old one
                    continue;
                }
                if (bus_.ready.get() == 0) {
                    ++transaction.wait_cycles;
                    continue;
                }

                if (!write) transaction.data = bus_.read_data.get();
                transaction.status = bus_.error.get() == 0
                                         ? MemoryStatus::Okay
                                         : MemoryStatus::SlaveError;
                this->publish(started, std::move(transaction));
                co_return;
            }
        }
    }

    Bus bus_;
};
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
typedef struct {
  bit          write;
  logic [31:0] address;
  logic [31:0] data;
  int unsigned wait_cycles;
  bit          error;
  time         begin_time;
  time         end_time;
} apb_transaction_t;

task automatic monitor_apb();
  apb_transaction_t transaction;
  bit active = 0;

  forever begin
    @(posedge apb.PCLK);
    #1ps;

    if (!active && apb.PSEL && !apb.PENABLE) begin
      transaction = '{default: '0};
      transaction.write = apb.PWRITE;
      transaction.address = apb.PADDR;
      transaction.data = apb.PWDATA;
      transaction.begin_time = $time;
      active = 1;
    end

    if (active && apb.PSEL && apb.PENABLE) begin
      if (!apb.PREADY) begin
        transaction.wait_cycles++;
      end else begin
        if (!transaction.write) transaction.data = apb.PRDATA;
        transaction.error = apb.PSLVERR;
        transaction.end_time = $time;
        observed.put(transaction);
        transaction_log.write(transaction);
        active = 0;
      end
    end
  end
endtask
```

<div class="cpptb-code-tab-label">UVM</div>

```systemverilog
class apb_monitor extends uvm_monitor;
  `uvm_component_utils(apb_monitor)

  virtual apb_if.monitor vif;
  uvm_analysis_port #(apb_item) observed;

  task run_phase(uvm_phase phase);
    forever begin
      apb_item item = apb_item::type_id::create("item");

      do begin
        @(posedge vif.PCLK);
        #1ps;
      end while (!(vif.PSEL && !vif.PENABLE));
      item.write = vif.PWRITE;
      item.address = vif.PADDR;
      item.data = vif.PWDATA;
      void'(begin_tr(item, "apb.observed"));

      do begin
        @(posedge vif.PCLK);
        #1ps;
        if (vif.PENABLE && !vif.PREADY) item.wait_cycles++;
      end while (!(vif.PENABLE && vif.PREADY));

      if (!item.write) item.data = vif.PRDATA;
      item.error = vif.PSLVERR;

      end_tr(item);
      observed.write(item);
    end
  endtask
endclass
```

<div class="cpptb-code-tab-label">Cocotb</div>

```python
@dataclass(frozen=True)
class ApbTransaction:
    write: bool
    address: int
    data: int
    wait_cycles: int
    error: bool
    begin_time_ns: float
    end_time_ns: float

class ApbMonitor:
    async def run(self):
        while True:
            await RisingEdge(self.bus.PCLK)
            await ReadOnly()
            if not (self.bus.PSEL.value and not self.bus.PENABLE.value):
                continue

            started = get_sim_time("ns")
            write = bool(self.bus.PWRITE.value)
            address = int(self.bus.PADDR.value)
            data = int(self.bus.PWDATA.value)
            waits = 0

            while True:
                await RisingEdge(self.bus.PCLK)
                await ReadOnly()
                if self.bus.PENABLE.value and self.bus.PREADY.value:
                    break
                if self.bus.PENABLE.value:
                    waits += 1

            if not write:
                data = int(self.bus.PRDATA.value)

            self.observed(ApbTransaction(
                write, address, data, waits,
                bool(self.bus.PSLVERR.value),
                started, get_sim_time("ns")))
```

In all four versions, a protocol-aware monitor reconstructs an operation from
signals. UVM adds an explicit transaction-database lifecycle around its
sequence item; the environment and simulator must also enable transaction
recording for `begin_tr()` and `end_tr()` to produce database output. Pure SV
and Cocotb need an authored sink or simulator-specific integration if a
persistent transaction timeline is desired.

## Performance

Qualification uses four distinct comparisons:

1. The existing `apb_component` C++/pure-SV pair is the disabled-recorder
   guard: no recorder is constructed or connected, so it measures the
   observation-envelope and monitor helper overhead paid by users who do not
   record transactions. The disabled path performs no record formatting, heap
   allocation, file I/O, or additional simulator crossings.
2. The runnable [`apb_trace`](../examples/apb-trace.md) C++/pure-SV pair
   performs 256 identical transfers, including 128 inserted wait cycles,
   retains equivalent record metadata and JSON payloads, and checks the same
   decoded transactions. Both report 649 checks and 898 simulated cycles.
3. The `transaction_recording` authoring-core pair scales that workload to
   100,000 write/read pairs and retains 200,000 equivalent records in each
   implementation. It is the enabled-recorder `1.10x` hard gate, certified at
   `1.0414x` in the August 8, 2026 admitted run, and it counts every analysis
   operation with exact work counters rather than assigning totals afterward.
4. JSON Lines output is measured separately. It becomes a hard comparison only
   if the SV peer performs equivalent formatting and file writes with the same
   schema; filesystem variation must not hide framework overhead.

Both guarded pairs run under the repository's usual load gate, paired and
independent samples, and `1.10x` policy.

## Design notes

The decision record behind this API — why the monitor owns its analysis output
and the recorder subscribes like any other consumer, why string-based dynamic
recording was rejected as the documented default, and why intent-to-observation
correlation is deliberately excluded — is kept with the deliberately deferred
extensions (intent recording, live event streaming, automatic aborted-record
emission, and others) on the
[roadmap](../roadmap.md#3-reusable-verification-components). One behavioral
consequence worth knowing today: the existing ready/valid monitor still
publishes its bare `value_type` payload rather than an observation envelope;
its migration is among the deferred items.

## Related APIs

- [Library reference: verification components](../library/components.md) lists
  the `TransactionRecorder`, `TransactionMonitor`, and sink signatures.
- [Verification components](../verification-components.md) introduces the
  analysis fan-out, monitors, and APB components that publish into a recorder.
- [Waveforms](../waveforms.md) covers the signal-level tracing that recorded
  transactions summarize and complement.

## References

- The Accellera [UVM reference implementation](https://github.com/accellera-official/uvm-core)
  defines `uvm_transaction`, recorder, stream, and component transaction APIs.
- *[Transaction Recording Anywhere Anytime](https://dvcon-proceedings.org/wp-content/uploads/transaction-recording-anywhere-anytime.pdf)*
  demonstrates signal-attached SystemVerilog monitors and recommends keeping
  UVM recording usage simple.
- *[UVM Transaction Recording Enhancements](https://dvcon-proceedings.org/wp-content/uploads/uvm-transaction-recording-enhancements.pdf)*
  discusses missing monitor automation and the value of component provenance
  and parent/child relationships.
- Cocotb's [testbench tools documentation](https://docs.cocotb.org/en/stable/testbench_tools.html)
  describes drivers translating transactions to pins and monitors translating
  pin activity back into transactions.
