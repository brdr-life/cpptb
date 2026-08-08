#include <cstdint>

#include "cpptb/cpptb.hpp"
#include "dut.hpp"

namespace cpptb::examples::fault_injection {
namespace {

using cpptb::Dut;
using coro::NextTimeStep;
using coro::ReadOnly;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

Task<void> fault_injection_sequence(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
    test.start_clock(dut.clk, 10_ns);

    dut.rst_n.set(0);
    dut.source_i.set(0);
    dut.memory_address.set(0);
    dut.memory_write.set(0);
    dut.memory_write_data.set(0);

    co_await clock_cycles(dut.clk, 2);
    dut.rst_n.set(1);

    // No sub-cycle settling delays: a check runs in ReadOnly -- the settled
    // region of the current timestep -- and the next mutation steps out of
    // it with NextTimeStep. The running clock provides the timesteps.
    co_await RisingEdge{dut.clk};
    co_await ReadOnly{};
    test.expect_eq("counter baseline", dut.counter_o.get(), 1u);
    co_await NextTimeStep{};

    dut.source_i.set(0x12);
    co_await ReadOnly{};
    test.expect_eq("resolved net baseline", dut.resolved_o.get(), 0x48u);
    co_await NextTimeStep{};

    dut.resolved_value.force(0xa5);
    test.expect_eq("force is immediately readable",
                   dut.resolved_value.get(), 0xa5u);
    co_await ReadOnly{};
    test.expect_eq("forced net reaches output", dut.resolved_o.get(), 0xa5u);
    co_await NextTimeStep{};

    dut.source_i.set(0x34);
    co_await ReadOnly{};
    test.expect_eq("force overrides changing driver", dut.resolved_o.get(),
                   0xa5u);
    co_await NextTimeStep{};

    dut.resolved_value.release();
    co_await ReadOnly{};
    test.expect_eq("release restores resolved driver", dut.resolved_o.get(),
                   0x6eu);
    co_await NextTimeStep{};

    dut.counter.force(0x55);
    co_await clock_cycles(dut.clk, 2);
    co_await ReadOnly{};
    test.expect_eq("RTL writes do not override force", dut.counter_o.get(),
                   0x55u);
    co_await NextTimeStep{};

    dut.counter.release();
    co_await RisingEdge{dut.clk};
    co_await ReadOnly{};
    test.expect_eq("RTL writes resume after release", dut.counter_o.get(),
                   0x56u);
    co_await NextTimeStep{};

    dut.memory[2].deposit(0xbeef);
    test.expect_eq("deposit is immediately readable",
                   dut.memory[2].get(), 0xbeefu);
    dut.memory_address.set(2);
    co_await ReadOnly{};
    test.expect_eq("deposit reaches memory output", dut.memory_read_data.get(),
                   0xbeefu);
    co_await NextTimeStep{};

    dut.memory[2].force(0xcafe);
    test.expect_eq("memory force is immediately readable",
                   dut.memory[2].get(), 0xcafeu);
    co_await ReadOnly{};
    test.expect_eq("memory force reaches output", dut.memory_read_data.get(),
                   0xcafeu);
    co_await NextTimeStep{};
    dut.memory[2].release();

    dut.memory_write_data.set(0x1234);
    dut.memory_write.set(1);
    co_await RisingEdge{dut.clk};
    co_await ReadOnly{};
    test.expect_eq("front-door write follows release",
                   dut.memory_read_data.get(), 0x1234u);
    co_await NextTimeStep{};
    dut.memory_write.set(0);
}

CPPTB_REGISTER_TEST(fault_injection_sequence);

}  // namespace
}  // namespace cpptb::examples::fault_injection
