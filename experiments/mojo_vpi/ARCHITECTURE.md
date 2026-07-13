# A Mojo Cocotb-Style Prototype

The scalable version should not generate one C function per DUT signal. It
should look more like cocotb:

```text
Simulator kernel
  -> VPI/VHPI/FLI callback bridge
  -> Mojo scheduler and tests
  -> VPI signal get/put and callback registration
```

## Current Prototype

The `experiments/mojo_vpi/` prototype proves the first slice of that architecture with
Verilator and VPI:

```text
experiments/mojo_vpi/examples/vpi_counter.sv
  -> verilator --cc --vpi --public-flat-rw
  -> experiments/mojo_vpi/verilator_vpi_host.cpp
  -> dlopen(build/experiments/mojo_vpi/libcounter_test.dylib)
  -> experiments/mojo_vpi/tests/counter_test.mojo
```

The C++ bridge uses standard VPI concepts:

- `vpi_handle_by_name` to resolve hierarchical signal names.
- `vpi_get_value` to read signal values.
- `vpi_put_value` to drive signal values.
- `vpi_register_cb` to subscribe to clock value changes.

The C++ bridge passes generic simulator hooks into the Mojo test:

- `get_u32(handle) -> UInt32`
- `put_u32(handle, value)`
- `resolve_signal(signal_id) -> SignalHandle`

The Mojo runtime layer wraps those raw pieces:

- `SignalHandle`
- `GetU32Fn`
- `PutU32Fn`
- `ResolveSignalFn`
- interned signal IDs such as `SignalClk` and `SignalCount`
- `Signal` with `get()` and `set(value)`
- `RisingEdge`
- `WaitRequest`
- `Scheduler`

The counter test then builds a DUT-specific generated-style path and several
small testbench processes in Mojo. Each process owns one concurrent activity and
returns the next event it wants to wait for:

```mojo
struct EnableDriver:
    def resume(self, time: UInt64) -> UInt64:
        var rst_value = self.dut.rst.get()
        var count_value = self.dut.count.get()

        if rst_value != 0:
            self.dut.en.set(0)
        elif count_value < 5:
            self.dut.en.set(1)
        else:
            self.dut.en.set(0)
            return self.scheduler.finish()

        return self.scheduler.wait(RisingEdge(self.dut.clk), PhaseRun)
```

The current example has three processes:

- `ResetDriver` asserts reset, waits one clock, then deasserts reset and exits.
- `EnableDriver` drives `en` until the counter reaches 5, then exits.
- `CountMonitor` observes `count` on every clock and exits when it sees 5.

`CounterDut` is hand-written in the prototype, but its shape matches what a
binding generator could emit. A deeper design could expose paths like
`dut.core.counter.count.get()` and `dut.core.counter.en.set(1)` while still
using the same primitive simulator hooks underneath. Higher-level behavior such
as reset sequences is intentionally left explicit in the test.

The clock is split deliberately in this prototype: Mojo initializes `dut.clk`
and each process returns a `WaitRequest` for `RisingEdge(dut.clk)`, while the
Verilator adapter owns the simple clock driver loop. That keeps the first VPI
bridge small. A later Mojo `Clock(dut.clk, period)` process could move clock
generation into Mojo.

The Mojo test exports C ABI callbacks:

- `mojotb_process_count() -> UInt32`
- `mojotb_on_setup(...)`
- `mojotb_on_process_start(..., process_id) -> encoded WaitRequest`
- `mojotb_on_process_resume(..., process_id, time, phase) -> encoded WaitRequest`
- `mojotb_on_done() -> status`

Run it with:

```sh
make vpi-run
```

Expected output:

```text
tb: setup clk=0 rst=1 en=0
reset: rst=1, wait one clock
enable: en=0, waiting for reset release
monitor: waiting for count activity
...
tb: PASS final_count= 5
```

## Why This Scales Better Than The C-Shim Demo

The first demo in `examples/` wraps the Verilated C++ model with functions like
`counter_tick()` and `counter_value()`. That is fast and simple, but it is
design-specific.

This prototype moves toward a reusable verification layer. The bridge talks to
the simulator through VPI, so the same mechanism can resolve hierarchical
signals and register simulator callbacks. The Mojo side now has a small place
to grow process-style agents like:

```mojo
struct OutputMonitor:
    def resume(self, time: UInt64) -> UInt64:
        if self.dut.valid.get() == 1:
            observe(self.dut.data.get())
        return self.scheduler.wait(RisingEdge(self.dut.clk), PhaseRun)
```

The current API is still explicit-process-based rather than coroutine-based,
but the key pieces are now proven:

- Mojo can build a shared library.
- Mojo can export C ABI functions with `@export`.
- C++ can load the Mojo test dynamically.
- VPI can read, write, and callback into the test flow.
- Mojo can select signals by name through a DUT wrapper.
- Mojo can return trigger requests to the simulator adapter.
- The adapter can track multiple active Mojo processes at once.
- The adapter can stop the simulation when every process returns
  `scheduler.finish()`.

## Next Increments

1. Replace the interned resolver with true path lookup once the C-string ABI is
   comfortable: `get_handle(path)`, `get_u64(handle)`, `put_u64(handle, value)`,
   `register_value_change(handle, callback_id)`, and `finish(status)`.
2. Add a small generated dispatch layer so each process can be registered by
   name rather than by a hand-written `process_id` switch.
3. Add typed signal wrappers for scalar, vector, string, and packed array
   values.
4. Add a simulator adapter boundary:
   Verilator starts as the first adapter, but Icarus/Questa/Xcelium-style VPI
   loading should only need a different bridge entry point.
5. Add generated bindings only for ergonomics, not for simulator access:
   inspect or parse hierarchy once, then generate Mojo names for signals.

## Verilator Notes

Verilator VPI is intentionally limited and only exposes public signals. The
prototype uses `--public-flat-rw` to make the small demo easy to access. In a
real design, a control file or explicit Verilator pragmas would be better than
marking everything public.

Verilator also differs from event-driven simulators: values written through VPI
do not propagate until the Verilated model is evaluated. The bridge handles
that by calling `eval()` and `VerilatedVpi::callValueCbs()` in its event loop.
