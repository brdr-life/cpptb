# APB register file

The generated `Dut` stays generic while `ApbMaster` is an ordinary user helper
that packages APB setup and access phases. It stores the typed DUT and a
pointer to the library-owned context for checks:

```cpp
class ApbMaster {
  public:
    ApbMaster(Dut dut, TestContext& test) : dut_(dut), test_(&test) {}

    Task<void> write(uint32_t address, uint32_t data) const;
    Task<uint32_t> read(uint32_t address) const;

  private:
    Dut dut_;
    TestContext* test_;
};
```

The registered sequence is protocol-oriented and has a fixed semantic
transaction count shared with the pure SV peer:

```cpp
constexpr uint32_t kRegisterTransactions = 12;

Task<void> register_sequence(Dut dut, TestContext& test) {
    dut.clk.set(0);
    test.start_clock(dut.clk, 10_ns);

    co_await reset_dut(dut);
    const ApbMaster apb{dut, test};
    uint32_t state = 0x1020'3040u;

    for (uint32_t index = 0; index < kRegisterTransactions; ++index) {
        const uint32_t address = (index % 4u) * 4u;
        const uint32_t value = next_word(state);
        co_await apb.write(address, value);
        co_await apb.read_expect("APB register readback", address, value);
    }
}

CPPTB_REGISTER_TEST(register_sequence);
```

Build and run it with
`cpptb test --project examples/apb_regfile --build-dir build`.
Signal writes do not insert protocol delays; the helper explicitly drives,
waits, settles, and samples.
