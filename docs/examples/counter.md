# Counter

This is the smallest end-to-end cpptb bench. It starts a C++-owned clock,
drives reset and enable, explicitly allows the DUT to settle, and
checks the counter output.

Its authored files, generated files, and standalone Makefile are described in
[Project layout and build ownership](../running-tests.md#project-layout-and-build-ownership).

```sh
make cpp-dpi-counter-run
make cpp-dpi-counter-sv-run
make cpp-dpi-counter-suite-test
```

## Complete C++ testbench

The complete `examples/counter/testbench.cpp` stays self-contained:

```cpp
#include <cstdint>

#include "cpptb/cpptb.hpp"
#include "dut.hpp"

namespace cpptb::examples::counter {
namespace {

using cpptb::Dut;
using coro::NextTimeStep;
using coro::ReadOnly;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

constexpr uint32_t kCountCycles = 8;

Task<void> counter_sequence(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
    test.start_clock(dut.clk, 10_ns);

    dut.rst_n.set(0);
    dut.enable.set(0);

    // The cocotb shape: write straight after the awaited edge; the deferred
    // flush applies it after this edge's own updates, so the release is
    // seen by the next rising edge -- no negedge detour.
    co_await clock_cycles(dut.clk, 2);
    dut.rst_n.set(1);
    dut.enable.set(1);

    for (uint32_t expected = 1; expected <= kCountCycles; ++expected) {
        co_await RisingEdge{dut.clk};
        co_await ReadOnly{};
        test.expect_eq("enabled count", dut.count.get(), expected);
    }

    // Writes are illegal inside ReadOnly (cocotb's rule too); step out of
    // the region first.
    co_await NextTimeStep{};
    dut.enable.set(0);
    co_await RisingEdge{dut.clk};
    co_await ReadOnly{};
    test.expect_eq("disabled count", dut.count.get(), kCountCycles);
}

Task<void> counter_reset_defaults(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
    test.start_clock(dut.clk, 10_ns);

    dut.rst_n.set(0);
    dut.enable.set(1);

    co_await RisingEdge{dut.clk};
    co_await ReadOnly{};
    test.expect_eq("reset count", dut.count.get(), 0u);
}

CPPTB_REGISTER_TEST(counter_sequence);
CPPTB_REGISTER_TEST(counter_reset_defaults);

}  // namespace

}  // namespace cpptb::examples::counter
```

`make cpp-dpi-counter-run` runs the registered catalog while the benchmark
adapter selects the original `counter_sequence` workload for its pure-SV
comparison. The same user flow is available directly:

```sh
uv run --frozen cpptb list --project examples/counter --build-dir build
uv run --frozen cpptb test --project examples/counter --build-dir build
```

Like every example here, this project sets `deferred_writes = true` -- the
standard cocotb write model. A
`set()` right after an awaited edge queues and applies after that edge's own
updates, so it is first seen by the next rising edge; `set_now()` is the
immediate escape hatch used to initialize the clock pin before
`start_clock()`. Sampling awaits `ReadOnly{}`, the settled region of the
same timestep, instead of an explicit delay.

The build command generates the typed interface from RTL, recovers the
testbench's hierarchy usage from compile-only objects, finalizes the
wrapper, and invokes Verilator:

```sh
uv run --frozen cpptb build --project examples/counter --build-dir build
```

`start_clock()` is the C++ equivalent of the pure-SV `initial` clock process.
The first registered input clock is used for the result's primary-cycle count.
Its generated simulator-side driver keeps period and phase testbench-owned.

For a bench that owns its timing without a clock, continue with
[clockless timers](timer-only.md).
