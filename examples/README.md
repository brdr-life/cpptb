# Examples

Follow the examples as a learning path, or jump to the one closest to your
DUT. Every directory contains a C++ DPI testbench and a pure-SystemVerilog
twin with the same stimulus, checks, and primary-clock cycle count.

| Example | Concepts introduced | Run |
|---|---|---|
| [`counter`](counter/) | C++-owned clock, explicit reset, `get()`/`set()`, edge waits, and post-edge sampling | `make cpp-dpi-counter-run` |
| [`timer_only`](timer_only/) | Clockless `Delay` scheduling and concurrent timer processes | `make cpp-dpi-timer-only-run` |
| [`fifo_scoreboard`](fifo_scoreboard/) | Ready/valid traffic, `Event`, `Channel`, driver, monitor, scoreboard, and `Join` | `make cpp-dpi-fifo-scoreboard-run` |
| [`multiclock`](multiclock/) | Independent input clocks, a DUT output clock, producer/consumer traffic, and `First` | `make cpp-dpi-multiclock-run` |
| [`apb_regfile`](apb_regfile/) | Reusable protocol transactions and typed `Task<uint32_t>` results | `make cpp-dpi-apb-regfile-run` |
| [`watchdog_timeout`](watchdog_timeout/) | Trigger/task timeouts, expected stalls, process handles, and cancellation | `make cpp-dpi-watchdog-timeout-run` |
| [`fault_injection`](fault_injection/) | Internal `get()`/`deposit()`/`force()`/`release()` on nets, variables, and memory | `make cpp-dpi-fault-injection-run` |
| [`rich_data`](rich_data/) | Wide values and slices, fixed point, arrays, packed structs, and enums | `make cpp-dpi-rich-data-run` |

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
- Start from `fault_injection` for explicit internal access and fault
  injection.
- Start from `rich_data` for typed interfaces beyond scalar ports.

Run all C++/SV pairs through the normal test target:

```sh
make examples-test
```

Each directory keeps the same recognizable shape:

```text
example/
├── README.md
├── design.sv
├── testbench.cpp
├── generated/
└── systemverilog/
```

`testbench.cpp` is the user-facing file. It accepts the target-specific
generated `Dut` and library-owned `TestContext&`, then uses
`CPPTB_REGISTER_TEST` once. The generator owns everything under `generated/`,
including the DPI transport. The `systemverilog/` testbench executes the same
fixed semantic workload for exact comparison.

The Makefile invokes `cpptb-codegen` directly on each RTL source. Clock roles,
periods, and phases live in `testbench.cpp`: initialize an input clock, then
call `test.start_clock(...)` before the first await. DUT-produced clocks are
ordinary observed signals.

The examples use the same broad verification patterns documented by cocotb:
typed DUT access, coroutines that yield on simulator events, concurrent tasks,
and explicit timeout races. See cocotb's official guides to
[writing testbenches](https://docs.cocotb.org/en/stable/writing_testbenches.html),
[coroutines and tasks](https://docs.cocotb.org/en/stable/coroutines.html), and
[simulation timing](https://docs.cocotb.org/en/stable/timing_model.html), plus
its [example projects](https://github.com/cocotb/cocotb/tree/master/examples).
