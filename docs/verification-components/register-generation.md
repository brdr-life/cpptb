# Generate a register model

cpptb generates typed C++ register handles from a register contract. SystemRDL
and IP-XACT use the PeakRDL exporter plugin; native RgGen YAML, JSON, and TOML
use the companion `cpptb-rggen` command. Generation is a build step: the
generated header belongs under `build/`, while the contract remains authored
source.

This facility is part of the optional `cpptb_vc` package. It does not alter DUT
signal generation and is not needed by testbenches that use only direct
hierarchical access.

## What the generator consumes

SystemRDL is the preferred source because it represents register widths,
access widths, fields, reset values, software access, read/write side effects,
volatility, hierarchy, arrays, and memories directly. For example:

```systemverilog
addrmap peripheral {
    hdl_path = "u_regs";
    default regwidth = 32;
    default accesswidth = 32;

    reg {
        hdl_path = "control_q";
        field { sw = rw; hw = r; reset = 0; } enable[0:0];
        field { sw = rw; hw = r; reset = 0; } mode[3:1];
    } control @ 0x00;

    reg {
        field {
            sw = rw;
            hw = r;
            onwrite = woclr;
            reset = 8'hff;
        } pending[7:0];
        field {
            sw = r;
            hw = w;
            onread = rclr;
        } sampled[15:8];
    } status @ 0x04;

    external mem {
        mementries = 16;
        memwidth = 32;
        sw = rw;
        hdl_path_slice = '{"buffer_storage"};
    } buffer @ 0x100;
};
```

The source describes the hardware contract, not a test sequence. Clocks, bus
timing, reset, test policy, and checks remain in authored C++.

`hdl_path` and `hdl_path_slice` are standard SystemRDL verification properties.
When present, cpptb uses them to generate typed backdoor access; no separate
path configuration file or handwritten dispatch table is required.

## Install and generate

Install the optional PeakRDL dependencies and invoke the `cpptb` exporter:

```sh
uv sync --extra peakrdl
uv run --frozen --extra peakrdl peakrdl cpptb registers.rdl \
  -o build/generated/peripheral_regs.hpp \
  --namespace peripheral_regs
```

Split transfers default to little-endian CPU byte order. Select a big-endian
frontdoor explicitly when that is the bus contract:

```sh
uv run --frozen --extra peakrdl peakrdl cpptb registers.rdl \
  -o build/generated/peripheral_regs.hpp \
  --register-endianness big
```

This option controls the ordering of `accesswidth` chunks within every
`regwidth`; it does not reverse field bit numbering or alter HDL backdoor
paths.

Registers wider than 64 bits are generated as typed `Bits<Width>` handles.
Their complete reset values and reset-valid masks are emitted as word arrays,
so no additional source annotation or handwritten packing code is required.
`accesswidth` selects the width of each frontdoor transfer and must be no more
than 64 bits and fit the selected transport data type.

SystemRDL `encode` declarations generate a C++ `enum class` and make the field
member typed. Enum declaration names and members are sanitized only when needed
to form valid C++ identifiers. Reusing one SystemRDL enum reuses one generated
C++ type. The generated field's `.raw()` member remains available for reserved
encodings and negative tests; both views share one register-model state.

Every generated top-level model exposes `reset_all()`, `update_all()`, and
`mirror_all()` in ascending address order. These are model/frontdoor operations:
`reset_all()` does not drive a reset signal, and none of them inserts an
implicit delay. `mirror_all()` omits registers that are entirely write-only.

Generated handles also expose allocation-free metadata and traversal:

```cpp
regs.for_each_register([](const auto& reg) {
    inspect(reg.path(), reg.address(), reg.width());
});
regs.control.for_each_field([](const auto& field) {
    inspect(field.path(), field.lsb(), field.width());
});
regs.for_each_memory([](const auto& memory) {
    inspect(memory.path(), memory.size());
});
```

`path()` is the logical SystemRDL hierarchy. `hdl_path()` returns the generated
RTL path when there is exactly one mapping; `hdl_slices()` represents split
register and field storage without flattening it into a misleading string.
Model-wide descriptor iteration is available through `regs.descriptor()`.
See [Introspection and traversal](../memory-register-models.md#introspection-and-traversal)
for memory slices, register arrays, path printing, and the frontdoor/backdoor
usage rules.

Generated memories support entry indices, byte offsets relative to the memory,
and absolute bus addresses. Scalar and caller-owned chunk operations use the
same vocabulary:

```cpp
std::array<uint32_t, 16> words{};

co_await regs.buffer.read(8, words);                    // Entry 8.
co_await regs.buffer.read_offset(0x20, words);          // 0x20 from buffer.
co_await regs.buffer.read_absolute(0x4000'0120, words); // Bus address.
```

The complete chunk must remain within `regs.buffer`; offsets and addresses must
be element-aligned. Every form accepts an optional final `AccessPath` argument,
and matching `write`, raw `peek`, and raw `poke` forms are available. See
[Register-backed memories](../memory-register-models.md#register-backed-memories)
for the complete API table and frontdoor/backdoor examples.

## Naming the generated model

No filename or C++ type name is forced to be `registers`. Five independent
naming layers are involved:

| Layer | Selected by | What it changes |
|---|---|---|
| Address-map type/top | SystemRDL declaration and `--top` | Which `addrmap` PeakRDL elaborates |
| Top instance | SystemRDL instance name or `--rename` | Logical descriptor-path root and the default C++ namespace |
| Header path and filename | `-o` | Where the generated file lives and how authored C++ includes it |
| C++ namespace | `--namespace` | C++ qualification only; it does not change logical SystemRDL paths |
| C++ model class | `--class-name` | Generated C++ class identifier only; default `RegModel` |

For this source:

```systemverilog
addrmap uart {
    reg { field { sw = rw; } enable[0:0]; } control @ 0x00;
};
```

the command below generates `soc_uart_regs.hpp`, a C++ type named
`soc_registers::UartRegisters`, and descriptor paths rooted at
`uart0.control`:

```sh
uv run --frozen --extra peakrdl peakrdl cpptb uart.rdl \
  --top uart \
  --rename uart0 \
  -o build/generated/soc_uart_regs.hpp \
  --namespace soc_registers \
  --class-name UartRegisters
```

The corresponding C++ spelling is independent of the logical descriptor root:

```cpp
#include "soc_uart_regs.hpp"

soc_registers::UartRegisters<Master> uart0_regs{test, master, uart0_base};
co_await uart0_regs.control.write(1u);
```

Without `--rename`, the instantiated top name defaults to the selected
component name. Without `--namespace`, that instance name is sanitized and
used as the C++ namespace. Use `--top` whenever the inputs expose more than one
possible address map; otherwise PeakRDL selects the last address map defined.

Generated register members retain the elaborated hierarchy below the top.
Scalar top-level registers remain direct members. Register files become nested
views, and arrays use compile-time indexed `at<Index>()` access:

```cpp
regs.control.enable.set_desired(1);
regs.security.key.key.set_desired(0x1234);
regs.bank.at<1>().control.value.set_desired(0xa55a);
regs.lane_control.at<0>().value.set_desired(0x5a);
```

Array views also provide deterministic `for_each()` and
`for_each_slice<First, Count>()`, while the block provides
`for_each_register()` in ascending address order. Compile-time indexing keeps
the concrete generated handle type and therefore preserves named fields.

Flattened nested members such as `security_key` are retained as compatibility
storage aliases for existing generated-model users; new code should use the
hierarchical view. Non-identifier characters are replaced, C++ keywords
receive a trailing underscore, and collisions receive a stable numeric suffix.
Logical descriptor paths always preserve the elaborated SystemRDL hierarchy.

## Actual core: secworks AES

The pinned secworks integration deliberately uses names specific to the core,
not generic `generated/registers.hpp` naming. Its contract begins with:

```systemverilog
addrmap secworks_aes {
    default regwidth = 32;
    default accesswidth = 32;
    // control, config, key0..key7, block0..block3, result0..result3
};
```

Its Makefile selects an output named `aes_regs.hpp` and a descriptive C++
namespace:

```makefile
GENERATED_HEADER := $(BUILD_DIR)/generated/aes_regs.hpp

$(GENERATED_HEADER): $(RDL)
	uv run --frozen --extra peakrdl peakrdl cpptb $(RDL) \
	  -o $@ --namespace secworks_aes_regs
```

The testbench include, type, and generated member names match that command:

```cpp
#include "aes_regs.hpp"

using RegModel = secworks_aes_regs::RegModel<AesMaster>;

RegModel regs{test, master};
co_await regs.key0.write(key[0]);
co_await regs.config.write(aes256 ? 2u : 0u);
co_await regs.control.write(1u);
```

The project adds only the containing generated directory to
`testbench.include_dirs`. The filename comes from `-o`, the namespace comes
from `--namespace`, the class keeps its default `RegModel`, and members come
from the RDL instances. This complete source, command, configuration, and C++
usage is runnable under `benchmarks/regmodel_ground_truth/secworks_aes/`.

## Generator CLI reference

Run the installed command for the authoritative help associated with that
version:

```sh
peakrdl cpptb --help
peakrdl --plugins
```

The `cpptb` subcommand accepts PeakRDL's compilation and importer options plus
the exporter-specific C++ names:

| Argument | Purpose |
|---|---|
| `FILE [FILE ...]` | One or more SystemRDL, IP-XACT, or supported PeakRDL input files |
| `-o OUTPUT` | Required generated header path and filename |
| `-t TOP`, `--top TOP` | Select the top-level address-map component |
| `--rename INST_NAME` | Override the elaborated top instance name and logical path root |
| `--namespace NAMESPACE` | Override only the generated C++ namespace |
| `--class-name CLASS_NAME` | Override only the generated C++ class name |
| `--register-endianness {little,big}` | Select split-transfer CPU byte order; default `little` |
| `-I INCDIR` | Add a SystemRDL include search directory |
| `-D MACRO[=VALUE]` | Define a SystemRDL preprocessor macro |
| `-P PARAMETER=VALUE` | Override a top-level SystemRDL parameter |
| `--remap-state STATE` | Select an IP-XACT memory-remap state |
| `-f FILE` | Read additional command-line arguments from a file |
| `--peakrdl-cfg CFG` | Use a PeakRDL configuration TOML file |

`peakrdl globals INPUT...` lists globally accessible component types when the
correct `--top` is unclear. The CLI rejects a missing `-o` and prints these
options through standard `--help` without running generation.

Generated wide registers and register-backed memories support typed
`Bits<Width>` frontdoors, generated HDL backdoors, passive prediction, reset,
update, mirror, and bulk memory operations. A logical value may be wider than
the bus while each individual transport transfer remains at most 64 bits.

## Keep generation in the build

The generated header starts with a tool-version banner and `Do not edit`.
Regenerate it whenever the contract or generator changes. A minimal Make rule
looks like this:

```makefile
RDL := rtl/registers.rdl
REGS_HPP := build/generated/peripheral_regs.hpp

$(REGS_HPP): $(RDL) pyproject.toml uv.lock
	uv run --frozen --extra peakrdl peakrdl cpptb $(RDL) \
	  -o $@ --namespace peripheral_regs

build/cpptb/my_testbench: $(REGS_HPP)
```

Expose the generated directory to the testbench compiler:

```toml
[testbench]
sources = ["testbench.cpp"]
include_dirs = ["build/generated"]
```

`cpptb build` currently generates DUT signal bindings from RTL, but it does not
infer arbitrary register-contract sources. Keep the explicit register-header
dependency in the project build so generation is deterministic and visible.
The complete secworks example uses this pattern in
`benchmarks/regmodel_ground_truth/secworks_aes/Makefile`.

## What the header contains

For each elaborated address map, the exporter emits:

- constexpr block, register, field, and memory descriptors;
- typed register members and named field members;
- generated reset masks that preserve unspecified reset bits as unknown model
  state;
- absolute contract offsets plus a constructor-time relocatable base address;
- `regwidth`, `accesswidth`, reset value/mask, software access, side-effect,
  frontdoor endianness, and volatility metadata; and
- complete logical SystemRDL paths for diagnostics;
- standard register slices and memory-array paths for typed RTL backdoors; and
- an optional typed `DutBackdoor` plus `make_backdoor<Master>(dut)` factory.

Nested register files and arrays are elaborated and unrolled by PeakRDL. Their
logical paths are retained in descriptors and exposed as typed nested views.
For example, `peripheral.security.key` is available as `regs.security.key`, and
`peripheral.bank[1].control` is available as
`regs.bank.at<1>().control`. Compatibility flattened storage remains visible
for now, but authored tests should use the hierarchy.

Generated code includes only `cpptb_vc/register_model.hpp`. The backdoor is a
template and does not include a specific DUT binding, APB component, clock, or
simulator adapter. It is instantiated only when authored C++ passes a generated
`Dut` to `make_backdoor`.

## Construct and connect

Include the generated header and provide any type satisfying the
`MemoryMappedMaster` concept:

```cpp
#include "generated/peripheral_regs.hpp"

ApbMaster master{bus};
peripheral_regs::RegModel<decltype(master)> regs{
    test, master, 0x4000'0000};

const auto write = co_await regs.control.write(0x0000'0005);
test.require_eq("control write", write.transport.status, MemoryStatus::Okay);
```

The base address is added to every generated contract offset. This lets one
header represent the same IP at different subsystem or SoC addresses. Pass a
fourth constructor argument when the model also needs generated backdoor
access:

```cpp
auto backdoor = peripheral_regs::make_backdoor<decltype(master)>(dut);
peripheral_regs::RegModel<decltype(master)> regs{
    test, master, 0x4000'0000, &backdoor};
```

Generation does not choose a bus protocol. The frontdoor remains an authored
`MemoryMappedMaster`; the default backdoor comes from standard paths in the
register contract. The generated adapter implements both `RegisterBackdoor`
and `RegisterMemoryBackdoor`. Projects can instead supply custom adapters; a
separate memory adapter may be passed as the fifth model-constructor argument.

## HDL path mapping

The complete path is the dot-joined sequence of `hdl_path` properties from the
selected address map through its register file and register. Paths are relative
to the generated cpptb `Dut` root:

```systemverilog
addrmap peripheral {
    hdl_path = "u_regs";

    reg {
        hdl_path = "control_q";
        field { sw = rw; } enable[0:0];
    } control @ 0;
};
```

This maps `control` to `dut.u_regs.control_q`. Packed ranges are accepted, such
as `control_q[15:0]`. If a register is stored in separate field signals, omit
its register-level path and add field slices:

```systemverilog
field {
    hdl_path_slice = '{"pending_q"};
} pending[7:0];

field {
    hdl_path_slice = '{"state[1]", "state[0]"};
} state[1:0];
```

A single entry represents the complete field. Multiple entries must contain
one path per bit, ordered from the field MSB to LSB; other lengths are
ambiguous and rejected. The generated descriptor retains each complete path,
logical LSB, and width for diagnostics and tooling.

A memory uses one `hdl_path_slice` entry naming the complete one-dimensional
unpacked HDL array:

```systemverilog
external mem {
    mementries = 256;
    memwidth = 32;
    sw = rw;
    hdl_path_slice = '{"packet_ram"};
} packet_ram @ 0x1000;
```

The generated adapter maps logical memory entry zero to the array's declared
low bound, including arrays whose HDL range does not begin at zero. Omit the
property for a frontdoor-only memory. Backdoor use then fails with the complete
logical memory path instead of falling back to runtime string lookup.

The generated adapter uses deposits. It does not force storage and does not
advance simulation time. `peek()` and `poke()` immediately update model state;
any wait needed for RTL to react remains explicit in the testbench. Hierarchy
discovery adds only the typed `get` and `deposit` hooks actually instantiated
by the testbench, so frontdoor-only users pay no backdoor transport cost.

## IP-XACT input

PeakRDL can import an IP-XACT component before invoking the same exporter:

```sh
uv run --frozen --extra peakrdl peakrdl cpptb component.xml \
  -o build/generated/peripheral_regs.hpp \
  --namespace peripheral_regs
```

IP-XACT is useful when it is the existing integration contract, but conversion
is limited to properties the PeakRDL importer can map into SystemRDL semantics.
Retain SystemRDL as the source of truth when read/write effects, nested
memories, or project-specific properties would be lost in an interchange
round-trip.

The runnable [IP-XACT register-model example](../examples/ipxact-regfile.md)
starts from a checked-in 1685-2014 component and covers fields, an enum,
register arrays, and a native memory address block. Its generated model is
compiled against both a fake memory-mapped master and a real APB4 DUT, with a
matching pure-SystemVerilog sequence. Unknown vendor extensions require an
explicit PeakRDL import mapping; cpptb cannot preserve properties the importer
does not expose.

## Native RgGen input

An existing RgGen YAML, JSON, or TOML contract can be consumed directly. No
IP-XACT conversion or user-authored cpptb mapping file is required:

```sh
uv run --frozen cpptb-rggen rtl/uart_csr.yml \
  -o build/generated/uart_regs.hpp \
  --namespace uart_regs \
  --class-name UartRegisters
```

The command accepts RgGen's mapping-style and sequence-style metadata. It
preserves register blocks, register files, arrays, field sequences, offsets,
resets, common built-in access/trigger field types, and field-qualified
diagnostics. Select one block from a multi-block input with `--block NAME`.
The output filename, C++ namespace, and model class are independent, exactly as
they are for the PeakRDL exporter.

Optional `cpptb_hdl_path` properties on a register, field, or external memory
enable generated hierarchy backdoors. This is a cpptb extension because native
RgGen metadata does not define a portable C++ simulator-binding property.
Omitting it produces a frontdoor-only model without backdoor wrapper code.

Run the installed command for its complete versioned interface:

```sh
uv run --frozen cpptb-rggen --help
```

| Argument | Purpose |
|---|---|
| `INPUT` | One native `.yaml`, `.yml`, `.json`, or `.toml` RgGen file |
| `-o OUTPUT` | Required generated header path and filename |
| `--block NAME` | Select one register block when the input contains several |
| `--namespace NAMESPACE` | Override the generated C++ namespace |
| `--class-name CLASS_NAME` | Override the generated model class; default `RegModel` |

The direct importer currently rejects indirect registers and plugin-specific
field types it cannot map to standard RAL semantics. The error includes the
complete RgGen logical path. External register arrays without fields are
modeled as register-backed memories; more specialized RgGen plugins should
continue to generate their RTL normally and either provide a standard
SystemRDL/IP-XACT contract or extend the importer explicitly.

## Validation and diagnostics

A generation job should be followed by compilation and at least one model
execution test. The cpptb regression performs all three levels:

1. Generate from native SystemRDL and syntax-check the C++ header.
2. Exercise generated handles against a fake `MemoryMappedMaster` to verify
   addresses, data, statuses, mirrors, and side effects without a simulator.
3. Run the generated model against the pinned secworks AES core, including an
   exact transaction-oracle comparison with its unchanged upstream bench.

Generation failures include the complete SystemRDL or RgGen node path. Current
explicit limitations are:

- a memory frontdoor transfer must be byte-aligned, at most 64 bits, and fit
  the master's integral data type;
- register transfer widths must be byte aligned, divide the logical register
  width, be at most 64 bits, and fit the master's integral data type;
- referenced reset values must elaborate to constants;
- SystemRDL register aliases currently require an explicit
  `RegisterAddressMap` plus `RegisterPredictor::add_alias()` so every address
  view shares one logical mirror; and
- user-defined SystemRDL `ruser`/`wuser` effects require an authored
  `RegisterUserEffectPolicy` to define prediction and update encoding.

These checks happen at generation or first use rather than silently truncating
an access.

## Related documentation

- [Register abstraction layer](../memory-register-models.md) explains desired,
  mirrored, frontdoor, backdoor, update, mirror, and prediction behavior.
- [APB register-file example](../examples/apb-regfile.md) provides the smallest
  source contract and equivalent bus-level workflow.
- [secworks AES register-model oracle](../examples/secworks-aes-regmodel.md)
  is the complete generated-model integration and validates it against an
  unchanged upstream testbench.
- [PeakRDL input processing](https://peakrdl.readthedocs.io/en/latest/processing-input.html)
  documents top-level elaboration and supported inputs.
- [SystemRDL 2.0](https://www.accellera.org/downloads/standards/systemrdl)
  defines the source semantics.
