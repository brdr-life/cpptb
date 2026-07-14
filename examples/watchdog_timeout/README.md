# Watchdog and timeout example

This example treats both successful responses and intentional timeouts as
checked outcomes. It demonstrates `with_timeout()` on a `Task<uint32_t>` and
on a `RisingEdge`, then explicitly cancels a dormant monitor through its
`Process` handle.

```cpp
auto response = co_await with_timeout(transaction(dut, word, 3, false),
                                      200_ns);
test.expect_eq("transaction completed", response.has_value(), true);

auto stalled = co_await with_timeout(transaction(dut, word, 2, true), 60_ns);
test.expect_eq("stalled transaction timed out", stalled.timed_out(), true);
```

Run the C++ DPI testbench and its exact SystemVerilog twin:

```sh
make cpp-dpi-watchdog-timeout-run
make cpp-dpi-watchdog-timeout-sv-run
make feature-test FEATURE=dpi_watchdog_timeout
```

Every response is separated from its deadline, so the result never depends on
same-timestamp race ordering. Both peers use the fixed semantic workload
`kTransactionCount = 8`. The source generator needs no clock or edge
annotations:

```sh
uv run --frozen cpptb-codegen stalling_responder.sv
```

The testbench starts `dut.clk` and directly awaits
`RisingEdge{dut.response_valid}`.

| cpptb | SystemVerilog |
|---|---|
| `with_timeout(task, 200_ns)` | response/deadline `fork ... join_any` race |
| `TimeoutResult<T>` | completion flag plus result |
| `with_timeout(RisingEdge{...}, 100_ns)` | edge/deadline race |
| `Process::cancel()` | disable a named monitor process |
