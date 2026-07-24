# APB transaction-recording example

This example records 256 completed APB operations from a passive monitor. The
same typed observation stream feeds an in-order scoreboard and an in-memory
transaction trace, so recording does not duplicate protocol decoding.

```cpp
TransactionRecorder recorder;
InMemoryTransactionSink trace;
auto trace_sink = recorder.connect(trace);
auto& stream = recorder.stream<Transaction>("apb0.observed");

auto checking = monitor.observed().connect(scoreboard.actual());
auto recording = monitor.observed().connect(stream);

co_await Join{trace_sequence(master, test, expected),
              monitor.run(kTransactionCount)};
```

The DUT is a 64-word APB RAM. The deterministic sequence performs 128 writes
and reads each value back, including 128 single-cycle wait states, while the
recorder retains transaction fields, begin/end simulation times, completion
disposition, and per-stream sequence.

Run the C++ testbench and its pure-SystemVerilog peer:

```sh
make cpp-dpi-apb-trace-run
make cpp-dpi-apb-trace-sv-run
```
