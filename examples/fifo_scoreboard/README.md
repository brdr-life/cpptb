# FIFO scoreboard example

This example verifies a ready/valid FIFO with independent reset, input-driver,
backpressure, output-monitor, and scoreboard processes. The C++ testbench uses
`Event` for reset completion and `Queue<uint32_t>` for the expected and
observed transaction streams. The default run also checks that output
backpressure fills the FIFO far enough to stall the input driver.

Run the C++ DPI testbench and its exact SystemVerilog twin:

```sh
make cpp-dpi-fifo-scoreboard-run
make cpp-dpi-fifo-scoreboard-sv-run
make feature-test FEATURE=dpi_fifo_scoreboard
```

The two testbenches use the same deterministic data generator, ready pattern,
sampling points, checks, and primary-clock cycle count. Both use the fixed
semantic workload `kWordCount = 24`; this equivalence example has no benchmark
iteration control. Build it with:

```sh
uv run --frozen cpptb build
```

The C++ testbench initializes `dut.clk` and starts its 10 ns period.

| cpptb | SystemVerilog |
|---|---|
| `Task<void>` process | `task automatic` process |
| `Event` | `event` |
| `Queue<uint32_t>` | typed `mailbox` |
| `Join{...}` | `fork ... join` |
| `co_await FallingEdge{clk}` | `@(negedge clk)` |
| `co_await Delay{1_ps}` | `#1ps` |
