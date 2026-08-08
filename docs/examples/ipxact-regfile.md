# IP-XACT register model

This example starts from a checked-in IP-XACT component rather than
SystemRDL. PeakRDL imports the standard IP-XACT memory map and the cpptb
exporter generates the same typed C++ RAL used by the other register examples.
The user does not author an intermediate `.rdl` file or check in generated C++.

The complete source is under `examples/ipxact_regfile/`:

```text
ipxact_regfile/
├── component.xml                  # authored IP-XACT contract
├── ipxact_regfile.sv              # authored APB4 RTL
├── testbench.cpp                  # generated-model APB test
├── model_contract.cpp             # simulator-free fake-master check
├── cpptb.toml
└── systemverilog/
    └── ipxact_regfile_sv_tb.sv    # matching direct-SV sequence

build/cpptb/ipxact_regfile/
├── generated/ipxact_regs.hpp      # generated typed model
├── obj/                           # C++ DPI simulator
└── systemverilog_obj/             # pure-SV simulator
```

## Author the contract

The checked-in `component.xml` uses IP-XACT 1685-2014. Its register address
block defines fields, an enum, and a four-element register array. A separate
native memory address block describes the scratchpad:

```xml
<ipxact:register>
  <ipxact:name>control</ipxact:name>
  <ipxact:addressOffset>'h0</ipxact:addressOffset>
  <ipxact:size>32</ipxact:size>
  <ipxact:field>
    <ipxact:name>enable</ipxact:name>
    <ipxact:bitOffset>0</ipxact:bitOffset>
    <ipxact:bitWidth>1</ipxact:bitWidth>
    <ipxact:access>read-write</ipxact:access>
  </ipxact:field>
</ipxact:register>

<ipxact:addressBlock>
  <ipxact:name>scratchpad</ipxact:name>
  <ipxact:baseAddress>'h100</ipxact:baseAddress>
  <ipxact:range>'h40</ipxact:range>
  <ipxact:width>32</ipxact:width>
  <ipxact:usage>memory</ipxact:usage>
  <ipxact:access>read-write</ipxact:access>
</ipxact:addressBlock>
```

The example build runs the equivalent generation command:

```sh
uv run --frozen --extra peakrdl peakrdl cpptb \
  examples/ipxact_regfile/component.xml \
  -o build/cpptb/ipxact_regfile/generated/ipxact_regs.hpp \
  --namespace ipxact_regs \
  --rename peripheral
```

`--namespace` selects the C++ namespace. `--rename` selects the logical model
root used by descriptors and diagnostics, so paths read
`peripheral.registers.control` instead of inheriting the component and memory
map wrapper names. The XML filename and generated header name are independent.

## Use the generated hierarchy

The generated model preserves the IP-XACT address-block hierarchy. Ordinary
registers, register arrays, and memory all use typed handles:

```cpp
ipxact_regs::RegModel regs{test, master};

co_await regs.registers.control.write(0x5u);

regs.registers.control.enable.set_desired(1u);
regs.registers.control.mode.set_desired(
    ipxact_regs::mode_enum_t::STREAM);
co_await regs.registers.control.update();

co_await regs.registers.threshold.at<2>().write(0x1234u);

constexpr std::array<uint32_t, 3> packet{
    0x1122'3344u, 0xa5a5'5a5au, 0xcafe'babeu};
co_await regs.scratchpad.write(
    4, std::span<const uint32_t>{packet});
```

The register array index is checked at compile time. The memory call writes
three words beginning at logical entry 4, which maps to absolute addresses
`0x110`, `0x114`, and `0x118`.

## Compare the executable sequence

The two benches issue the same APB4 transfers and both finish at 410 ns. The
C++ version uses the generated contract; the SV version spells out the
corresponding addresses and bus operations.

<div class="cpptb-code-tabs" data-tabs="2" data-tab-group="ipxact-regfile" data-tab-label="Generated RAL sequence"></div>

<div class="cpptb-code-tab-label">cpptb (C++ DPI)</div>

```cpp
ipxact_regs::RegModel regs{test, master};

co_await regs.registers.control.write(0x5u);
const auto control = co_await regs.registers.control.read();
test.expect_eq("control readback", control.data, 0x5u);

regs.registers.control.mode.set_desired(
    ipxact_regs::mode_enum_t::STREAM);
co_await regs.registers.control.update();

co_await regs.registers.threshold.at<2>().write(0x1234u);
co_await regs.scratchpad.write(4, packet);
co_await regs.scratchpad.read(4, readback);
test.expect_eq("scratchpad readback", readback, packet);
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
apb_write_word(9'h000, 32'h0000_0005);
apb_read_word(9'h000, data);
expect_eq("control readback", data, 32'h0000_0005);

// Combined enable/mode field update.
apb_write_word(9'h000, 32'h0000_0003);

apb_write_word(9'h028, 32'h0000_1234);
for (int unsigned index = 0; index < 3; index++)
  apb_write_word(9'h110 + (index * 4), packet[index]);
for (int unsigned index = 0; index < 3; index++) begin
  apb_read_word(9'h110 + (index * 4), data);
  expect_eq("scratchpad readback", data, packet[index]);
end
```

Run the complete example from the repository root:

```sh
make cpptb-ipxact-regfile-model-test
make cpp-dpi-ipxact-regfile-run
make cpp-dpi-ipxact-regfile-sv-run
```

Or run `make test` inside `examples/ipxact_regfile/`.

## Compatibility boundary

cpptb accepts IP-XACT through the installed PeakRDL importer. Registers,
fields, arrays, standard access and reset properties, enumerated values, and
native memory address blocks all reach the generated model in this regression.
The checked-in example is regenerated, compiled against a fake master, and run
over APB in the normal test suite.

Vendor extensions are not automatically portable. PeakRDL must first map an
extension into SystemRDL semantics before the cpptb exporter can preserve it.
When a project depends on custom IP-XACT properties, add that mapping to the
import layer or keep a SystemRDL source contract; silently assuming an unknown
extension affected the model is not supported.

See [Register generation](../verification-components/register-generation.md)
for naming controls and [Memory and register models](../memory-register-models.md)
for the complete generated API.
