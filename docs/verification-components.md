# Verification components

Reusable protocol and transaction components are an optional layer named
`cpptb_vc`. They are deliberately outside the core include tree and namespace:

```cpp
#include "cpptb/cpptb.hpp"          // scheduler, signals, tests
#include "cpptb_vc/cpptb_vc.hpp"   // optional verification components

using namespace cpptb::vc;
```

The core umbrella does not include `cpptb_vc`. In CMake, consumers can make
the dependency equally explicit:

```cmake
target_link_libraries(my_testbench PRIVATE cpptb::cpptb cpptb::vc)
```

`cpptb::vc` is currently shipped from the same repository, but it is a
separate interface target whose only dependency is the public `cpptb` API.
The component headers do not include generated DUT bindings, a simulator DPI
adapter, or project-specific RTL. That boundary allows the component library
to move into a separately versioned package later without changing its public
namespace or include paths.

## Package layout

| Header | Responsibility |
|---|---|
| `cpptb_vc/ports.hpp` | Put/get endpoints, analysis fan-out, and explicit analysis buffering |
| `cpptb_vc/scoreboards.hpp` | In-order and keyed scoreboards plus reference-model publication |
| `cpptb_vc/stream.hpp` | Stream concepts and ready/valid source, sink, and monitor components |
| `cpptb_vc/memory_mapped.hpp` | Protocol-neutral requests, responses, transactions, statuses, and master concept |
| `cpptb_vc/memory_model.hpp` | Sparse expected memory, regions, callbacks, images, and passive prediction |
| `cpptb_vc/register_model.hpp` | Typed registers, fields, memories, mirrors, and frontdoor/backdoor adapters |
| `cpptb_vc/register_coverage.hpp` | Optional register, field, and register-memory access coverage |
| `cpptb_vc/register_sequences.hpp` | Reusable reset-check, access-check, and policy-aware bit-bash sequences |
| `cpptb_vc/apb.hpp` | APB master, passive monitor, and protocol checker |
| `cpptb_vc/cpptb_vc.hpp` | Convenience umbrella for the complete optional package |

The old `cpptb/components.hpp` header remains a monorepo compatibility shim for
the first endpoint APIs. New code should import `cpptb_vc` directly. The shim
is deprecated for new integrations: before `cpptb_vc` becomes a separately
versioned distribution, it will move with that distribution or be removed so
the core package never acquires a reverse dependency on optional components.

## Component guides

Keep core testbenches on `cpptb::cpptb` and add only the optional component
layers the environment needs:

| Guide | Use it for |
|---|---|
| [Transaction recording](verification-components/transaction-recording.md) | Record monitor-derived typed transactions to memory or JSON Lines |
| [Sparse expected memory](verification-components/memory-model.md) | Images, expected byte storage, byte enables, permissions, and passive prediction |
| [Register abstraction layer](memory-register-models.md) | Typed registers and fields, desired/mirrored state, frontdoor/backdoor access, and side effects |
| [Generate register models](verification-components/register-generation.md) | SystemRDL, IP-XACT, or native RgGen input, build dependencies, naming, and limitations |
| [Standard register sequences](verification-components/register-sequences.md) | Reset checks, frontdoor/backdoor agreement, bit-bash policy, summaries, and performance peer |

These components depend on scheduler tasks and generic transaction interfaces;
the scheduler and generated DUT API do not depend on them.
This packaging follows the same useful boundary described by Cocotb's
[extension guidance](https://docs.cocotb.org/en/stable/extensions.html): the
simulation framework remains small while reusable verification IP can be
installed, documented, and versioned separately.

## One component in four frameworks

The following tabs show the same component boundary in four familiar styles.
Each monitor waits for a ready/valid transfer, samples the 32-bit payload, and
publishes it to downstream checking code. The examples intentionally use each
ecosystem's native communication mechanism instead of forcing identical class
shapes onto different languages.

<div class="cpptb-code-tabs" data-tabs="4" data-tab-group="ready-valid-component-comparison" data-tab-label="Ready-valid monitor component"></div>

<div class="cpptb-code-tab-label">cpptb-vc (C++ DPI)</div>

```cpp
template <typename Clock, typename Valid, typename Ready, typename Data>
class ReadyValidMonitor {
  public:
    using value_type = typename Data::value_type;

    ReadyValidMonitor(Clock clock, Valid valid, Ready ready, Data data,
                      coro::SimTime sample_delay)
        : clock_(clock), valid_(valid), ready_(ready), data_(data),
          sample_delay_(sample_delay) {}

    coro::Task<void> run(AnalysisPort<value_type>& observed,
                         std::size_t transaction_count) {
        std::size_t received = 0;
        while (received < transaction_count) {
            co_await coro::RisingEdge{static_cast<coro::Signal>(clock_)};
            if (sample_delay_.in_femtoseconds() != 0) {
                co_await coro::Delay{sample_delay_};
            }
            if (valid_.get() == 0 || ready_.get() == 0) continue;

            observed.write(data_.get());
            ++received;
        }
    }

  private:
    Clock clock_;
    Valid valid_;
    Ready ready_;
    Data data_;
    coro::SimTime sample_delay_;
};
```

<div class="cpptb-code-tab-label">Cocotb</div>

```python
from cocotb.queue import Queue
from cocotb.triggers import ReadOnly, RisingEdge


class ReadyValidMonitor:
    def __init__(self, clock, valid, ready, data):
        self.clock = clock
        self.valid = valid
        self.ready = ready
        self.data = data
        self.observed = Queue()

    async def run(self, transaction_count):
        received = 0
        while received < transaction_count:
            await RisingEdge(self.clock)
            await ReadOnly()
            if not self.valid.value or not self.ready.value:
                continue

            self.observed.put_nowait(int(self.data.value))
            received += 1
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
interface ready_valid_if(input logic clk);
  logic        valid;
  logic        ready;
  logic [31:0] data;
endinterface

class ready_valid_monitor;
  virtual ready_valid_if       vif;
  mailbox #(logic [31:0])      observed;

  function new(virtual ready_valid_if vif,
               mailbox #(logic [31:0]) observed);
    this.vif = vif;
    this.observed = observed;
  endfunction

  task run(int unsigned transaction_count);
    int unsigned received = 0;
    while (received < transaction_count) begin
      @(posedge vif.clk);
      #1ps;
      if (!vif.valid || !vif.ready) continue;

      observed.put(vif.data);
      received++;
    end
  endtask
endclass
```

<div class="cpptb-code-tab-label">UVM</div>

```systemverilog
class ready_valid_transaction extends uvm_sequence_item;
  `uvm_object_utils(ready_valid_transaction)
  logic [31:0] data;

  function new(string name = "ready_valid_transaction");
    super.new(name);
  endfunction
endclass

class ready_valid_monitor extends uvm_monitor;
  `uvm_component_utils(ready_valid_monitor)

  virtual ready_valid_if                         vif;
  uvm_analysis_port #(ready_valid_transaction)   observed;

  function new(string name, uvm_component parent);
    super.new(name, parent);
    observed = new("observed", this);
  endfunction

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    if (!uvm_config_db #(virtual ready_valid_if)::get(
            this, "", "vif", vif)) begin
      `uvm_fatal("NO_VIF", "ready_valid_if was not configured")
    end
  endfunction

  task run_phase(uvm_phase phase);
    forever begin
      ready_valid_transaction transaction;
      @(posedge vif.clk);
      #1ps;
      if (!vif.valid || !vif.ready) continue;

      transaction = ready_valid_transaction::type_id::create("transaction");
      transaction.data = vif.data;
      observed.write(transaction);
    end
  endtask
endclass
```

The structural correspondence is more important than identical spelling:

| Concern | cpptb-vc | Cocotb | Pure SystemVerilog | UVM |
|---|---|---|---|---|
| Component lifecycle | An ordinary `Task` joined or spawned by the test | An `async` method started by the test | A `task` started with `fork` or from the test module | A component `run_phase` |
| Transaction output | Fan-out `AnalysisPort<T>` | `Queue` or package-defined callbacks | `mailbox` or authored callbacks | Fan-out `uvm_analysis_port<T>` |
| DUT connection | Generated typed signal handles | Simulator hierarchy handles | `virtual interface` | A `virtual interface`, commonly supplied through `uvm_config_db` |
| Packaging | Optional `cpptb_vc` CMake target and headers | Python module or add-on package | SystemVerilog package/interface files | UVM package and agent hierarchy |

cpptb-vc deliberately adopts the lightweight transaction-connectivity idea
from UVM without requiring UVM's factory, phase, objection, or configuration
machinery. Cocotb similarly keeps scheduling and DUT handles in its core while
reusable components can live in ordinary Python or separate packages. See the
official Cocotb documentation for [`Queue`](https://docs.cocotb.org/en/stable/library_reference.html#cocotb.queue.Queue)
and the [timing model](https://docs.cocotb.org/en/stable/timing_model.html), and
the [Accellera UVM 1.2 User's Guide](https://www.accellera.org/images/downloads/standards/uvm/uvm_users_guide_1.2.pdf)
for monitors and analysis ports.

The cpptb-vc and pure-SystemVerilog forms have complete runnable equivalents
in the [component FIFO example](examples/component-fifo.md). The Cocotb and UVM
tabs above are concise architectural references, not additional performance
measurements.

## Protocol-neutral transactions

A sequence can depend on the `MemoryMappedMaster` concept instead of APB:

```cpp
template <MemoryMappedMaster Master>
Task<void> program_control(Master& bus, TestContext& test) {
    const auto write = co_await bus.write(0x10u, 0x0000'0003u);
    test.require_eq("control write", write.status, MemoryStatus::Okay);

    const auto read = co_await bus.read(0x10u);
    test.expect_eq("control readback", read.data, 0x0000'0003u);
}
```

The concrete master supplies its address, data, byte-enable, request, and
response types. `MemoryWriteResponse` and `MemoryReadResponse<T>` retain the
protocol-independent status and number of wait cycles. A timeout is returned
as `MemoryStatus::Timeout`; it is not silently converted into a test failure.
The calling sequence decides whether that response is expected, nonfatal, or
fatal.

`StreamSource` and `StreamSink` provide the corresponding compile-time
contracts for packet and word streams. `ReadyValidDriver` and
`ReadyValidSink` implement those contracts, while `ReadyValidMonitor` remains
passive and publishes accepted transfers through an `AnalysisPort`.

## APB components

Create a typed bus from generated signals and pass it to each component:

```cpp
const auto bus = ApbBus{
    dut.clk,           dut.apb_select,    dut.apb_enable,
    dut.apb_write,     dut.apb_address,   dut.apb_write_data,
    dut.apb_read_data, dut.apb_ready,     dut.apb_error};

ApbMaster master{bus, ApbConfig{.sample_delay = 1_ps,
                                .max_wait_cycles = 32}};
ApbMonitor monitor{test, bus, 1_ps};
ApbProtocolChecker checker{test, bus, 1_ps};
```

The components carry both write models. Under `deferred_writes = true`
they take the cocotb shape: drives anchor on the rising edge and apply
after that edge's own updates. The APB master's completion loop, the APB
monitor, and the protocol checker then observe at the pre-evaluation
resume -- the values the design sampled at the edge -- and do not await
`sample_delay`; the ready-valid stream driver deliberately keeps its
post-edge `sample_delay` ready check in both models, mirroring its pure-SV
twin. Under immediate writes everything keeps the falling-edge drive
points and post-edge sampling. One counting consequence is user-visible
on the APB side: pre-evaluation observation reports a wait state for
**every** access cycle with `PREADY` low, where post-edge sampling skipped
the first one; calibrated wait-cycle expectations differ between the
modes accordingly.

Pass a tenth signal to `ApbBus` when `PSTRB` is present. Without it, requests
still carry a byte-enable value so a generic sequence has one stable shape;
an APB3-style master simply cannot apply partial writes.

For APB4, `ApbMaster::all_bytes()` derives the full-write mask from the HDL
width of `PSTRB`, even when the generated C++ storage type is wider. The master
drives `PSTRB` to zero during reads as required by APB. Monitored read
transactions remain protocol-neutral and report `all_bytes()`; the physical
read strobe is a bus control rule, not a partial-read request.

`ApbMaster` serializes concurrent callers with a cancellation-safe lock. Its
coroutines explicitly drive setup, drive access, wait on `PREADY`, sample the
response, and return the bus to idle. It does not start a clock, reset the
DUT, spawn itself, or add an unrequested sampling delay.

`ApbMonitor` never drives a signal. Its `observed()` analysis port publishes
completed reads and writes as timed `TransactionObservation<MemoryTransaction>`
values, including byte enables, response status, and wait cycles. Scoreboards,
predictors, and register coverage accept completed observations directly.
`ApbProtocolChecker` independently reports malformed setup/access
transitions, a nonzero read `PSTRB`, and control changes while waiting for
`PREADY`. `PWDATA` stability is checked only for writes because it is not a
read control signal.

Connect a recorder when a persistent or in-memory transaction timeline is
useful. The monitor performs protocol decoding once and fans the same typed
observation out to every consumer:

```cpp
TransactionRecorder recorder;
InMemoryTransactionSink trace;
auto trace_sink = recorder.connect(trace);
auto& stream = recorder.stream<Transaction>("apb0.observed");

auto checking = monitor.observed().connect(scoreboard.actual());
auto recording = monitor.observed().connect(stream);
co_await monitor.run(expected_transfer_count);
```

See [Transaction recording](verification-components/transaction-recording.md)
and the runnable [APB trace example](examples/apb-trace.md).

## Prediction and checking

Use an in-order scoreboard when responses preserve request order. Use a keyed
scoreboard when transactions can return in another order:

```cpp
const auto transaction_id = [](const Response& response) {
    return response.id;
};
KeyedScoreboard<Response, decltype(transaction_id)> scoreboard{
    test, "response", transaction_id};

auto expected_connection = predicted.connect(scoreboard.expected());
auto actual_connection = observed.connect(scoreboard.actual());
```

Duplicate keys are retained in FIFO order. `finalize()` reports unmatched
expected and actual values as nonfatal checks. Transaction text is formatted
only after a mismatch, so successful comparisons do not allocate diagnostic
strings.

`ReferenceModelAdapter<Input>` turns an ordinary callable into an analysis
subscriber and publishes its return value:

```cpp
auto predictor = make_reference_model<Request>(
    [](const Request& request) { return model(request); });
auto connection = predictor.predicted.connect(scoreboard.expected());
auto request_connection = requests.connect(predictor);
```

The complete [APB register-file example](examples/apb-regfile.md) connects a
master, monitor, checker, scoreboard, and coverage model. The
[component FIFO](examples/component-fifo.md) demonstrates the stream and
analysis components.

The same APB monitor can feed a protocol-independent
[sparse memory predictor](verification-components/memory-model.md). Use the
[register abstraction layer](memory-register-models.md) when the environment
also needs typed fields, desired/mirrored state, or generated SystemRDL access.

## Performance qualification

The exact `apb_component` benchmark performs the same APB pin sequence,
monitor publication, protocol checking, scoreboard comparisons, response
checks, and checksum calculation in C++ DPI and pure SystemVerilog:

```sh
make feature-test FEATURE=apb_component
make feature-benchmark FEATURE=apb_component
```

The valid July 17, 2026 run measured `0.916x` C++ DPI over pure SystemVerilog
at 100,000 write/read pairs, with `0.925x` DPI-first, `0.899x` SV-first,
`0.920x` independent, and `0.45%` paired/independent disagreement. It passes
the standard `1.10x` hard guard.
