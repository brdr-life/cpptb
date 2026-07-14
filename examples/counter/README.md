# Counter

This is the smallest end-to-end cpptb project. The user-facing sequence is
[`testbench.cpp`](testbench.cpp): an explicitly registered coroutine receives
the generated `Dut dut` and library-owned `TestContext`, drives reset and
enable, waits for clock edges, and checks the count.

```sh
make cpp-dpi-counter-run
make cpp-dpi-counter-sv-run
```

Both implementations run the same fixed `kCountCycles` workload and report the
same checks, primary-clock cycles, and failures. Regenerate the typed DUT, DPI
wrapper, and transport adapter directly from the source with:

```sh
uv run --frozen cpptb-codegen examples/counter/counter.sv
```

The testbench owns timing with `test.start_clock(dut.clk, 10_ns)`.
