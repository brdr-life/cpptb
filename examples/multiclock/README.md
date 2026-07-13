# Multi-clock DPI trigger prototype

This example validates the generated multi-clock wrapper and the coroutine
trigger model against a dual-clock mailbox. The user-facing testbench is
`testbench.cpp`; generated bindings and simulator scheduling remain outside it.

The testbench exercises concurrent producer and consumer tasks, `Join`, exact
simulation-time `Delay`, `First`, any-edge waits, and explicit post-edge
settling delays.

```sh
make cpp-dpi-multiclock-run
make cpp-dpi-multiclock-sv-run
```

Both executables accept `CPPTB_MULTICLOCK_ITERS`. The C++ executable reports
one `CPP_DPI_MULTICLOCK_RESULT` and the pure-SV executable reports one
`PURE_SV_MULTICLOCK_RESULT`. Each marker contains exactly these fields in
order: `iterations`, `checks`, `sim_cycles`, and `failures`.

The feature-regression adapter classifies this example as `equivalence_only`.
It passes only when all four fields match exactly and `failures=0`; elapsed
runtime is neither compared nor normalized. Run it through the registry with:

```sh
make feature-test FEATURE=dpi_multiclock
make feature-benchmark FEATURE=dpi_multiclock
```
