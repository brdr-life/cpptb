# Examples

Follow the examples as a learning path, or jump to the one closest to your
DUT. Every directory contains a C++ DPI testbench and a pure-SystemVerilog
twin with the same stimulus, checks, and primary-clock cycle count.

| Example | Concepts introduced | Run |
|---|---|---|
| [`counter`](counter/) | C++-owned clock, explicit reset, `get()`/`set()`, edge waits, and post-edge sampling | `make cpp-dpi-counter-run` |
| [`timer_only`](timer_only/) | Clockless `Delay` scheduling and concurrent timer processes | `make cpp-dpi-timer-only-run` |
| [`fifo_scoreboard`](fifo_scoreboard/) | Ready/valid traffic, `Event`, `Queue`, driver, monitor, scoreboard, and `Join` | `make cpp-dpi-fifo-scoreboard-run` |
| [`component_fifo`](component_fifo/) | Typed ports, analysis fan-out, buffered observation, reusable scoreboard, and ready/valid components | `make cpp-dpi-component-fifo-run` |
| [`multiclock`](multiclock/) | Independent input clocks, a DUT output clock, producer/consumer traffic, and `First` | `make cpp-dpi-multiclock-run` |
| [`apb_regfile`](apb_regfile/) | `cpptb_vc` APB master, monitor, checker, scoreboard, and coverage | `make cpp-dpi-apb-regfile-run` |
| [`watchdog_timeout`](watchdog_timeout/) | Trigger/task timeouts, expected stalls, process handles, and cancellation | `make cpp-dpi-watchdog-timeout-run` |
| [`fault_injection`](fault_injection/) | Internal `get()`/`deposit()`/`force()`/`release()` on nets, variables, and memory | `make cpp-dpi-fault-injection-run` |
| [`rich_data`](rich_data/) | Wide values and slices, fixed point, arrays, packed structs, and enums | `make cpp-dpi-rich-data-run` |
| [`interfaces`](interfaces/) | Parameterized interfaces, modports, interface arrays, independent clocks, and inouts | `make cpp-dpi-interfaces-run` |

## Which example should I copy?

- Start from `counter` for a small single-clock DUT.
- Start from `timer_only` for combinational or clockless models that settle
  after explicit delays.
- Start from `fifo_scoreboard` for streaming interfaces and concurrent
  verification written from primitives.
- Start from `component_fifo` when drivers, monitors, and scoreboards should be
  connected through reusable transaction endpoints.
- Start from `apb_regfile` for protocol-neutral memory-mapped sequences and a
  complete active/passive bus component composition.
- Start from `multiclock` for independent clock domains.
- Start from `apb_regfile` when a bus-functional model should hide pin-level
  protocol phases from the test sequence.
- Start from `watchdog_timeout` when operations may hang, time out, or need
  cancellation.
- Start from `fault_injection` for explicit internal access and fault
  injection.
- Start from `rich_data` for typed interfaces beyond scalar ports.
- Start from `interfaces` for SystemVerilog interfaces, modports, or `inout`
  pins.

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
└── systemverilog/
```

`testbench.cpp` is the user-facing file. It includes the stable generated
`dut.hpp`, accepts `cpptb::Dut` and library-owned `TestContext&`, then uses
`CPPTB_REGISTER_TEST` for each root test. Generated bindings and all compiler
output live under `build/cpptb/<example>/`; the source directories remain
author-owned. The `systemverilog/` testbench is repository validation material
that executes the same fixed semantic workload for exact comparison. Users do
not need it in their own projects.

The root Makefile targets are developer aliases over `cpptb build` and
`cpptb test`; generation, discovery, and Verilator compilation live inside the
CLI. Clock roles, periods, and phases remain in `testbench.cpp`: initialize an
input clock, then call `test.start_clock(...)` before the first await.
DUT-produced clocks are ordinary observed signals.

The examples use the same broad verification patterns documented by cocotb:
typed DUT access, coroutines that yield on simulator events, concurrent tasks,
and explicit timeout races. See cocotb's official guides to
[writing testbenches](https://docs.cocotb.org/en/stable/writing_testbenches.html),
[coroutines and tasks](https://docs.cocotb.org/en/stable/coroutines.html), and
[simulation timing](https://docs.cocotb.org/en/stable/timing_model.html), plus
its [example projects](https://github.com/cocotb/cocotb/tree/master/examples).
