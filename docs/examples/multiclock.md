# Multiple clocks

The dual-clock mailbox declares clock timing in the C++ testbench. Port names
carry no timing semantics, and generation needs only the RTL:

```sh
uv run --frozen cpptb-codegen \
  examples/multiclock/dual_clock_mailbox.sv
```

The target-unique generated type is
`cpptb::generated::dual_clock_mailbox::Dut`. Producer and consumer coroutines
wait on the clock that owns their interface, then the registered test composes
them with reset and a phase probe:

```cpp
constexpr uint32_t kTransferCount = 16;

Task<void> traffic(Dut dut, TestContext& test) {
    co_await Join{producer(dut), consumer(dut, test)};
    test.expect_eq("write count", dut.write_count.get(), kTransferCount);
    test.expect_eq("read count", dut.read_count.get(), kTransferCount);
}

Task<void> multiclock_test(Dut dut, TestContext& test) {
    dut.write_clk.set(0);
    dut.read_clk.set(0);
    test.start_clock(dut.write_clk, 4_ns);
    test.start_clock(dut.read_clk, 6_ns, 1_ns);

    co_await Join{reset_dut(dut, test), traffic(dut, test),
                  trigger_and_phase_probe(dut, test),
                  output_clock_probe(dut, test)};
}

CPPTB_REGISTER_TEST(multiclock_test);
```

`TestContext::now()` checks absolute time, while `First`, `Edge`, and explicit
`Delay{1_ps}` waits make trigger selection and settle points visible. The pure
SV peer uses the same `kTransferCount = 16` workload. The example also awaits
`RisingEdge{dut.output_clk}`. Because `output_clk` is produced by the DUT, it
is observed rather than passed to `start_clock()`.

```sh
make cpp-dpi-multiclock-run
make cpp-dpi-multiclock-sv-run
```
