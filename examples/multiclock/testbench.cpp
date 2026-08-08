#include <cstdint>

#include "cpptb/cpptb.hpp"
#include "dut.hpp"

namespace cpptb::examples::dpi_multiclock {
namespace {

using cpptb::Dut;
using coro::Delay;
using coro::First;
using coro::Join;
using coro::NextTimeStep;
using coro::ReadOnly;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

constexpr uint32_t kTransferCount = 16;

Task<void> reset_dut(Dut dut, TestContext& test) {
    dut.rst_n.set(0);
    dut.write_valid.set(0);
    dut.write_data.set(0);
    dut.read_ready.set(0);
    dut.probe_in.set(0);

    co_await Delay{20_ns};
    test.expect_eq("reset delay deadline",
                   static_cast<uint32_t>(test.now().in_nanoseconds()), 20u);
    dut.rst_n.set(1);
}

Task<void> wait_reset_write(Dut dut) {
    while (dut.rst_n.get() == 0) {
        co_await RisingEdge{dut.write_clk};
    }
}

Task<void> wait_reset_read(Dut dut) {
    while (dut.rst_n.get() == 0) {
        co_await RisingEdge{dut.read_clk};
    }
}

Task<void> producer(Dut dut) {
    co_await wait_reset_write(dut);

    // The cocotb driver shape: RisingEdge resumes before the design
    // evaluates that edge, so a get() reads the value the DUT is about to
    // sample, and a set() applies after this edge's updates -- in time for
    // the next one. No settling delays anywhere.
    for (uint32_t value = 0; value < kTransferCount; ++value) {
        while (true) {
            co_await RisingEdge{dut.write_clk};
            if (dut.write_ready.get() != 0) break;
        }

        dut.write_data.set((0x40u + value) & 0xffu);
        dut.write_valid.set(1);

        co_await RisingEdge{dut.write_clk};
        dut.write_valid.set(0);
    }
}

Task<void> consumer(Dut dut, TestContext& test) {
    co_await wait_reset_read(dut);

    for (uint32_t expected = 0; expected < kTransferCount; ++expected) {
        while (true) {
            co_await RisingEdge{dut.read_clk};
            if (dut.read_valid.get() != 0) break;
        }

        test.expect_eq("mailbox payload", dut.read_data.get(),
                       (0x40u + expected) & 0xffu);
        dut.read_ready.set(1);

        co_await RisingEdge{dut.read_clk};
        dut.read_ready.set(0);
    }
}

Task<void> traffic(Dut dut, TestContext& test) {
    co_await Join{producer(dut), consumer(dut, test)};
    // The consumer returns at the pre-evaluation resume of its last commit
    // edge; ReadOnly is that timestep's settled region, where the final
    // counter increment is visible.
    co_await ReadOnly{};
    test.expect_eq("write count", dut.write_count.get(), kTransferCount);
    test.expect_eq("read count", dut.read_count.get(), kTransferCount);
}

Task<void> trigger_and_phase_probe(Dut dut, TestContext& test) {
    co_await Delay{7_ns};
    test.expect_eq("independent delay",
                   static_cast<uint32_t>(test.now().in_nanoseconds()), 7u);

    const auto winner = co_await First{RisingEdge{dut.read_clk}, Delay{100_ns}};
    test.expect_eq("First chose read clock", static_cast<uint32_t>(winner), 0u);

    // First resumed at t=10, where the read and write edges coincide. In
    // cpptb an edge await registered mid-timestep catches a later dispatch
    // in the same timestep -- defined behavior, but a pure-SV @(posedge)
    // armed after the coincident toggle waits for the next one. Step to
    // the next timestep first so the anchor edge is the same in both
    // worlds.
    co_await NextTimeStep{};
    co_await RisingEdge{dut.write_clk};
    dut.probe_in.set(0xa5);
    co_await ReadOnly{};
    test.expect_eq("ReadOnly settles combinational output",
                   dut.probe_echo.get(), 0xa5u);

    co_await NextTimeStep{};
    dut.probe_in.set(0x3c);
    co_await ReadOnly{};
    test.expect_eq("successive write settles by the next ReadOnly",
                   dut.probe_echo.get(), 0x3cu);
}

Task<void> output_clock_probe(Dut dut, TestContext& test) {
    co_await RisingEdge{dut.output_clk};
    test.expect_eq("DUT output clock edge",
                   static_cast<uint32_t>(test.now().in_nanoseconds()), 22u);
}

Task<void> multiclock_test(Dut dut, TestContext& test) {
    dut.write_clk.set_now(0);
    dut.read_clk.set_now(0);
    test.start_clock(dut.write_clk, 4_ns);
    test.start_clock(dut.read_clk, 6_ns, 1_ns);

    co_await Join{reset_dut(dut, test), traffic(dut, test),
                  trigger_and_phase_probe(dut, test),
                  output_clock_probe(dut, test)};
}

CPPTB_REGISTER_TEST(multiclock_test);

}  // namespace
}  // namespace cpptb::examples::dpi_multiclock
