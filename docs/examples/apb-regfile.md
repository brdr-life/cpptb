# APB register file

This example uses the optional `cpptb_vc` package rather than defining a local
bus helper. The registered test owns one active master and three passive
consumers: a monitor, protocol checker, and functional coverage subscriber.
The monitor also feeds an in-order transaction scoreboard.

The example registers a second `memory_model_apb_test` that replaces the
hand-authored expected queue with `SparseMemory` and `MemoryPredictor`. It
covers writable regions, a read-only ID image, and error translation without
putting APB timing into the model.

## Construct the bus

Generated signals are assembled into a typed `ApbBus`. No configuration file
or DUT-specific component subclass is required:

```cpp
#include "cpptb/cpptb.hpp"
#include "cpptb_vc/cpptb_vc.hpp"

using namespace cpptb::vc;

auto make_apb_bus(Dut dut) {
    return ApbBus{dut.clk,           dut.apb_select,    dut.apb_enable,
                  dut.apb_write,     dut.apb_address,   dut.apb_write_data,
                  dut.apb_read_data, dut.apb_ready,     dut.apb_error};
}
```

An APB4 design can pass `PSTRB` as a tenth signal. The same master and monitor
types then retain the strobe in every generic memory transaction.

## Write a reusable sequence

The sequence is templated on the protocol-neutral `MemoryMappedMaster`
concept. It can be reused with a future AXI-Lite, Wishbone, or custom adapter:

```cpp
template <MemoryMappedMaster BusMaster>
Task<void> register_sequence(
    BusMaster& apb, TestContext& test,
    AnalysisPort<typename BusMaster::transaction_type>& expected) {
    uint32_t state = 0x1020'3040u;

    for (uint32_t index = 0; index < kRegisterTransactions; ++index) {
        const uint32_t address = (index % 4u) * 4u;
        const uint32_t value = next_word(state);

        const auto write = co_await apb.write(address, value);
        test.expect_eq("APB write status", write.status,
                       MemoryStatus::Okay);

        const auto read = co_await apb.read(address);
        test.expect_eq("APB register readback", read.data, value);
        test.expect_eq("APB read status", read.status,
                       MemoryStatus::Okay);
    }
}
```

Every protocol operation remains an explicit `co_await`. A response retains
its status and wait-cycle count; the user decides how to check it.

## Compose the test

```cpp
Task<void> component_apb_test(Dut dut, TestContext& test) {
    dut.clk.set(0);
    test.start_clock(dut.clk, 10_ns);
    co_await reset_dut(dut);

    const auto bus = make_apb_bus(dut);
    Master master{bus, ApbConfig{.sample_delay = 1_ps}};
    ApbMonitor monitor{bus, 1_ps};
    ApbProtocolChecker checker{test, bus, 1_ps};
    AnalysisPort<Transaction> expected;
    AnalysisPort<Transaction> observed;
    InOrderScoreboard<Transaction> scoreboard{test, "APB transaction"};

    auto expected_connection = expected.connect(scoreboard.expected());
    auto actual_connection = observed.connect(scoreboard.actual());
    auto coverage_connection = observed.connect(coverage_subscriber);
    test.spawn_detached(checker.run_forever());

    co_await Join{register_sequence(master, test, expected),
                  monitor.run(observed, kRegisterTransactions * 2u)};

    scoreboard.finalize();
    test.expect_eq("APB protocol violations", checker.violations(),
                   uint64_t{0});
}

CPPTB_REGISTER_TEST(component_apb_test);
```

The root test explicitly starts the clock and resets the DUT. The master owns
only APB transfer timing, while the passive components are started by authored
test code.

## C++ and pure SV

<div class="cpptb-code-tabs" data-tabs="2" data-tab-group="apb-components" data-tab-label="APB transaction"></div>

<div class="cpptb-code-tab-label">cpptb_vc (C++ DPI)</div>

```cpp
const auto write = co_await master.write(address, value);
test.expect_eq("APB write status", write.status, MemoryStatus::Okay);

const auto read = co_await master.read(address);
test.expect_eq("APB read data", read.data, value);
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
@(negedge clk);
paddr = address;
pwdata = value;
pstrb = 4'hf;
pwrite = 1'b1;
psel = 1'b1;
penable = 1'b0;
@(posedge clk);
@(negedge clk);
penable = 1'b1;
do @(posedge clk); while (!pready);
expect_eq("APB write status", pslverr, 1'b0);
@(negedge clk);
psel = 1'b0;
penable = 1'b0;
```

Run the example implementations with:

```sh
make cpp-dpi-apb-regfile-run
make cpp-dpi-apb-regfile-sv-run
```

Run the 100,000-iteration exact component benchmark with:

```sh
make feature-test FEATURE=apb_component
make feature-benchmark FEATURE=apb_component
```

Run the equivalent sparse-memory predictor pair with:

```sh
make feature-test FEATURE=memory_model
make feature-benchmark FEATURE=memory_model
```

The authored register contract is in `examples/apb_regfile/registers.rdl` and
can be exported through PeakRDL as described in
[the four-framework register workflow](../memory-register-models.md#one-register-workflow-in-four-frameworks).

See [Verification components](../verification-components.md) for the package
boundary and complete component contracts.
