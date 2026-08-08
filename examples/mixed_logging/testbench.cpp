#include "cpptb/cpptb.hpp"
#include "dut.hpp"

using namespace cpptb;
using namespace cpptb::coro;

Task<void> mixed_language_logging(Dut dut, TestContext& test) {
    auto log = test.logger("cpp.driver");
    log.info("starting mixed-language traffic");

    dut.clk.set_now(0);
    test.start_clock(dut.clk, 10_ns);
    dut.rst_n.set(0);
    dut.valid.set(0);
    dut.data.set(0);
    co_await clock_cycles(dut.clk, 2);
    dut.rst_n.set(1);

    for (uint32_t index = 0; index < 2; ++index) {
        co_await RisingEdge{dut.clk};
        dut.data.set(0x1234'0000u + index);
        dut.valid.set(1);
        co_await RisingEdge{dut.clk};
        co_await ReadOnly{};
        test.expect_eq("accepted count", dut.accepted_count.get(), index + 1);
        log.info([&] {
            return "observed accepted count=" + std::to_string(index + 1);
        });
        co_await NextTimeStep{};
        dut.valid.set(0);
    }
}

CPPTB_REGISTER_TEST(mixed_language_logging);
