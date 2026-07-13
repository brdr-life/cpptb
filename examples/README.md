# Examples

Follow the examples as a learning path, or jump to the one closest to your
DUT. Every directory contains a C++ DPI testbench and a pure-SystemVerilog
twin with the same stimulus, checks, and primary-clock cycle count.

| Example | Concepts introduced | Run |
|---|---|---|
| [`counter`](counter/) | Generated clock, explicit reset, `get()`/`set()`, edge waits, and post-edge sampling | `make cpp-dpi-counter-run` |
| [`timer_only`](timer_only/) | Clockless `Delay` scheduling and concurrent timer processes | `make cpp-dpi-timer-only-run` |
| [`fifo_scoreboard`](fifo_scoreboard/) | Ready/valid traffic, `Event`, `Channel`, driver, monitor, scoreboard, and `Join` | `make cpp-dpi-fifo-scoreboard-run` |
| [`multiclock`](multiclock/) | Independent clocks, producer/consumer traffic, `First`, and explicit settle delays | `make cpp-dpi-multiclock-run` |
| [`apb_regfile`](apb_regfile/) | Reusable protocol transactions and typed `Task<uint32_t>` results | `make cpp-dpi-apb-regfile-run` |
| [`watchdog_timeout`](watchdog_timeout/) | Trigger/task timeouts, expected stalls, process handles, and cancellation | `make cpp-dpi-watchdog-timeout-run` |

## Which example should I copy?

- Start from `counter` for a small single-clock DUT.
- Start from `timer_only` for combinational or clockless models that settle
  after explicit delays.
- Start from `fifo_scoreboard` for streaming interfaces and concurrent
  verification components.
- Start from `multiclock` for independent clock domains.
- Start from `apb_regfile` when a bus-functional model should hide pin-level
  protocol phases from the test sequence.
- Start from `watchdog_timeout` when operations may hang, time out, or need
  cancellation.

Run all C++/SV pairs through the normal test target:

```sh
make examples-test
```

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

The examples use the same broad verification patterns documented by cocotb:
typed DUT access, coroutines that yield on simulator events, concurrent tasks,
and explicit timeout races. See cocotb's official guides to
[writing testbenches](https://docs.cocotb.org/en/stable/writing_testbenches.html),
[coroutines and tasks](https://docs.cocotb.org/en/stable/coroutines.html), and
[simulation timing](https://docs.cocotb.org/en/stable/timing_model.html), plus
its [example projects](https://github.com/cocotb/cocotb/tree/master/examples).
