#include "examples/fifo_scoreboard/framework.hpp"

namespace cpptb::examples::fifo_scoreboard {
namespace {

using coro::Channel;
using coro::Delay;
using coro::Event;
using coro::FallingEdge;
using coro::Join;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

uint32_t next_word(uint32_t& state) {
    state = state * 1'664'525u + 1'013'904'223u;
    return state;
}

Task<void> reset_dut(FifoScoreboardTb tb, Event& reset_done) {
    tb.dut.rst_n.set(0);
    tb.dut.in.valid.set(0);
    tb.dut.in.data.set(0);
    tb.dut.out.ready.set(0);

    co_await clock_cycles(tb.dut.clk, 2);
    co_await FallingEdge{tb.dut.clk};
    tb.dut.rst_n.set(1);
    reset_done.set();
}

Task<void> input_driver(FifoScoreboardTb tb, Event& reset_done,
                        Channel<uint32_t>& expected_words,
                        uint32_t& input_stalls) {
    co_await reset_done;

    uint32_t state = 0x3141'5926u;
    for (uint32_t index = 0; index < tb.iterations(); ++index) {
        const uint32_t word = next_word(state);
        while (true) {
            co_await FallingEdge{tb.dut.clk};
            tb.dut.in.data.set(word);
            tb.dut.in.valid.set(1);
            co_await Delay{1_ps};

            if (tb.dut.in.ready.get() == 0) {
                ++input_stalls;
                continue;
            }
            expected_words.put_nowait(word);
            co_await RisingEdge{tb.dut.clk};
            co_await Delay{1_ps};
            tb.dut.in.valid.set(0);
            break;
        }
    }
}

Task<void> output_ready_driver(FifoScoreboardTb tb, Event& reset_done) {
    co_await reset_done;

    uint32_t cycle = 0;
    uint32_t accepted = 0;
    while (accepted < tb.iterations()) {
        co_await FallingEdge{tb.dut.clk};
        const uint32_t ready =
            (cycle % 5u == 1u || cycle % 5u == 2u) ? 0u : 1u;
        tb.dut.out.ready.set(ready);
        co_await Delay{2_ps};

        if (ready != 0 && tb.dut.out.valid.get() != 0) ++accepted;
        ++cycle;
    }

    co_await RisingEdge{tb.dut.clk};
    co_await Delay{1_ps};
    tb.dut.out.ready.set(0);
}

Task<void> output_monitor(FifoScoreboardTb tb, Event& reset_done,
                          Channel<uint32_t>& observed_words) {
    co_await reset_done;

    uint32_t observed = 0;
    while (observed < tb.iterations()) {
        co_await FallingEdge{tb.dut.clk};
        co_await Delay{1_ps};
        if (tb.dut.out.valid.get() == 0 || tb.dut.out.ready.get() == 0) {
            continue;
        }

        observed_words.put_nowait(tb.dut.out.data.get());
        ++observed;
    }
}

Task<void> scoreboard(FifoScoreboardTb tb,
                      Channel<uint32_t>& expected_words,
                      Channel<uint32_t>& observed_words) {
    for (uint32_t index = 0; index < tb.iterations(); ++index) {
        const uint32_t expected = co_await expected_words.get();
        const uint32_t actual = co_await observed_words.get();
        tb.expect_eq("FIFO payload", actual, expected);
    }
}

Task<void> fifo_test(FifoScoreboardTb tb) {
    Event reset_done;
    Channel<uint32_t> expected_words;
    Channel<uint32_t> observed_words;
    uint32_t input_stalls = 0;

    co_await Join{reset_dut(tb, reset_done),
                  input_driver(tb, reset_done, expected_words, input_stalls),
                  output_ready_driver(tb, reset_done),
                  output_monitor(tb, reset_done, observed_words),
                  scoreboard(tb, expected_words, observed_words)};

    co_await Delay{1_ps};
    tb.expect_eq("FIFO drained", tb.dut.out.valid.get(), 0);
    tb.expect_eq("FIFO accepts after drain", tb.dut.in.ready.get(), 1);
    if (tb.iterations() > 4) {
        tb.expect_eq("input backpressure observed", input_stalls != 0, 1);
    }
}

}  // namespace

void register_user_testbench(FifoScoreboardTb& tb) {
    tb.run(fifo_test(tb));
}

}  // namespace cpptb::examples::fifo_scoreboard
