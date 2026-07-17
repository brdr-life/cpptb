# Component FIFO

This example verifies the same kind of ready/valid FIFO as the direct
[FIFO scoreboard](fifo-scoreboard.md), but keeps connectivity separate from
storage and checking. The test uses typed endpoints, zero-time analysis
fan-out, an in-order scoreboard, and reusable ready/valid helpers. It is a
separate example so the primitive and component styles remain easy to compare.

The framework does not start a clock, reset the DUT, spawn a process, or insert
a sampling delay on behalf of the test.

## Connect the components

The root test owns every component and connection. The connection objects keep
the subscriptions active, while `Join` makes all concurrent work explicit:

```cpp
Task<void> component_fifo_test(Dut dut, TestContext& test) {
    dut.clk.set(0);
    test.start_clock(dut.clk, 10_ns);

    Event reset_done;
    AnalysisPort<uint32_t> expected;
    AnalysisPort<uint32_t> observed;
    InOrderScoreboard<uint32_t> scoreboard{test, "FIFO payload"};
    AnalysisBuffer<uint32_t> audit{4, AnalysisOverflowPolicy::Error};

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
```

## Drive and observe

The driver writes expected transactions before translating each one into
explicit ready/valid pin activity. The monitor publishes only accepted output
transfers. Both helpers receive the `1_ps` sample delay from authored test code.

<div class="cpptb-code-tabs" data-tabs="2" data-tab-group="component-fifo" data-tab-label="Component FIFO implementation"></div>

<div class="cpptb-code-tab-label">cpptb (C++ DPI)</div>

```cpp
Task<void> input_sequence(Dut dut, Event& reset_done,
                          AnalysisPort<uint32_t>& expected,
                          uint32_t& input_stalls) {
    co_await reset_done;
    ReadyValidDriver driver{dut.clk, dut.in_valid, dut.in_ready, dut.in_data,
                            1_ps};

    uint32_t state = 0x3141'5926u;
    for (uint32_t index = 0; index < kWordCount; ++index) {
        const uint32_t word = next_word(state);
        expected.write(word);
        input_stalls += co_await driver.send(word);
    }
}

Task<void> output_monitor(Dut dut, Event& reset_done,
                          AnalysisPort<uint32_t>& observed) {
    co_await reset_done;
    ReadyValidMonitor monitor{
        dut.clk, dut.out_valid, dut.out_ready, dut.out_data,
        ReadyValidSampleEdge::Falling, 1_ps};
    co_await monitor.run(observed, kWordCount);
}
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
task automatic input_sequence();
  logic [31:0] state;
  logic [31:0] word;
  @reset_done;
  state = 32'h3141_5926;

  for (int unsigned index = 0; index < kWordCount; index++) begin
    state = state * 32'd1664525 + 32'd1013904223;
    word = state;
    expected_words.push_back(word);
    forever begin
      @(negedge clk);
      in_data = word;
      in_valid = 1'b1;
      @(posedge clk);
      #1ps;
      if (!in_ready) begin
        input_stalls++;
        continue;
      end
      @(negedge clk);
      in_valid = 1'b0;
      break;
    end
  end
endtask

task automatic output_monitor();
  logic [31:0] expected;
  logic [31:0] actual;
  int unsigned observed;
  @reset_done;
  observed = 0;

  while (observed < kWordCount) begin
    @(negedge clk);
    #1ps;
    if (!out_valid || !out_ready) continue;

    observed_words.push_back(out_data);
    expected = expected_words.pop_front();
    actual = observed_words.pop_front();
    expect_eq("FIFO payload", actual, expected);
    scoreboard_comparisons++;
    if (!audit_words.try_put(out_data)) begin
      $fatal(1, "component FIFO audit buffer is full");
    end
    observed++;
  end
endtask
```

The repository pair executes the same `24` transactions, `56` checks, and
`124` primary-clock cycles:

```sh
make feature-test FEATURE=dpi_component_fifo
```

The complete authored files are `examples/component_fifo/testbench.cpp` and
`examples/component_fifo/systemverilog/component_fifo_sv_tb.sv`. Run them with
`make cpp-dpi-component-fifo-run` or
`make cpp-dpi-component-fifo-sv-run` respectively.
