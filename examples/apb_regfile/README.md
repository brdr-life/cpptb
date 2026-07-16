# APB register-file example

This example wraps APB setup and access phases in a reusable `ApbMaster`.
`write()` is a `Task<void>`, `read()` returns a value with `Task<uint32_t>`,
and `read_expect()` composes a transaction with a check. The main sequence is
therefore register-oriented instead of pin-oriented:

```cpp
co_await apb.write(address, value);
co_await apb.read_expect("APB register readback", address, value);
```

Run the C++ DPI testbench and its exact SystemVerilog twin:

```sh
make cpp-dpi-apb-regfile-run
make cpp-dpi-apb-regfile-sv-run
make feature-test FEATURE=dpi_apb_regfile
```

Both implementations hold bus controls through the active clock edge and
sample `PREADY`/`PRDATA` after the same explicit `1ps` settle delay. An
unmapped read also verifies the APB error path.

Both peers use the fixed semantic workload `kRegisterTransactions = 12`.
Build the typed `Dut`, wrapper, transport, and simulator with:

```sh
uv run --frozen cpptb build
```

| cpptb | SystemVerilog |
|---|---|
| `Task<void> write(...)` | `task automatic apb_write_word(...)` |
| `Task<uint32_t> read(...)` | task with an `output` value |
| `co_await read(...)` | blocking task call |
| `dut.apb.address.set(...)` | `apb_address = ...` |
