#include <cstdint>

#include "cpptb/cpptb.hpp"
#include "dut.hpp"

namespace cpptb::examples::fifo_scoreboard {
namespace {

using cpptb::Dut;
using coro::Event;
using coro::Join;
using coro::Queue;
using coro::ReadOnly;
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
    dut.rst_n.set(1);
    reset_done.set();
}

Task<void> input_driver(Dut dut, Event& reset_done,
                        Queue<uint32_t>& expected_words,
                        uint32_t& input_stalls) {
    co_await reset_done;

    // The cocotb driver shape: RisingEdge resumes before the design
    // evaluates that edge, so a get() here reads the value the DUT is
    // about to sample, and a set() applies after this edge's updates --
    // in time for the next one.
    uint32_t state = 0x3141'5926u;
    co_await RisingEdge{dut.clk};
    for (uint32_t index = 0; index < kWordCount; ++index) {
        const uint32_t word = next_word(state);
        dut.in_data.set(word);
        dut.in_valid.set(1);
        while (true) {
            co_await RisingEdge{dut.clk};
            if (dut.in_ready.get() != 0) break;
            ++input_stalls;
        }
        expected_words.put_nowait(word);
    }
    dut.in_valid.set(0);
}

Task<void> output_ready_driver(Dut dut, Event& reset_done) {
    co_await reset_done;

    uint32_t cycle = 0;
    uint32_t accepted = 0;
    while (accepted < kWordCount) {
        co_await RisingEdge{dut.clk};
        if (dut.out_ready.get() != 0 && dut.out_valid.get() != 0) ++accepted;
        const uint32_t ready =
            (cycle % 5u == 1u || cycle % 5u == 2u) ? 0u : 1u;
        dut.out_ready.set(ready);
        ++cycle;
    }
    dut.out_ready.set(0);
}

Task<void> output_monitor(Dut dut, Event& reset_done,
                          Queue<uint32_t>& observed_words) {
    co_await reset_done;

    uint32_t observed = 0;
    while (observed < kWordCount) {
        co_await RisingEdge{dut.clk};
        if (dut.out_valid.get() == 0 || dut.out_ready.get() == 0) continue;

        observed_words.put_nowait(dut.out_data.get());
        ++observed;
    }
}

Task<void> scoreboard(TestContext& test,
                      Queue<uint32_t>& expected_words,
                      Queue<uint32_t>& observed_words) {
    for (uint32_t index = 0; index < kWordCount; ++index) {
        const uint32_t expected = co_await expected_words.get();
        const uint32_t actual = co_await observed_words.get();
        test.expect_eq("FIFO payload", actual, expected);
    }
}

Task<void> fifo_test(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
    test.start_clock(dut.clk, 10_ns);

    Event reset_done;
    Queue<uint32_t> expected_words;
    Queue<uint32_t> observed_words;
    uint32_t input_stalls = 0;

    co_await Join{reset_dut(dut, reset_done),
                  input_driver(dut, reset_done, expected_words, input_stalls),
                  output_ready_driver(dut, reset_done),
                  output_monitor(dut, reset_done, observed_words),
                  scoreboard(test, expected_words, observed_words)};

    co_await ReadOnly{};
    test.expect_eq("FIFO drained", dut.out_valid.get(), 0u);
    test.expect_eq("FIFO accepts after drain", dut.in_ready.get(), 1u);
    test.expect_eq("input backpressure observed", input_stalls != 0, true);
}

CPPTB_REGISTER_TEST(fifo_test);

}  // namespace
}  // namespace cpptb::examples::fifo_scoreboard
