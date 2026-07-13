# Clockless DPI timer example

This example validates absolute `Delay` scheduling without any configured DUT
clock. Two independent coroutine cadences run at exact 7 ns and 11 ns ticks.
Each drive is followed by an explicit 1 ps settling delay, with the next wait
shortened by 1 ps so cadence deadlines do not drift. The pure-SystemVerilog
twin uses the same `#` schedule and checks.

```sh
make cpp-dpi-timer-only-run
make cpp-dpi-timer-only-sv-run
make feature-test FEATURE=dpi_timer_only
```

Both executables accept `CPPTB_TIMER_ONLY_ITERS`. The C++ executable reports
`CPP_DPI_TIMER_ONLY_RESULT`; the pure-SV executable reports
`PURE_SV_TIMER_ONLY_RESULT`. Their `iterations`, `checks`, `sim_cycles`, and
`failures` fields must match exactly, and `sim_cycles` is always zero.
