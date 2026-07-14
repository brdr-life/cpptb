# Clockless timers

`Delay` does not depend on a DUT clock. This example is generated directly
from its RTL without a clock option:

```sh
uv run --frozen cpptb-codegen \
  examples/timer_only/timer_only_probe.sv
```

Two independent cadences share one fixed semantic workload and are joined by
the registered test:

```cpp
constexpr uint32_t kCadenceSamples = 9;

Task<void> timer_only_test(Dut dut, TestContext& test) {
    co_await Join{fast_cadence(dut, test), slow_cadence(dut, test)};

    test.expect_eq("timer-only final absolute time",
                   test.now().in_picoseconds(),
                   static_cast<uint64_t>(kCadenceSamples) * 11'000u + 1u);
}

CPPTB_REGISTER_TEST(timer_only_test);
```

Each write is followed by an explicit `Delay{1_ps}` before sampling. The write
itself does not advance simulation time. The pure SV peer mirrors
`kCadenceSamples = 9` and the same `#1ps` settle points.

```sh
make cpp-dpi-timer-only-run
make cpp-dpi-timer-only-sv-run
```
