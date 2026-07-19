# Interfaces and inouts

This runnable example contains a parameterized interface array, two C++-owned
interface clocks, modport-directed input and output members, an interface
inout, and a top-level four-bit inout.

```sh
make cpp-dpi-interfaces-run
make cpp-dpi-interfaces-sv-run
make feature-test FEATURE=dpi_interfaces
```

The paired benches perform the same eight checks and one primary-clock cycle.
The feature registry treats this short workload as semantic equivalence; use
the longer authoring workloads for stable performance ratios.

<div class="cpptb-code-tabs" data-tabs="2" data-tab-group="interfaces" data-tab-label="Interface and inout stimulus"></div>

<div class="cpptb-code-tab-label">cpptb (C++ DPI)</div>

```cpp
dut.links[0].clk.set(0);
dut.links[1].clk.set(0);
test.start_clock(dut.links[0].clk, 10_ns);
test.start_clock(dut.links[1].clk, 14_ns);

dut.links[0].reset_n.set(1);
dut.links[1].reset_n.set(1);
dut.links[0].data.set(0x24);
dut.links[1].data.set(0x35);
co_await Delay{1_ns};
test.expect_eq("link zero observed", dut.links[0].observed.get(), 0x24u);
test.expect_eq("link one observed", dut.links[1].observed.get(), 0x36u);

dut.links[0].sideband.drive(1);
co_await Delay{1_ns};
test.expect_eq("testbench interface drive",
               dut.links[0].sideband.get(), 1u);
dut.links[0].sideband.high_z();

dut.gpio.drive(0x5);
co_await Delay{1_ns};
test.expect_eq("testbench top-level inout", dut.gpio_seen.get(), 0x5u);
dut.gpio.high_z();
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
initial forever #5ns link_clk[0] = ~link_clk[0];
initial forever #7ns link_clk[1] = ~link_clk[1];

links[0].reset_n = 1;
links[1].reset_n = 1;
links[0].data = 8'h24;
links[1].data = 8'h35;
#1ns;
expect_eq("link zero observed", links[0].observed, 8'h24);
expect_eq("link one observed", links[1].observed, 8'h36);

sideband_tb_drive[0] = 1;
sideband_tb_oe[0] = 1;
#1ns;
expect_eq("testbench interface drive", links[0].sideband, 1);
sideband_tb_oe[0] = 0;

gpio_tb_drive = 4'h5;
gpio_tb_oe = 1;
#1ns;
expect_eq("testbench top-level inout", gpio_seen, 4'h5);
gpio_tb_oe = 0;
```

The authored files are in `examples/interfaces/`. Generated interface
instances, flattened DPI transport storage, clock identities, and inout
enable/value drivers remain under `build/cpptb/interfaces/`.
