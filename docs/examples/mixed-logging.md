# Mixed-language logging

This runnable example combines C++ testbench messages with diagnostics authored
inside the RTL. Both enter the same sink and optional `LogHistory`, receive one
per-test sequence, and retain their source language and source location.

```sh
make cpp-dpi-mixed-logging-run
make cpp-dpi-mixed-logging-output-test
make cpp-dpi-mixed-logging-sv-run
```

`cpptb build` adds the SV package, DPI bridge, include path, and
`CPPTB_ENABLE_SV_LOGGING` define automatically. Users author only the testbench
and the guarded logging calls.

<div class="cpptb-code-tabs" data-tabs="2" data-tab-group="mixed-logging-authored" data-tab-label="Mixed-language authored code"></div>

<div class="cpptb-code-tab-label">C++ testbench</div>

```cpp
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
```

<div class="cpptb-code-tab-label">Instrumented SystemVerilog</div>

```systemverilog
`ifdef CPPTB_ENABLE_SV_LOGGING
`include "cpptb/sv/cpptb_log.svh"
`endif

always_ff @(posedge clk or negedge rst_n) begin
  if (!rst_n) begin
    accepted_count <= '0;
  end else if (valid) begin
    accepted_count <= accepted_count + 1'b1;
`ifdef CPPTB_ENABLE_SV_LOGGING
    `cpptb_info($sformatf("accepted data=0x%08x", data), "request_monitor")
`endif
  end
end
```

The output checker asserts that all five records have contiguous sequence
numbers, the two RTL records have `SystemVerilog` origin plus file, line, scope,
and hierarchy metadata, and the three C++ records retain C++ source locations.
The standalone pure-SV twin drives the same two transfers and checks the same
counter results.

See [Structured logging](../logging.md#systemverilog-messages) for filtering,
origin semantics, custom sinks, and the distinction from ordinary `$display`.
