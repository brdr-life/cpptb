# A C++ VPI Testbench Prototype

This prototype mirrors the MojoTB counter experiment, but keeps the testbench
runtime and test processes in C++.

The goal is to evaluate the same cocotb-like scheduler shape with direct,
standard VPI hooks instead of a Mojo shared-library boundary:

```text
Verilator model
  -> standard VPI handles and callbacks
  -> C++ process scheduler
  -> C++ reset/driver/monitor processes
```

## Layout

- `experiments/cpp_vpi/counter/runtime.hpp`: primitive signal, trigger, wait request, scheduler, and
  DUT handle wrappers.
- `experiments/cpp_vpi/counter/counter_test.cpp`: reset driver, enable driver, count monitor,
  and final check.
- `experiments/cpp_vpi/counter/verilator_vpi_host.cpp`: Verilator host and VPI callback scheduler.
- `experiments/cpp_vpi/counter/vpi_counter.sv`: local copy of the counter RTL for this
  experiment.

The C++ runtime uses these VPI calls directly:

- `vpi_handle_by_name`
- `vpi_get_value`
- `vpi_put_value`
- `vpi_register_cb`
- `cbValueChange`

## Process Model

The C++ host stores a `ProcessState` for each active process:

```cpp
struct ProcessState {
    uint32_t id;
    WaitRequest wait;
    bool done;
};
```

Each process returns a `WaitRequest`. When a watched signal changes, the host
resumes every process waiting on that signal:

```cpp
if (process.wait.kind == kRequestRisingEdge
    && process.wait.signal_id == watch->signal_id) {
    apply_wait_request(process, process_resume(...));
}
```

This is the same scheduling contract as the Mojo prototype, but without C ABI
packing/unpacking or cross-language function pointers.

## Run

```sh
make cpp-vpi-run
```

Expected end of output:

```text
cpp enable: time=11 count=5 drive en=0, done
cpp monitor: observed target count, done
cpp tb: PASS final_count=5
```

## Current Limit

This is still a Verilator-hosted prototype. The simulator-facing mechanism is
standard VPI, but the executable owns the top-level simulation loop and calls
`VerilatedVpi::callValueCbs()` after `eval()`. A fully loadable simulator plugin
would move startup into simulator registration hooks such as
`vlog_startup_routines` and simulation callbacks.

## Coroutine API Experiment

The next prototype adds `include/cpptb/coro_runtime.hpp` and a more realistic
RgGen-derived DUT under `experiments/cpp_vpi/rggen_apb_event/`.

Run it with:

```sh
make cpp-apb-event-run
```

The testbench registers concurrent coroutines once:

```cpp
void register_tests(coro::Testbench& tb, ApbEventDut dut) {
    tb.spawn(reset_driver(tb, dut));
    tb.spawn(apb_register_sequence(tb, dut));
    tb.spawn(irq_monitor(tb, dut));
    tb.spawn(sleep_monitor(tb, dut));
}
```

Inside a coroutine, scheduler interaction is implicit:

```cpp
Task<void> apb_register_sequence(coro::Testbench& tb, ApbEventDut dut) {
    co_await wait_reset_released(dut);

    const ApbMaster apb{tb, dut};
    co_await apb.write("write irq enable", kIrqEnable, 0x0000'002a);

    dut.irq_i.set(0x0000'0008);
    co_await clock_cycles(dut.HCLK, 3);

    co_await apb.read_expect("irq pending from irq_i", kIrqPending, 0x8);
}
```

This version still uses VPI for signal lookup, reads, writes, and clock
callbacks, but C++20 coroutines remove process ids and explicit wait-request
returns from user test code. Void coroutines use `Task<void>`; typed,
move-only results use `Task<T>` and are moved through `co_return`/`co_await`.
Scheduler roots and `Join` children remain `Task<void>`. See
`docs/testbench-authoring.md` for the live Authoring Core API, including
`clock_cycles`, edge timeouts, predicate waits, sticky events, and unbounded
FIFO channels.
