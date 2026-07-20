# Mixed-language logging

This example emits ordinary C++ testbench logs and simulation-only logs from
inside the authored SystemVerilog DUT. Both use the same CPPTB sink, severity
filter, simulation-time ordering, and per-test sequence numbers.

```sh
make cpp-dpi-mixed-logging-run
make cpp-dpi-mixed-logging-output-test
make cpp-dpi-mixed-logging-sv-run
```

The SystemVerilog instrumentation is guarded by
`CPPTB_ENABLE_SV_LOGGING`, which `cpptb build` supplies automatically. It is
absent from synthesis and from the standalone pure-SystemVerilog comparison.
