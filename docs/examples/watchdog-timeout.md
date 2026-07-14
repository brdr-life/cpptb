# Watchdogs and timeouts

This bench treats completed responses and intentional timeouts as separate
checked outcomes. Generation needs no clock or edge annotations:

```sh
uv run --frozen cpptb-codegen \
  examples/watchdog_timeout/stalling_responder.sv
```

The test starts `dut.clk` at 10 ns and awaits the DUT-produced
`dut.response_valid` edge directly.

The registered test runs `kTransactionCount = 8` normal operations, one raw
edge timeout, one expected task timeout, and a cancellable monitor. The pure SV
peer mirrors the same count and deadlines.

```cpp
auto response = co_await with_timeout(
    transaction(dut, word, 3, false), 200_ns);
test.expect_eq("transaction completed", response.has_value(), true);

auto stalled = co_await with_timeout(
    transaction(dut, word, 2, true), 60_ns);
test.expect_eq("stalled transaction timed out", stalled.timed_out(), true);

auto monitor = test.spawn(dormant_monitor(dut));
co_await Delay{3_ns};
monitor.cancel();
co_await monitor;
```

`spawn()` returns a `Process` for joining or cancellation.
`spawn_detached()` is available for roots that need no handle, and
`TestContext::now()` reports the current scheduler time. The public API has no
implicit global context.
