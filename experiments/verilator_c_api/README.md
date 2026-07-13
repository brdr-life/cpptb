# Mojo + Verilator Counter Demo

This directory is a small proof that Mojo can drive a Verilated hardware model.

The integration shape is:

1. Verilator compiles `counter.sv` into a C++ model.
2. `counter_c_api.cpp` wraps that C++ model with an `extern "C"` API.
3. `counter_driver.mojo` loads `build/libcounter.dylib` with `std.ffi.OwnedDLHandle`.
4. Mojo calls the C ABI functions to reset, tick, and read the simulated counter.

Run it from the repo root:

```sh
make run
```

Expected output:

```text
after reset: 0
after 5 enabled ticks: 5
after disabled tick: 5
```
