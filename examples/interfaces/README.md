# Interfaces and bidirectional ports

This example uses a parameterized SystemVerilog interface array with an
explicit modport, two independently driven interface clocks, and both
interface-member and top-level `inout` signals.

The generated C++ hierarchy follows the authored RTL names:

```cpp
dut.links[0].data.set(0x24);
const auto observed = dut.links[0].observed.get();

dut.links[0].sideband.drive(1);
dut.links[0].sideband.high_z();

dut.gpio.drive(0x5);
dut.gpio.high_z();
```

`drive()` and `high_z()` change drive intent immediately. They do not insert a
delay. Add an explicit `Delay`, edge wait, or sampling phase when a dependent
RTL result must settle before it is checked.

Run the matching C++ DPI and pure-SystemVerilog peers with:

```sh
make cpp-dpi-interfaces-run
make cpp-dpi-interfaces-sv-run
```
