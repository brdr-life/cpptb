# Counter

This is the smallest end-to-end cpptb bench. It starts a C++-owned clock,
drives reset and enable, explicitly allows the DUT to settle, and
checks the counter output.

```sh
make cpp-dpi-counter-run
make cpp-dpi-counter-sv-run
```

## Complete C++ testbench

The complete `examples/counter/testbench.cpp` stays self-contained:

```cpp
#include <cstdint>

#include "cpptb/cpptb.hpp"
#include "examples/counter/generated/counter_dut.hpp"

namespace cpptb::examples::counter {
namespace {

using cpptb::generated::counter::Dut;
using coro::Delay;
using coro::FallingEdge;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

constexpr uint32_t kCountCycles = 8;

Task<void> counter_sequence(Dut dut, TestContext& test) {
    dut.clk.set(0);
    test.start_clock(dut.clk, 10_ns);

    dut.rst_n.set(0);
    dut.enable.set(0);

    co_await clock_cycles(dut.clk, 2);
    co_await FallingEdge{dut.clk};

    dut.rst_n.set(1);
    dut.enable.set(1);

    for (uint32_t expected = 1; expected <= kCountCycles; ++expected) {
        co_await RisingEdge{dut.clk};
        co_await Delay{1_ps};
        test.expect_eq("enabled count", dut.count.get(), expected);
    }

    dut.enable.set(0);
    co_await RisingEdge{dut.clk};
    co_await Delay{1_ps};
    test.expect_eq("disabled count", dut.count.get(), kCountCycles);
}

CPPTB_REGISTER_TEST(counter_sequence);

}  // namespace

}  // namespace cpptb::examples::counter
```

`set()` changes driven values without advancing time. `RisingEdge` and
`FallingEdge` suspend until the requested simulator event. The explicit
`1_ps` delay after a rising edge lets sequential and downstream combinational
logic settle before `get()` samples the result.

Code generation needs only the RTL source:

```sh
uv run --frozen cpptb-codegen examples/counter/counter.sv
```

`start_clock()` is the C++ equivalent of the pure-SV `initial` clock process.
The first registered input clock is used for the result's primary-cycle count.
Its generated simulator-side driver keeps period and phase testbench-owned.

For a bench that owns its timing without a clock, continue with
[clockless timers](timer-only.md).
