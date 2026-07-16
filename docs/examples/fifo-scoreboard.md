# FIFO scoreboard

This ready/valid example separates reset, input drive, output readiness,
monitoring, and checking into ordinary `Task<void>` coroutines. `Event`
publishes reset completion and two `Queue<uint32_t>` objects connect the
driver and monitor to the scoreboard.

```cpp
constexpr uint32_t kWordCount = 24;

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
    dut.clk.set(0);
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
}

CPPTB_REGISTER_TEST(fifo_test);
```

Build and run it with
`cpptb test --project examples/fifo_scoreboard --build-dir build`.
Every drive, edge wait, and settle delay remains explicit. The pure SV peer
uses the same `kWordCount = 24`, data generator, and ready pattern.
