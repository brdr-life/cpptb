# Multi-clock DPI trigger prototype

This example validates the generated multi-clock wrapper and the coroutine
trigger model against a dual-clock mailbox. The user-facing testbench is
`testbench.cpp`; generated bindings and simulator scheduling remain outside it.

The testbench exercises concurrent producer and consumer tasks, `Join`, two
independently phased input clocks, a DUT-produced output clock, exact
simulation-time `Delay`, `First`, and explicit post-edge settling delays.

```sh
make cpp-dpi-multiclock-run
make cpp-dpi-multiclock-sv-run
```

Both peers run the fixed `kTransferCount = 16` workload. The C++ executable
reports one module-specific `CPP_DPI_*_RESULT` marker and the pure-SV peer
reports `PURE_SV_MULTICLOCK_RESULT`; both report `iterations=1` because this is
an equivalence example, not a scalable benchmark.

Its project build needs no timing configuration:

```sh
uv run --frozen cpptb build
```

The C++ testbench owns the clocks:

```cpp
dut.write_clk.set(0);
dut.read_clk.set(0);
test.start_clock(dut.write_clk, 4_ns);
test.start_clock(dut.read_clk, 6_ns, 1_ns);

co_await RisingEdge{dut.output_clk};
```

The feature-regression adapter classifies this example as `equivalence_only`.
It passes only when all four fields match exactly and `failures=0`; elapsed
runtime is neither compared nor normalized. Run it through the registry with:

```sh
make feature-test FEATURE=dpi_multiclock
make feature-benchmark FEATURE=dpi_multiclock
```
