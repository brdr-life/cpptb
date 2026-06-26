# Micro-MojoTB VPI Prototype

This is a first sketch of a cocotb-like stack with Mojo as the test language.

The layers are:

1. Verilator builds the RTL with `--vpi --public-flat-rw`.
2. `verilator_vpi_host.cpp` is the reusable simulator bridge.
   It uses standard VPI calls such as `vpi_handle_by_name`, `vpi_get_value`,
   `vpi_put_value`, and `vpi_register_cb`.
3. `runtime.mojo` defines the small ergonomic layer: simulator function pointer
   types, opaque signal handles, interned signal IDs, `Signal.get()`,
   `Signal.set(value)`, `RisingEdge`, `WaitRequest`, and `Scheduler`.
4. `tests/counter_test.mojo` builds as a shared library, exports C ABI
   callbacks, and uses `CounterDut` as the generated-style signal path.
5. The bridge starts several Mojo processes, tracks each process's current
   `WaitRequest`, and resumes every process whose trigger fires.

Run from the repo root:

```sh
make vpi-run
```

This is intentionally much smaller than cocotb, but it is the scalable shape:
the test talks in terms of simulator handles and callbacks instead of generated
per-DUT C++ methods.

The test now has several small Mojo processes. Signal access stays primitive,
while scheduling is expressed as explicit trigger requests:

```mojo
struct ResetDriver:
    def start(self) -> UInt64:
        self.dut.rst.set(1)
        return self.scheduler.wait(RisingEdge(self.dut.clk), PhaseRun)

    def resume(self, time: UInt64) -> UInt64:
        self.dut.rst.set(0)
        return self.scheduler.finish()
```

Another process can drive enable concurrently:

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

The host-side scheduler keeps a process table. When `clk` rises, it resumes all
processes currently waiting on `RisingEdge(dut.clk)`, applies each returned
`WaitRequest`, and stops when all processes have returned `scheduler.finish()`.

`CounterDut` is currently hand-written, but it is shaped like generated
bindings: deeper hierarchy can become fields such as
`dut.core.counter.count.get()`.
