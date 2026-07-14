#include <cstdint>

#include "cpptb/cpptb.hpp"
#include "examples/fifo_scoreboard/generated/stream_fifo_dut.hpp"

namespace cpptb::examples::fifo_scoreboard {
namespace {

using cpptb::generated::stream_fifo::Dut;
using coro::Channel;
using coro::Delay;
using coro::Event;
using coro::FallingEdge;
using coro::Join;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

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
    co_await FallingEdge{dut.clk};
    dut.rst_n.set(1);
    reset_done.set();
}

Task<void> input_driver(Dut dut, Event& reset_done,
                        Channel<uint32_t>& expected_words,
                        uint32_t& input_stalls) {
    co_await reset_done;

    uint32_t state = 0x3141'5926u;
    for (uint32_t index = 0; index < kWordCount; ++index) {
        const uint32_t word = next_word(state);
        while (true) {
            co_await FallingEdge{dut.clk};
            dut.in_data.set(word);
            dut.in_valid.set(1);
            co_await Delay{1_ps};

            if (dut.in_ready.get() == 0) {
                ++input_stalls;
                continue;
            }
            expected_words.put_nowait(word);
            co_await RisingEdge{dut.clk};
            co_await Delay{1_ps};
            dut.in_valid.set(0);
            break;
        }
    }
}

Task<void> output_ready_driver(Dut dut, Event& reset_done) {
    co_await reset_done;

    uint32_t cycle = 0;
    uint32_t accepted = 0;
    while (accepted < kWordCount) {
        co_await FallingEdge{dut.clk};
        const uint32_t ready =
            (cycle % 5u == 1u || cycle % 5u == 2u) ? 0u : 1u;
        dut.out_ready.set(ready);
        co_await Delay{2_ps};

        if (ready != 0 && dut.out_valid.get() != 0) ++accepted;
        ++cycle;
    }

    co_await RisingEdge{dut.clk};
    co_await Delay{1_ps};
    dut.out_ready.set(0);
}

Task<void> output_monitor(Dut dut, Event& reset_done,
                          Channel<uint32_t>& observed_words) {
    co_await reset_done;

    uint32_t observed = 0;
    while (observed < kWordCount) {
        co_await FallingEdge{dut.clk};
        co_await Delay{1_ps};
        if (dut.out_valid.get() == 0 || dut.out_ready.get() == 0) continue;

        observed_words.put_nowait(dut.out_data.get());
        ++observed;
    }
}

Task<void> scoreboard(TestContext& test,
                      Channel<uint32_t>& expected_words,
                      Channel<uint32_t>& observed_words) {
    for (uint32_t index = 0; index < kWordCount; ++index) {
        const uint32_t expected = co_await expected_words.get();
        const uint32_t actual = co_await observed_words.get();
        test.expect_eq("FIFO payload", actual, expected);
    }
}

Task<void> fifo_test(Dut dut, TestContext& test) {
    dut.clk.set(0);
    test.start_clock(dut.clk, 10_ns);

    Event reset_done;
    Channel<uint32_t> expected_words;
    Channel<uint32_t> observed_words;
    uint32_t input_stalls = 0;

    co_await Join{reset_dut(dut, reset_done),
                  input_driver(dut, reset_done, expected_words, input_stalls),
                  output_ready_driver(dut, reset_done),
                  output_monitor(dut, reset_done, observed_words),
                  scoreboard(test, expected_words, observed_words)};

    co_await Delay{1_ps};
    test.expect_eq("FIFO drained", dut.out_valid.get(), 0u);
    test.expect_eq("FIFO accepts after drain", dut.in_ready.get(), 1u);
    test.expect_eq("input backpressure observed", input_stalls != 0, true);
}

CPPTB_REGISTER_TEST(fifo_test);

}  // namespace
}  // namespace cpptb::examples::fifo_scoreboard
