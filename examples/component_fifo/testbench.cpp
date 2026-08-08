#include <cstdint>

#include "cpptb/cpptb.hpp"
#include "cpptb_vc/cpptb_vc.hpp"
#include "dut.hpp"

namespace cpptb::examples::component_fifo {
namespace {

using cpptb::Dut;
using coro::Delay;
using coro::Event;
using coro::Join;
using coro::RisingEdge;
using coro::Task;
using namespace coro;
using namespace cpptb::vc;

constexpr uint32_t kWordCount = 24;

uint32_t next_word(uint32_t& state) {
    state = state * 1'664'525u + 1'013'904'223u;
    return state;
}

Task<void> reset_dut(Dut dut, Event& reset_done) {
    dut.rst_n.set(0);
    dut.in_valid.set(0);
    dut.in_data.set(0);
    dut.out_ready.set(0);

    co_await clock_cycles(dut.clk, 2);
    dut.rst_n.set(1);
    reset_done.set();
}

Task<void> input_sequence(Dut dut, Event& reset_done,
                          AnalysisPort<uint32_t>& expected,
                          uint32_t& input_stalls) {
    co_await reset_done;
    // No sample delay: under deferred writes the driver reads the
    // handshake at the pre-evaluation resume -- the values the design
    // samples at that edge.
    ReadyValidDriver driver{dut.clk, dut.in_valid, dut.in_ready, dut.in_data,
                            {}};

    uint32_t state = 0x3141'5926u;
    for (uint32_t index = 0; index < kWordCount; ++index) {
        const uint32_t word = next_word(state);
        expected.write(word);
        input_stalls += co_await driver.send(word);
    }
}

Task<void> output_ready_driver(Dut dut, Event& reset_done) {
    co_await reset_done;

    uint32_t cycle = 0;
    uint32_t accepted = 0;
    while (accepted < kWordCount) {
        co_await RisingEdge{dut.clk};
        if (dut.out_ready.get() != 0 && dut.out_valid.get() != 0) ++accepted;
        const uint32_t ready = cycle % 5u == 0u ? 1u : 0u;
        dut.out_ready.set(ready);
        ++cycle;
    }
    // One more edge before dropping ready: the falling-edge monitor and the
    // drained check both need the final accept to commit first.
    co_await RisingEdge{dut.clk};
    dut.out_ready.set(0);
}

Task<void> output_monitor(Dut dut, Event& reset_done,
                          AnalysisPort<uint32_t>& observed) {
    co_await reset_done;
    ReadyValidMonitor monitor{
        dut.clk, dut.out_valid, dut.out_ready, dut.out_data,
        ReadyValidSampleEdge::Falling, {}};
    co_await monitor.run(observed, kWordCount);
}

Task<void> audit_stream(TestContext& test, GetPort<uint32_t> audit,
                        uint32_t& audited) {
    uint32_t state = 0x3141'5926u;
    for (uint32_t index = 0; index < kWordCount; ++index) {
        test.expect_eq("audit payload", co_await audit.get(),
                       next_word(state));
        ++audited;
    }
}

Task<void> component_fifo_test(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
    test.start_clock(dut.clk, 10_ns);

    Event reset_done;
    AnalysisPort<uint32_t> expected;
    AnalysisPort<uint32_t> observed;
    InOrderScoreboard<uint32_t> scoreboard{test, "FIFO payload"};
    AnalysisBuffer<uint32_t> audit{
        4, AnalysisOverflowPolicy::Error};
    auto expected_connection = expected.connect(scoreboard.expected());
    auto actual_connection = observed.connect(scoreboard.actual());
    auto audit_connection = observed.connect(audit);
    uint32_t input_stalls = 0;
    uint32_t audited = 0;

    co_await Join{reset_dut(dut, reset_done),
                  input_sequence(dut, reset_done, expected, input_stalls),
                  output_ready_driver(dut, reset_done),
                  output_monitor(dut, reset_done, observed),
                  audit_stream(test, audit.output(), audited)};

    scoreboard.finalize();
    test.expect_eq("scoreboard comparisons", scoreboard.compared(),
                   std::size_t{kWordCount});
    test.expect_eq("audit transactions", audited, kWordCount);
    test.expect_eq("audit buffer drained", audit.empty(), true);
    test.expect_eq("FIFO drained", dut.out_valid.get(), 0u);
    test.expect_eq("FIFO accepts after drain", dut.in_ready.get(), 1u);
    test.expect_eq("input backpressure observed", input_stalls != 0, true);
}

CPPTB_REGISTER_TEST(component_fifo_test);

}  // namespace
}  // namespace cpptb::examples::component_fifo
