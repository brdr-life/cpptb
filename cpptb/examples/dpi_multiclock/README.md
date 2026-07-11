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

Both executables accept `CPPTB_MULTICLOCK_ITERS` and report the iteration,
check, primary-clock-cycle, and failure counts. The C++ and pure-SV versions
run the same three concurrent processes and are intended to be compared only
when all four result fields match.
