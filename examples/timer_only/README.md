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

Both peers run the fixed `kCadenceSamples = 9` workload. This clockless target
is generated directly from `timer_only_probe.sv`; its test simply never calls
`start_clock()`.
Their checks and `sim_cycles=0` must match exactly; the result marker reports
`iterations=1` because this is an equivalence example, not a scalable
benchmark.
