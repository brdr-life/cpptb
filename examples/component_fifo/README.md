# Component FIFO example

This example verifies a ready/valid FIFO through the reusable transaction
component layer. It deliberately sits beside `fifo_scoreboard`: that example
shows direct `Queue` and coroutine composition, while this one shows typed
ports, analysis fan-out, a buffered subscriber, an in-order scoreboard, and
ready/valid helpers imported explicitly from `cpptb_vc`.

Run the C++ DPI testbench and its exact SystemVerilog twin:

```sh
make cpp-dpi-component-fifo-run
make cpp-dpi-component-fifo-sv-run
make feature-test FEATURE=dpi_component_fifo
```

The authored testbench uses:

- `AnalysisPort<uint32_t>` for zero-time expected and observed publication;
- `InOrderScoreboard<uint32_t>` for nonfatal transaction comparison;
- `AnalysisBuffer<uint32_t>` with an explicit overflow policy;
- `GetPort<uint32_t>` to keep the audit consumer independent of storage; and
- `ReadyValidDriver` and `ReadyValidMonitor` for explicit pin-level protocol
  activity.

The framework does not start a process, reset the DUT, or advance simulation
time implicitly. The test starts every process through `Join`, performs reset
explicitly, and supplies the ready/valid sample edge and delay.
