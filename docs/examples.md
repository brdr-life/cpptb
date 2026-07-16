# Examples

Every example is runnable and contains a C++ DPI testbench plus a pure
SystemVerilog twin with the same stimulus, checks, and primary-clock cycle
count. The pages below show the user-facing C++ code that constructs each
bench; generated bindings, DPI transport, and result-reporting glue stay out
of the way.

| Example | Start here for | Source shown |
|---|---|---|
| [Counter](examples/counter.md) | A small, single-clock DUT | Complete testbench |
| [Clockless timers](examples/timer-only.md) | Combinational or clockless models | Complete testbench |
| [FIFO scoreboard](examples/fifo-scoreboard.md) | Streaming drivers, monitors, and scoreboards | Key processes and composition |
| [Multiple clocks](examples/multiclock.md) | Independent clock domains | Producer, consumer, and trigger probe |
| [APB register file](examples/apb-regfile.md) | Reusable protocol transactions | Bus-functional methods and sequence |
| [Watchdogs and timeouts](examples/watchdog-timeout.md) | Operations that may stall or need cancellation | Transaction, timeout, and process lifecycle |
| [Fault injection](examples/fault-injection.md) | Internal access and controlled fault injection | Deposit, force, release, and explicit settling |
| [Rich data](examples/rich-data.md) | Wide, fixed-point, array, struct, and enum ports | Typed construction and checking |
| [Heavy benchmarks](examples/heavy-benchmarks.md) | Computationally substantial four-mode comparisons | FIR, packet CRC32, and matrix accelerator sequences |
| [Open-source cores](examples/open-source-cores.md) | Real CPU, crypto, and network RTL comparisons | Firmware, register programming, and AXI-stream sequences |

## Constructing a bench

A cpptb project keeps the hand-written DUT and testbench separate from
framework-owned files:

```text
example/
|-- README.md
|-- design.sv
`-- testbench.cpp

build/cpptb/design/
|-- generated/
|-- metadata/
|-- obj/
`-- results/
```

`testbench.cpp` is the primary user-facing file. `cpptb build` infers target
metadata from SystemVerilog and writes the typed DUT, wrapper, binding, and
transport adapter under the ignored build directory. The repository examples
add a `systemverilog/` comparison bench for semantic and performance
regression; user projects do not need one.

### 1. Build typed access

Run the project command after changing RTL or C++. It derives interface
metadata, discovers clock and hierarchy usage, and compiles the simulator:

```sh
cpptb build
```

It infers the unambiguous `counter` top and emits the target-scoped binding,
stable `dut.hpp`, SystemVerilog DPI wrapper, and C++ transport adapter under
`build/cpptb/counter/generated/`. Use `--top` when sources contain more than
one top-level module.

### 2. Write and register the sequence

A testbench is one or more `Task<void>` coroutines. Signal access is explicit,
and every point where simulator time advances is visible as `co_await`:

```cpp
Task<void> counter_sequence(Dut dut, TestContext& test) {
    dut.clk.set(0);
    test.start_clock(dut.clk, 10_ns);

    dut.rst_n.set(0);
    dut.enable.set(0);

    co_await clock_cycles(dut.clk, 2);
    co_await FallingEdge{dut.clk};
    dut.rst_n.set(1);
    dut.enable.set(1);

    co_await RisingEdge{dut.clk};
    co_await Delay{1_ps};
    test.expect_eq("enabled count", dut.count.get(), 1);
}

CPPTB_REGISTER_TEST(counter_sequence);
```

The complete version is on the [counter page](examples/counter.md).

### Internal hierarchy quick start

The [fault-injection example](examples/fault-injection.md) is the smallest
complete bench centered on hierarchical access. It uses the same `Dut`,
`TestContext`, coroutine, clock, and registration model shown above, then
accesses inferred internal objects without a probe list:

```cpp
dut.resolved_value.force(0xa5);
test.expect_eq("force readback", dut.resolved_value.get(), 0xa5u);
dut.resolved_value.release();

dut.memory.at(2).deposit(0xbeef);
test.expect_eq("memory backdoor", dut.memory.at(2).get(), 0xbeefu);
```

The full page explains immediate versus settled observations, variable and net
release semantics, memory elements, and the matching pure-SystemVerilog code.

### 3. Build and compare

Run either implementation directly, or run the paired feature check:

```sh
make cpp-dpi-counter-run
make cpp-dpi-counter-sv-run
make feature-test FEATURE=dpi_counter
```

Run every documented C++/SV pair with:

```sh
make examples-test
```

The [testbench authoring guide](testbench-authoring.md) is the API reference
for the primitives composed by these examples.
