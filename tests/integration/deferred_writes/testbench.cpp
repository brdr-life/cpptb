// The deferred-write contract, pinned against the exact example
// docs/scheduling.md marks "Wrong" for immediate writes:
//
//     co_await RisingEdge{dut.clk};
//     dut.wdata.set(value);
//
// Under deferred_writes = true that shape is correct -- the cocotb write
// model. Each check below states the clause of the contract it pins.

#include <cpptb/cpptb.hpp>

#include "dut.hpp"

using cpptb::Dut;
using cpptb::TestContext;
using cpptb::coro::Delay;
using cpptb::coro::NextTimeStep;
using cpptb::coro::ReadOnly;
using cpptb::coro::RisingEdge;
using cpptb::coro::Task;
using namespace cpptb::coro;

Task<void> cocotb_shaped_driver(Dut dut, TestContext& test) {
    // Initialization uses the escape hatch on purpose: set_now() is
    // cocotb's setimmediatevalue(), the immediate deposit.
    dut.clk.set_now(0);
    dut.d.set_now(0);
    test.start_clock(dut.clk, 10_ns);

    // Let the pipeline flush the initial zero through both stages.
    co_await clock_cycles(dut.clk, 3);

    for (uint8_t value = 1; value <= 8; ++value) {
        co_await RisingEdge{dut.clk};

        // The cocotb driver shape: write straight after the edge.
        dut.d.set(value);

        // Contract: a get() between set() and the flush reads the
        // simulator's value, not the queued one -- cocotb's caching.
        test.expect_eq("get() sees the simulator's value before the flush",
                       dut.d.get(), static_cast<uint8_t>(value - 1));

        // Contract: the flush happens at this timestep's ReadWrite point,
        // after the awaited edge's own updates. In ReadOnly the write is
        // applied and settled -- and q proves the edge just awaited sampled
        // the OLD d: a cycle-early capture would show `value` here.
        co_await ReadOnly{};
        test.expect_eq("the flush applied the write by ReadOnly",
                       dut.d.get(), value);
        test.expect_eq("the awaited edge sampled the previous value",
                       dut.q.get(), static_cast<uint8_t>(value - 1));

        // Leave the read-only region before the next iteration writes.
        co_await NextTimeStep{};
    }
}

CPPTB_REGISTER_TEST(cocotb_shaped_driver);

Task<void> set_now_is_the_old_semantics(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
    dut.d.set_now(0);
    test.start_clock(dut.clk, 10_ns);
    co_await clock_cycles(dut.clk, 3);

    // The escape hatch keeps the immediate behavior: RisingEdge resumes
    // before the design evaluates the edge, so an immediate write IS picked
    // up by the edge being awaited -- the documented cycle-early capture the
    // deferred mode exists to prevent.
    co_await RisingEdge{dut.clk};
    dut.d.set_now(42);
    co_await ReadOnly{};
    test.expect_eq("set_now() is captured by the awaited edge",
                   dut.q.get(), 42);
}

CPPTB_REGISTER_TEST(set_now_is_the_old_semantics);
