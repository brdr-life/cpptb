# Examples

The examples are ordered from a first testbench to more involved
multi-process testbenches:

| Example | Demonstrates | Run |
|---|---|---|
| [`counter`](counter/) | Generated clock, reset, signal reads and writes, edge waits, and post-edge sampling | `make cpp-dpi-counter-run` |
| [`timer_only`](timer_only/) | Clockless absolute delays and concurrent timer processes | `make cpp-dpi-timer-only-run` |
| [`multiclock`](multiclock/) | Independent clocks, reset, producer/consumer traffic, `First`, and explicit settle delays | `make cpp-dpi-multiclock-run` |

Each directory keeps the same recognizable shape:

```text
example/
├── README.md
├── design.dpi.json
├── design.sv
├── testbench.cpp
├── framework.cpp/.hpp
├── dpi_transport.cpp
├── generated/
└── systemverilog/
```

`testbench.cpp` is the primary user-facing file. The framework and transport
files connect the generated DUT type to the reusable runtime. The
`systemverilog/` testbench executes the equivalent sequence for semantic and
performance comparisons.
