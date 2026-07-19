# Register abstraction layer

The cpptb register abstraction layer (RAL) is an optional verification
component in `cpptb_vc`. It turns a register contract into typed C++ handles,
tracks expected register state, and sends explicit accesses through a
protocol-neutral master or a user-supplied backdoor.

RAL is not part of the core scheduler or DUT signal API. A test that only needs
`dut.path.signal.get()`, `set()`, clocks, and triggers does not include or pay
for it. The component does not start clocks, reset the DUT, advance time, or
perform a transaction unless the test calls an operation that requires one.

## Architecture at a glance

```text
SystemRDL or IP-XACT
        |
        v
PeakRDL cpptb exporter  --->  generated typed C++ model
                                      |
test sequence  --->  register/field handles
                         |            |
                         |            +--> RegisterBackdoor --> DUT hierarchy
                         +--> MemoryMappedMaster --> APB, AXI-Lite, Wishbone, ...
```

The generated header contains register and field names, addresses, widths,
access policies, reset metadata, and typed handles. It contains no protocol
driver and no simulator binding. The authored test chooses the concrete
`MemoryMappedMaster`, optional `RegisterBackdoor`, base address, clock, reset,
and scheduling policy.

| Authored by the user | Generated | Supplied by `cpptb_vc` |
|---|---|---|
| SystemRDL/IP-XACT contract | Typed block, register, field, and memory members | Register state and access semantics |
| Bus component or custom master | Descriptors and logical paths | Frontdoor and backdoor interfaces |
| Optional hierarchical backdoor | Reset and access-policy metadata | Prediction, update, mirror, and diagnostics |
| Test sequences and checks | Relocatable block construction | Per-register access serialization |

## First model

1. Describe the register block in SystemRDL. The small
   [APB register-file example](examples/apb-regfile.md) uses the contract at
   `examples/apb_regfile/registers.rdl`.
2. Follow [Generate a register model](verification-components/register-generation.md)
   to produce a C++ header under the build directory.
3. Construct the generated model with a `MemoryMappedMaster` and optional
   base address or backdoor.
4. Use the typed members from an ordinary test sequence:

```cpp
#include "generated/registers.hpp"

Task<void> configure_device(Dut dut, TestContext& test) {
    ApbMaster master{ApbBus{dut.clk, dut.psel, dut.penable, dut.pwrite,
                            dut.paddr, dut.pwdata, dut.prdata,
                            dut.pready, dut.pslverr}};
    generated_registers::RegModel<decltype(master)> regs{
        test, master, 0x4000'0000};

    regs.control.enable.set_desired(1);
    regs.control.mode.set_desired(3);
    const auto update = co_await regs.control.update();
    test.require_eq("control update", update.transport.status,
                    MemoryStatus::Okay);

    const auto status = co_await regs.status.read();
    test.require_eq("status read", status.transport.status,
                    MemoryStatus::Okay);
    test.expect_eq("sampled", regs.status.sampled.mirrored(),
                   status.data >> 8u & 0xffu);
}
```

The exact constructor type and member names come from the generated header.
The [APB register-file](examples/apb-regfile.md) provides the smallest authored
contract and bus comparison. The
[secworks AES oracle](examples/secworks-aes-regmodel.md) is the complete
end-to-end generated-model integration against an open-source core.

## Three values to keep straight

The RAL follows the useful distinction in the
[Accellera UVM register layer](https://accellera.org/images/downloads/standards/uvm/uvm_users_guide_1.2.pdf)
between requested state, predicted state, and hardware state. These values are
intentionally named rather than hidden behind an ambiguous `get()`:

| Value | Meaning | Changes when |
|---|---|---|
| Desired | State the test wants writable fields to reach | `set_desired()`, successful prediction, or `reset()` |
| Mirrored | State the model currently predicts is in the DUT | Successful reads/writes, `predict()`, `peek()`/`poke()`, or `reset()` |
| DUT | Actual hardware storage | RTL behavior, frontdoor transactions, or a backdoor operation |

`set_desired()` changes only model state. It performs no bus access. `update()`
does nothing when desired and mirrored values already match; otherwise it
encodes the write needed by each field policy and performs a frontdoor write.
`mirror()` performs a frontdoor read, optionally checks the previous mirror,
then updates prediction. This is the same conceptual separation described by
UVM RAL, but cpptb uses explicit `desired()` and `mirrored()` names.

### Known and unknown model bits

A numeric model value is accompanied by a validity mask. A `1` means that bit
has a known predicted value; a `0` means the model has not established its
value yet:

```cpp
const auto value = regs.status.mirrored();
const auto known = regs.status.mirrored_valid_mask();

test.expect_eq("known reset bits", known & 0x0000'ffffu, 0x0000'ffffu);
test.expect_eq("known value", value & known, expected & known);
```

`desired_valid_mask()` and `mirrored_valid_mask()` are available on both
register and field handles. Field masks are shifted down to the field width, so
an eight-bit field reports `0xff` when every field bit is known.

At construction and after `reset()`, validity comes from the generated
SystemRDL reset mask. A field with no reset is unknown; it is not silently
treated as zero. Successful frontdoor reads, ordinary writes, direct
prediction, and `peek()`/`poke()` establish validity for the bits they predict.
Read and write side effects propagate validity bit by bit. A user-defined
`ruser` or `wuser` effect leaves the resulting bits unknown until a custom
prediction policy is supplied.

`mirror(MirrorCheck::Enabled)` compares only readable, nonvolatile bits that
were valid before the read. The first mirror of a register with no reset learns
the value without reporting a false mismatch. A later mirror checks that learned
prediction. `set_desired()` marks only the selected writable register or field
bits as valid. `update()` rejects a full-register transaction if another
writable bit still has unknown desired state, with a path-qualified diagnostic;
read, predict, or set the complete writable value first.

## Register semantics

Choose an operation by the intent of the test:

| Intent | Operation | DUT access | Model effect |
|---|---|---:|---|
| Send an exact bus write | `co_await reg.write(value)` | Frontdoor write | Predicts write effects after success |
| Read through the bus | `co_await reg.read()` | Frontdoor read | Predicts sampled value and read effects |
| Request a future state | `reg.set_desired(value)` | None | Changes desired writable fields only |
| Apply requested state if needed | `co_await reg.update()` | Conditional frontdoor write | Converges desired and mirrored state |
| Read and compare with prediction | `co_await reg.mirror(check)` | Frontdoor read | Checks then updates the mirror |
| Account for an observed transaction | `reg.predict(value, kind)` | None | Applies direct, read, or write prediction |
| Inspect or deposit through hierarchy | `reg.peek()` / `reg.poke(value)` | Backdoor | Updates the mirror immediately |
| Restore model reset state | `reg.reset()` | None | Restores reset values, validity masks, and write-once state |

Frontdoor operations return a small aggregate response. `data` is the complete
logical register value, `transport` is the last bus response,
`transfers_completed` counts successful transfers, and `failed_address` names
the first failed transfer when present. `okay()` is true only when every
required transfer completed. The test decides whether a timeout or bus error
is fatal, nonfatal, or expected. Model-only operations do not advance
simulation time.

### Compact register and field operations

Assume `regs` was constructed with both a bus master and the
[generated backdoor adapter](#hierarchical-backdoor-access). Each operation
selects its own access path; the model does not have a global "frontdoor mode"
or "backdoor mode."

#### Register frontdoor

`write()` sends the exact register value over the bus. `read()` samples it
through the same bus and returns the complete logical register value:

```cpp
co_await regs.control.write(0x0000'0007);
const auto control = co_await regs.control.read();
test.expect_eq("control", control.data, 0x0000'0007u);
```

Use desired state when the model should encode access policy such as W1C,
W1S, or toggle fields before conditionally writing the register:

```cpp
regs.status.pending.set_desired(0x03);
co_await regs.status.update();
```

#### Field frontdoor

A field can be written and read directly. `write()` applies desired state
through the parent register's `update()` and issues a whole-register bus write
when needed. `read()` performs a whole-register bus read and extracts the named
field:

```cpp
co_await regs.control.enable.write(0);
const auto enable = co_await regs.control.enable.read();
test.expect_eq("enable", enable.data, 0u);
```

Set several fields first and call the parent register's `update()` once to
combine them into one conditional register write:

```cpp
regs.control.enable.set_desired(1);
regs.control.mode.set_desired(generated_registers::mode_e::ACTIVE);
co_await regs.control.update();
```

#### Register and field backdoor

`peek()` and `poke()` inspect or deposit the complete register through its HDL
path without a bus transaction or implicit delay:

```cpp
regs.control.poke(0x0000'0007);
test.expect_eq("control storage", regs.control.peek(), 0x0000'0007u);
```

Field backdoor access is an explicit read-modify-write of the parent register.
This keeps one clear storage operation and preserves adjacent fields:

```cpp
constexpr uint64_t kPendingMask = 0xffull;
auto status = regs.status.peek();
status = (status & ~kPendingMask) | 0x03ull;
regs.status.poke(status);
test.expect_eq("pending mirror", regs.status.pending.mirrored(), 3u);
```

#### Mix frontdoor and backdoor operations

The same model can choose a different path for every operation. This is useful
for configuring through the real bus, checking storage directly, or preparing
an otherwise expensive DUT state before returning to normal bus traffic:

```cpp
co_await regs.control.write(0x0000'0007);                 // Frontdoor setup.
test.expect_eq("stored control", regs.control.peek(), 7u); // Backdoor check.

regs.status.poke(0x0000'0080);                            // Backdoor setup.
const auto status = co_await regs.status.read();           // Frontdoor sample.
test.expect_eq("visible status", status.data, 0x80u);
```

`read()`, `write()`, `update()`, and `mirror()` are frontdoor operations.
`peek()` and `poke()` are backdoor operations. `set_desired()`, `predict()`,
and `reset()` only change model state. Successful frontdoor and backdoor
operations update the same desired and mirrored state, so they may be
interleaved without maintaining separate models.

### Register policy examples

Generated member names stay consistent across access policies. The policy
controls which operations are legal and how a successful access updates the
mirror:

```systemverilog
reg {
    field { sw = rw; hw = r; reset = 0; } enable[0:0];
    field { sw = rw; hw = r; onwrite = woclr; } pending[15:8];
    field { sw = r; hw = w; volatile = true; } level[23:16];
    field { sw = r; hw = w; onread = rclr; } events[31:24];
} status @ 0x00;

reg {
    field { sw = rw1; hw = r; } key[31:0];
} unlock @ 0x04;
```

```cpp
// Ordinary RW state and a W1C field are combined into one bus update.
regs.status.enable.set_desired(1);
regs.status.pending.set_desired(0);
co_await regs.status.update();

// Volatile RO state is sampled but excluded from mirror comparisons.
const auto level = co_await regs.status.level.read();
test.expect("level in range", level.data <= 0xffu);

// A field read also predicts read-clear behavior in the parent register.
const auto events = co_await regs.status.events.read();
test.expect_eq("events cleared", regs.status.events.mirrored(), 0u);

// A second successful write to this rw1 field is rejected with its full path.
co_await regs.unlock.key.write(0x51f1'5eadu);
```

Encoded fields use generated C++ enums, split registers issue ordered
frontdoor transfers, and registers wider than 64 bits use `Bits<Width>`. See
[Typed field enumerations](#typed-field-enumerations),
[Split frontdoor accesses](#split-frontdoor-accesses), and
[Arbitrary-width registers](#arbitrary-width-registers) for those value forms.

### Split frontdoor accesses

`regwidth` is the logical register width and `accesswidth` is the width of each
bus transfer. A 32-bit register with a 16-bit access width therefore performs
two explicit transactions:

```cpp
const auto write = co_await regs.accumulator.write(0x1122'3344);
test.require("complete split write", write.okay());
test.expect_eq("write transfers", write.transfers_completed, 2u);

const auto read = co_await regs.accumulator.read();
test.require("complete split read", read.okay());
test.expect_eq("assembled value", read.data, 0x1122'3344u);
```

For little-endian frontdoors, the low-address transfer carries the least
significant register word. For big-endian frontdoors, it carries the most
significant word. The generated setting is selected once with
`--register-endianness`; every generated descriptor, explicit frontdoor
operation, and passive-predictor transfer uses the same order.

Split operations stop at the first failed transport response. Successful
write chunks are predicted immediately, while unissued or failed chunks retain
their previous value and validity. A partially successful read returns the
assembled successful chunks in `data` and marks them in `valid_mask`; mirror
comparison and prediction cover only those valid chunks. The framework does
not retry, roll back, or insert a delay.

### Arbitrary-width registers

Generated registers wider than 64 bits use `Bits<Width>` and retain the same
frontdoor vocabulary. The logical value is divided into explicit bus-width
transfers; authored code still reads and writes one register value:

```cpp
Bits<256> command;
command.set_bit(3, true);
command.set_bit(192, true);

const auto write = co_await regs.command.write(command);
test.require("complete wide write", write.okay());

const auto read = co_await regs.command.read();
test.expect_eq("wide command", read.data, command);

Bits<17> opcode;
opcode.set_bit(16, true);
regs.command.opcode.set_desired(opcode);
co_await regs.command.update();
```

`WideRegisterHandle<Width, Master>` and its generated typed field handles
support reset, desired and mirrored values, validity masks, read/write effects,
frontdoor `read()`, `write()`, `update()`, and `mirror()`. Reset values and
masks are emitted as complete word arrays, so reset state is not truncated at
64 bits. Each individual frontdoor transfer must still be byte aligned, divide
the register width, be at most 64 bits, and fit the transport's data type.

Generated HDL backdoors, passive prediction, and register-backed memory
elements use the same `Bits<Width>` representation, so no part of the logical
value is truncated to the transport width. A model that contains wide
registers exposes typed `for_each_register()` traversal but does not expose the
homogeneous narrow-only `register_handles()` span.

### Multiple address maps and custom frontdoors

A generated handle has one logical desired/mirrored state but can be accessed
through several named bus views. Construct a `RegisterAddressMap` with the
master and base for that view, then pass it explicitly to the operation:

```cpp
RegisterAddressMap primary{"primary", apb, 0x4000'0000};
RegisterAddressMap debug{"debug", debug_apb, 0x8000'0000};

debug.route(regs.control.descriptor(), 0x40); // Alias in the debug view.
debug.route(regs.buffer.descriptor(), 0x200);

co_await regs.control.write(0x1, primary); // Generated primary offset.
co_await regs.control.write(0x2, debug);   // 0x8000'0040.
co_await regs.buffer.write(4, words, debug);
```

The map may use another instance of the same master type, so primary and debug
traffic can travel through different components. Register and field
`read/write/update/mirror`, narrow or wide memory scalar/bulk operations, and
address conversion accept an explicit map. Omitting it uses the handle's
original master and generated address. Backdoor operations never take a map:
they name physical RTL storage, not a software address view.

An exceptional access procedure can replace the normal master call for one
descriptor. Implement `RegisterFrontdoor<Master>` or
`RegisterMemoryFrontdoor<Master>` and attach it to that map route:

```cpp
class UnlockFrontdoor : public RegisterFrontdoor<Master> {
  public:
    Task<write_response_type> write(
        Master& bus, const RegisterDescriptor& reg,
        write_request_type request) override {
        co_await bus.write(unlock_request());
        co_return co_await bus.write(request);
    }

    Task<read_response_type> read(
        Master& bus, const RegisterDescriptor& reg,
        read_request_type request) override {
        co_return co_await bus.read(request);
    }
};

UnlockFrontdoor unlock;
debug.route(regs.protected_control.descriptor(), 0x80, &unlock);
co_await regs.protected_control.write(value, debug);
```

The custom frontdoor receives the descriptor and fully resolved request, so it
can perform an unlock, indirect-index, mailbox, or other project-specific
procedure without changing the generated handle API. Map names and complete
logical paths are included in routing and overflow diagnostics.

### Typed field enumerations

A SystemRDL field with an `encode` property is generated as a C++ `enum class`
and a typed field handle. Normal testbench code uses the symbolic value:

```systemverilog
enum mode_e {
    IDLE = 3'b000;
    ACTIVE = 3'b011;
    DIAGNOSTIC = 3'b111;
};

field {
    sw = rw;
    encode = mode_e;
} mode[2:0];
```

```cpp
regs.control.mode.set_desired(registers::mode_e::ACTIVE);
co_await regs.control.update();

const auto sampled = co_await regs.control.mode.read();
test.expect_eq("active mode", sampled.data, registers::mode_e::ACTIVE);
```

The generated enum participates in failure-only diagnostic formatting, so a
failed comparison names `mode_e::ACTIVE` instead of printing only `3`.
Hardware can still return a reserved encoding. The typed handle preserves that
underlying value rather than throwing; an unknown value formats numerically.
Use the explicit raw handle for protocol-negative tests or reserved values:

```cpp
co_await regs.control.mode.raw().write(0x5);
const auto reserved = co_await regs.control.mode.raw().read();
test.expect_eq("reserved encoding", reserved.data, 0x5u);
```

`raw()` is an escape hatch, not a second model: typed and raw operations share
the same desired value, mirror, validity masks, access policy, frontdoor, and
lock. Encoded fields are currently limited to 64 bits because C++ enum
underlying types cannot represent a wider value.

### User-defined read and write effects

SystemRDL `ruser` and `wuser` effects deliberately require project policy.
Implement `RegisterUserEffectPolicy` once and pass it to the generated model:

```cpp
class DeviceEffects : public RegisterUserEffectPolicy {
  public:
    bool encode_write(const RegisterUserEffectBitContext& bit) override {
        return bit.previous_valid ? bit.previous != bit.value : bit.value;
    }

    RegisterUserEffectBitResult predict_write(
        const RegisterUserEffectBitContext& bit) override {
        return {.value = bit.previous != bit.value,
                .valid = bit.previous_valid};
    }

    RegisterUserEffectBitResult predict_read(
        const RegisterUserEffectBitContext& bit) override {
        return {.value = !bit.value, .valid = true};
    }

    // Preferred packed overrides for fields up to 64 bits.
    uint64_t encode_write_field(
        const RegisterUserEffectFieldContext& field) override {
        return field.previous ^ field.value;
    }

    RegisterUserEffectFieldResult predict_write_field(
        const RegisterUserEffectFieldContext& field) override {
        return {.value = field.previous ^ field.value,
                .valid_mask = field.previous_valid_mask};
    }

    RegisterUserEffectFieldResult predict_read_field(
        const RegisterUserEffectFieldContext& field) override {
        const uint64_t mask = field.field_descriptor.width == 64
                                  ? ~uint64_t{0}
                                  : (uint64_t{1} <<
                                     field.field_descriptor.width) - 1;
        return {.value = ~field.value & mask, .valid_mask = mask};
    }
};

DeviceEffects effects;
generated_registers::RegModel regs{test, apb, effects, 0x4000'0000};
```

The packed field callbacks are the normal high-throughput path for fields up to
64 bits. Their context contains the complete register and field descriptors,
previous value and validity mask, and requested or sampled value. The default
packed implementations call the bit callbacks, preserving existing policies
and supporting effects that genuinely differ per bit. `encode_write_field()`
controls the bus value used by `update()`. `predict_write_field()` and
`predict_read_field()` control the resulting mirrored value and validity.
Ordinary fields do not invoke the virtual policy. If a generated user effect
has no policy, its affected bits become explicitly unknown instead of silently
assuming hardware behavior.

The model tracks `desired()` and `mirrored()` values and validity, reset values,
volatile fields, read-clear/read-set behavior, write-one/zero set, clear, and
toggle behavior, and write-once access. Concurrent frontdoor accesses to one
register are serialized. `mirror()` compares known, readable, nonvolatile
fields before applying read side effects; write-only fields are neither sampled
nor compared.

`update()` warns when a requested state cannot be reached through a field's
write policy. For example, a write-one-set field cannot clear an already-set
bit. The transaction still occurs, and desired state converges to the predicted
hardware state so repeated `update()` calls do not loop forever.

### Passive bus prediction

Frontdoor methods predict transactions issued through the register handle. A
passive predictor keeps the same model coherent when another processor, DMA
engine, debug port, or testbench component accesses the bus:

```cpp
using Transaction = typename decltype(apb)::transaction_type;

AnalysisPort<Transaction> observed;
ApbMonitor monitor{bus};
RegisterPredictor predictor{test, regs.register_handles()};
regs.set_auto_predict(false);
auto prediction_connection = observed.connect(predictor);

co_await Join{
    monitor.run(observed, expected_transfer_count),
    processor_sequence(dut),
};

test.expect_eq("predicted reads", predictor.reads(), expected_reads);
test.expect_eq("predicted writes", predictor.writes(), expected_writes);
```

`register_handles()` is generated with the model; users do not author an
address table. Construction sorts the handles once and rejects overlapping
address ranges. Each successful observed read or write updates the handle
matching that transfer address with the same read/write-effect and validity
rules as an explicit frontdoor
operation. Failed and unmapped transactions do not change model state and are
available through `failed()` and `unmapped()` counters.

Disable automatic prediction when a passive monitor owns prediction for
traffic issued through these handles; otherwise that frontdoor transaction
would be predicted twice. Leave automatic prediction enabled when the model is
driven directly without a passive predictor.

An alternate address map can feed the same logical mirror:

```cpp
RegisterAddressMap debug{"debug", apb, 0x8000};
debug.route(regs.control.descriptor(), 0x40);
predictor.add_alias(regs.control, debug);
```

### Optional register access coverage

`RegisterAccessCoverage` is an opt-in analysis subscriber. Connect the same
passive bus monitor used by prediction, then snapshot coverage for registers,
fields, and register-backed memories:

```cpp
RegisterAccessCoverage accesses{regs.descriptor(), peripheral_base,
                                "peripheral_access"};
AnalysisPort<Transaction> observed;
auto access_connection = observed.connect(accesses);
// Pass `observed` to the passive bus monitor's run task.

// Raw hierarchy operations are invisible on the bus, so sample them where
// the test intentionally performs them.
regs.status.peek();
accesses.sample_register(regs.status, MemoryOperation::Read,
                         AccessPath::Backdoor);

const auto result = accesses.snapshot();
test.expect_eq("access coverage", result.coverage_percent(), 100.0);
const auto* buffer = result.find("peripheral.buffer");
test.expect("all buffer entries written",
            buffer && buffer->unique_written_indices == regs.buffer.size());
```

Frontdoor transactions are classified by address, byte enable, operation, and
access policy. Failed and unmapped transactions are counted separately.
Backdoor sampling is explicit because a raw hierarchy access emits no bus
transaction. Snapshot allocation occurs only when requested; a testbench that
does not construct `RegisterAccessCoverage` executes no coverage code.

Write byte enables are applied per byte. Disabled bytes preserve their prior
value and validity, including W1C/W1S and toggle fields. Strobe bits outside the
selected register width are ignored and produce a warning containing the full
register path; `invalid_byte_enables()` counts those transactions. The
predictor is synchronous and does not advance simulation time. The monitor owns
sampling phase and publication timing.

A field read is a whole-register frontdoor read followed by field extraction.
It therefore predicts read side effects on readable sibling fields as well:

```cpp
const auto enabled = co_await regs.control.enable.read();
// Any read-clear siblings in regs.control have now been predicted as cleared.
```

### Hierarchical backdoor access

When the SystemRDL contract supplies standard `hdl_path` or
`hdl_path_slice` properties, the exporter emits an optional typed adapter:

```cpp
auto backdoor = peripheral_regs::make_backdoor<decltype(master)>(dut);
peripheral_regs::RegModel regs{test, master, base_address, &backdoor};

regs.control.poke(0xa5a5'5a5a);
test.expect_eq("control storage", regs.control.peek(), 0xa5a5'5a5au);
```

The adapter calls the generated `Dut::cpptb_signal<"path">()` lookup at compile
time. There is no runtime string search, and hierarchy discovery emits only the
`get` and `deposit` operations instantiated by the adapter. A frontdoor-only
test that does not use the adapter adds no backdoor transport hooks.

`peek()` and `poke()` do not add a delay. `poke()` is an HDL deposit, not a
persistent force; RTL may overwrite it on a later evaluation. Both operations
immediately predict the returned or deposited value. Tests that need to
observe subsequent RTL behavior must author the corresponding `ReadOnly{}`,
edge, or `Delay{...}` explicitly.

A register-level path maps the whole register. Field-level slices are assembled
into their logical bit positions. One `hdl_path_slice` entry maps an entire
field; multiple entries must provide exactly one path per field bit in
MSB-to-LSB order. Ambiguous lists fail generation. Missing adapters and
incomplete paths produce diagnostics containing the full logical register
path.

`RegisterBackdoor<Data>` remains replaceable for projects that need a custom
simulator mechanism or nonstandard storage policy.

### Register-backed memories

A generated memory handle uses a logical entry index. In
`regs.buffer.read(7)`, `7` means entry seven; it is not a byte address. The
frontdoor address is:

```text
model base + SystemRDL memory offset + index * element bytes
```

For a 32-bit `buffer @ 0x100` in a model based at `0x4000'0000`, entry `7`
therefore accesses byte address `0x4000'011c`.

#### Entry, offset, and absolute coordinates

The handle names all three useful coordinate systems explicitly:

| Coordinate | Meaning | Scalar method | Chunk method |
|---|---|---|---|
| Entry index | Element number relative to this memory | `read(index)` | `read(first_index, output)` |
| Byte offset | Bytes relative to this memory's effective start | `read_offset(offset)` | `read_offset(offset, output)` |
| Absolute address | Effective frontdoor bus byte address | `read_absolute(address)` | `read_absolute(address, output)` |

`write`, `peek`, and `poke` provide the same entry, offset, and absolute forms.
For the 32-bit memory above, these select the same four entries:

```cpp
std::array<uint32_t, 4> words{};

co_await regs.buffer.read(8, words);
co_await regs.buffer.read_offset(0x20, words);
co_await regs.buffer.read_absolute(0x4000'0120, words);
```

The destination size supplies the chunk length, so no separate count argument
is needed. Offsets and absolute addresses must be aligned to
`element_bytes()`, and the complete chunk must remain inside the generated
memory. Diagnostics include the logical path and offending coordinate.

Useful mapping metadata is available without accessing the DUT:

```cpp
test.expect_eq("memory start", regs.buffer.base_address(), 0x4000'0100u);
test.expect_eq("memory end", regs.buffer.end_address(), 0x4000'0500u);
test.expect_eq("entry bytes", regs.buffer.element_bytes(), 4u);
test.expect_eq("entry", regs.buffer.index_from_absolute(0x4000'0120), 8u);
test.expect_eq("entry", regs.buffer.index_from_offset(0x20), 8u);
```

`end_address()` is exclusive. `contains_absolute(address)` accepts only aligned
entry addresses inside `[base_address(), end_address())`.

These absolute methods remain scoped to the generated memory. They reject an
address outside that memory instead of silently issuing an unrelated bus
transaction. Use the protocol master directly when a sequence intentionally
accesses an arbitrary system address that is not represented by this register
model:

```cpp
const auto response = co_await master.read(
    decltype(master)::read_request_type{absolute_address});
```

#### Scalar frontdoor and backdoor

`Frontdoor` is the default. A test can select `Backdoor` for any individual
operation without constructing a second model:

```cpp
using cpptb::vc::AccessPath;

const auto write = co_await regs.buffer.write(7, 0x1234'5678);
test.require("frontdoor write", write.okay());
test.expect_eq("frontdoor transfers", write.transfers_completed, 1u);

const auto frontdoor = co_await regs.buffer.read(7);
test.require_eq("frontdoor status", frontdoor.transport.status,
                MemoryStatus::Okay);
test.expect_eq("frontdoor value", frontdoor.data, 0x1234'5678u);

co_await regs.buffer.write(7, 0xcafe'babe, AccessPath::Backdoor);
const auto backdoor =
    co_await regs.buffer.read(7, AccessPath::Backdoor);
test.expect_eq("backdoor value", backdoor.data, 0xcafe'babeu);
```

Both paths return `Task` and the same response type. Backdoor operations do not
advance simulation time, but the common asynchronous signature lets a helper
choose its path at runtime:

```cpp
Task<uint32_t> read_word(auto& memory, uint64_t index, AccessPath path) {
    const auto response = co_await memory.read(index, path);
    if (!response.okay()) throw std::runtime_error{"memory read failed"};
    co_return response.data;
}
```

Semantic `read()` and `write()` enforce the SystemRDL `sw` policy on both
paths. A generated read-only memory can be read frontdoor or backdoor but
cannot be written through these methods.

Every semantic coordinate form accepts `AccessPath` as its final optional
argument. Frontdoor remains the default:

```cpp
co_await regs.buffer.write_offset(0x20, words); // Frontdoor.
co_await regs.buffer.write_absolute(
    0x4000'0120, words, AccessPath::Backdoor);
```

#### Raw hierarchy access

`peek()` and `poke()` are synchronous, unconditional HDL operations. They
bypass the `sw` policy and are appropriate for initialization, fault setup,
or inspecting storage software cannot access:

```cpp
regs.boot_rom.poke(12, 0x0000'0013);  // Raw deposit, even when sw = r.
test.expect_eq("patched opcode", regs.boot_rom.peek(12), 0x13u);
```

They do not add a delay, predict a memory mirror, or make the deposit persist.
The test authors any later edge or observation phase explicitly. Direct DUT
hierarchy remains available as the lowest-level escape hatch:

```cpp
dut.u_core.instruction_ram.at(12).deposit(0x0000'0013);
const auto opcode = dut.u_core.instruction_ram.at(12).get();
```

The generated handle adds logical SystemRDL mapping, bounds checks,
access-path selection, and contextual diagnostics around the same storage.

#### Bulk operations

Span operations consume caller-owned storage and do not allocate. They fit
packet buffers, descriptor rings, lookup tables, and parsed firmware words:

```cpp
std::array<uint32_t, 4> descriptor{
    0x1000'0000, 0x0000'0200, 0x0000'0040, 0x0000'0001};

const auto loaded = co_await regs.descriptors.write(
    8, std::span<const uint32_t>{descriptor}, AccessPath::Backdoor);
test.require_eq("descriptor words", loaded.transfers_completed,
                descriptor.size());

std::array<uint32_t, 4> sampled{};
const auto read = co_await regs.descriptors.read(
    8, std::span<uint32_t>{sampled});  // Frontdoor by default.
test.require("descriptor read", read.okay());
test.expect_eq("descriptor", sampled, descriptor);
```

Frontdoor blocks issue ordered scalar bus transfers and stop at the first
failed response. For ordinary memories, `transfers_completed` counts successful
entries because each entry uses one transfer. For wide memories it counts
frontdoor bus transfers, while a backdoor bulk operation counts logical
entries. `failed_index` names the first logical entry that failed:

```cpp
const auto result = co_await regs.packet_ram.write(
    first, std::span<const uint32_t>{words});
if (!result.okay()) {
    test.require_eq("prefix written", result.transfers_completed,
                    *result.failed_index - first);
}
```

Raw bulk `poke(first, span)` and `peek_into(first, span)` use the generated
memory backdoor directly and remain synchronous:

```cpp
regs.coefficients.poke(
    0, std::span<const uint32_t>{coefficients});
regs.coefficients.peek_into(0, std::span<uint32_t>{readback});
```

Raw offset and absolute variants use the same mapping while continuing to
bypass software access policy:

```cpp
regs.coefficients.poke_offset(0x20, coefficients);
regs.coefficients.peek_absolute_into(0x4000'0120, readback);
```

Use `slice(first, count)` when several operations target the same window. The
view stores only a handle pointer, first index, and count; it does not copy or
allocate storage. Scalar indices on the view are relative to the window:

```cpp
auto ring = regs.descriptors.slice(8, descriptor.size());

co_await ring.write(descriptor); // Frontdoor.
co_await ring.read_into(sampled); // Frontdoor.

co_await ring.write(descriptor, AccessPath::Backdoor);
const uint32_t third =
    (co_await ring.read(2, AccessPath::Backdoor)).data; // Entry 10.
```

The span size must equal the slice size. A mismatch or relative index outside
the slice reports the complete logical memory path. The original
`write(first, span)` and `read(first, span)` calls remain the lowest-level bulk
primitives and have identical transport behavior. `read_into(first, span)` is
an equivalent compatibility spelling. `std::array`,
`std::vector`, C arrays, and other contiguous caller-owned storage convert to
the span parameters without copying.

Semantic backdoor `read()`, `write()`, and `read_into()` are also completed
inline. They retain the same `co_await` spelling as frontdoor operations so a
helper can choose its path at runtime, but they do not allocate or schedule a
child coroutine. Frontdoor operations remain ordinary asynchronous bus
transactions.

For a generated one-dimensional HDL memory up to 64 bits wide, a bulk
backdoor operation uses standard DPI packed-vector exports carrying as many as
four adjacent elements per crossing. Longer spans are divided into four-entry
blocks and a final partial block. The generated adapter accounts for nonzero
HDL array bounds, masks subword element widths, performs no allocation, and
adds no simulation delay. The public API above is unchanged; unsupported
shapes use the scalar backdoor fallback.

This batching is generated only when the compiled testbench accesses that
memory with `get` or `deposit`. A design that does not use the memory gets no
wrapper functions or runtime cost.

#### SystemRDL memory contracts

Different element widths and software policies generate the same API using the
master's unsigned integral `data_type`:

```systemverilog
external mem {
    mementries = 256;
    memwidth = 32;
    sw = rw;
    hdl_path_slice = '{"packet_ram"};
} packet_ram @ 0x1000;

external mem {
    mementries = 1024;
    memwidth = 8;
    sw = r;
    hdl_path_slice = '{"boot_rom"};
} boot_rom @ 0x2000;

external mem {
    mementries = 64;
    memwidth = 64;
    sw = rw;
    hdl_path_slice = '{"descriptor_ram"};
} descriptors @ 0x4000;
```

One memory `hdl_path_slice` entry names the complete one-dimensional unpacked
HDL array. Generated mapping accounts for the HDL array's declared low bound,
so logical entry zero maps correctly to an array declared `[15:8]`. A missing
memory path is valid for a frontdoor-only model; selecting `Backdoor` or using
`peek/poke` then reports the complete logical memory path. Multiple slice paths
are rejected because split memory-element backdoors are not yet represented.

ELF, Intel HEX, and binary parsing policy remains outside the handle. Parse a
file into the project's chosen word container, then pass a span to `write()` or
`poke()`. `SparseMemory` is a separate byte-addressed expected memory and is
not an automatic mirror of a generated DUT memory.

## Introspection and traversal

Generated handles expose paths and dimensions directly without a runtime name
lookup. `path()` is the logical SystemRDL path. `hdl_path()` is the elaborated
RTL storage path when the object maps to one HDL object:

```cpp
std::cout << regs.control.path() << '\n';       // peripheral.control
if (const auto path = regs.control.hdl_path()) {
    std::cout << *path << '\n';                 // u_regs.control_q
}

test.expect_eq("field address", regs.control.enable.address(),
               regs.control.address());
test.expect_eq("memory entries", regs.buffer.size(), 256u);
```

`hdl_path()` returns `std::optional<std::string_view>` because a frontdoor-only
object may have no HDL mapping and a split register or field may use several
objects. Use `hdl_slices()` for the complete mapping. Each slice supplies its
HDL path, logical register LSB, and width:

```cpp
for (const auto& slice : regs.status.hdl_slices()) {
    std::cout << slice.hdl_path() << '\n';
    inspect_mapping(slice.register_lsb, slice.width);
}
```

These strings and spans point at generated constant metadata. Calling the
accessors performs no allocation, hierarchy discovery, simulator crossing, or
string search. Available metadata is summarized below:

| Object | Direct metadata |
|---|---|
| Model | `descriptor()` with address-ordered register and memory descriptors |
| Register | `name()`, `path()`, `hdl_path()`, `hdl_slices()`, `address()`, `width()` |
| Field | `name()`, `path()`, `hdl_path()`, `hdl_slices()`, `address()`, `lsb()`, `width()` |
| Memory | `name()`, `path()`, `hdl_path()`, `address(index)`, `size()`, `width()` |
| Memory slice | parent paths plus `first_index()`, `size()`, and first `address()` |

### Complete generated-handle surface

The tables below collect the public vocabulary in one place. Operations named
`read`, `write`, `update`, or `mirror` are semantic model operations. `peek`
and `poke` are raw synchronous hierarchy operations. Metadata and model-state
operations do not access the DUT or advance simulation time.

| Model operation | Purpose |
|---|---|
| `reset_all()` | Restore generated desired and mirrored reset state without driving the DUT |
| `set_auto_predict(enabled)` | Select direct-handle or passive-monitor prediction ownership for every register |
| `update_all()` | Frontdoor-write registers whose desired state differs from the mirror |
| `mirror_all(check)` | Frontdoor-read readable registers and optionally compare their mirrors |
| `for_each_register`, `for_each_memory` | Visit generated handles in deterministic address order |
| `descriptor()` | Inspect allocation-free address-map metadata |
| `RegisterAddressMap` | Select a named master/base, aliases, and optional custom frontdoors |
| `RegisterAccessCoverage` | Opt-in frontdoor/backdoor access coverage and immutable snapshots |

| Register operation | Purpose |
|---|---|
| `read([map])`, `write(value[, map])` | Exact frontdoor transfer through the default or selected map |
| `set_desired(value)`, `update([map])` | Request and conditionally apply writable state |
| `mirror([map,] check)`, `predict(value, kind)` | Check through a selected map or update predicted state |
| `set_auto_predict(enabled)` | Disable handle prediction when a passive predictor owns observed traffic |
| `peek()`, `poke(value)` | Raw register backdoor with immediate prediction |
| `reset()` | Restore generated model reset state only |
| `desired()`, `mirrored()`, validity masks | Inspect model state |
| `name()`, `path()`, `hdl_path()`, `hdl_slices()`, `address()`, `width()` | Inspect mapping metadata |

| Field operation | Purpose |
|---|---|
| `read([map])`, `write(value[, map])` | Parent-register frontdoor operation with field extraction/policy |
| `set_desired(value)` | Change the field's desired writable bits |
| `desired()`, `mirrored()`, validity masks | Inspect field model state |
| `raw()` | Access the raw value behind a generated enum field |
| `name()`, `path()`, `hdl_path()`, `hdl_slices()`, `address()`, `lsb()`, `width()` | Inspect mapping metadata |

| Memory operation | Entry-relative | Byte-relative | Absolute |
|---|---|---|---|
| Semantic scalar read/write | `read`, `write` | `read_offset`, `write_offset` | `read_absolute`, `write_absolute` |
| Semantic chunk read/write | `read`, `write` | `read_offset`, `write_offset` | `read_absolute`, `write_absolute` |
| Raw scalar read/write | `peek`, `poke` | `peek_offset`, `poke_offset` | `peek_absolute`, `poke_absolute` |
| Raw chunk read/write | `peek_into`, `poke` | `peek_offset_into`, `poke_offset` | `peek_absolute_into`, `poke_absolute` |

`read_into()` remains a descriptive compatibility spelling for entry-relative
chunk reads. `slice(first_index, count)` creates a reusable entry-relative
window. All chunk APIs consume caller-owned contiguous storage and allocate
nothing. Semantic frontdoor operations and `address(index)` also accept a final
`RegisterAddressMap&`; raw `peek/poke` operations always use physical HDL
storage and therefore do not accept a software map.

| Memory utility | Purpose |
|---|---|
| `address(index)` | Convert an entry index to its effective bus byte address |
| `base_address()`, `end_address()` | Return the memory's inclusive start and exclusive end addresses |
| `element_bytes()`, `size()`, `width()` | Report element stride, entry count, and element width |
| `index_from_offset(offset)` | Convert an aligned byte offset to a checked entry index |
| `index_from_absolute(address)` | Convert an aligned in-range bus address to an entry index |
| `contains_absolute(address)` | Test whether an aligned address names an entry in this memory |
| `slice(first_index, count)` | Create a reusable checked entry-relative window |
| `name()`, `path()`, `hdl_path()` | Inspect generated logical and HDL mapping metadata |

Typed traversal keeps generated members and enum field types intact:

```cpp
regs.control.for_each_field([&](const auto& field) {
    inspect(field.path(), field.desired(), field.mirrored());
});

regs.for_each_register([&](const auto& reg) {
    inspect(reg.path(), reg.address(), reg.width());
});

regs.for_each_memory([&](const auto& memory) {
    inspect(memory.path(), memory.size(), memory.width());
});
```

The traversal callbacks are synchronous and are intended for metadata,
prediction, reset, diagnostics, and other model-only operations. Frontdoor bus
access remains explicitly asynchronous. Use `update_all()` or `mirror_all()`
for address-ordered whole-model traffic, or write an ordinary coroutine when a
test needs custom ordering, filtering, or error policy.

Register arrays retain concrete generated types through compile-time indexing.
`for_each()` visits every element; `for_each_slice<First, Count>()` visits a
compile-time subrange without constructing a runtime container:

```cpp
regs.lane_control.for_each([](auto& reg) { reg.reset(); });
regs.lane_control.for_each_slice<2, 4>([](const auto& reg) {
    inspect(reg.path(), reg.address());
});
```

There is intentionally no bulk `read(span)` across arbitrary registers. Unlike
memory entries, adjacent registers may have different widths, access effects,
volatility, and transport failure policy. Explicit register traffic and the
policy-aware whole-model operations preserve those semantics.

Generated register-file hierarchy follows the SystemRDL structure:

```cpp
regs.security.key.key.set_desired(0x1234);
regs.bank.at<1>().control.value.set_desired(0xa55a);

regs.lane_control.for_each([](auto& reg) { reg.reset(); });
regs.for_each_register([](auto& reg) {
    // Ascending-address traversal across every generated register.
    inspect(reg.descriptor());
});
```

`at<Index>()` is compile-time indexed so each element retains its concrete
generated type and named fields. Existing flattened nested members remain
available for source compatibility, but hierarchical names are the primary
API.

Generated blocks also provide address-ordered whole-model operations:

```cpp
regs.reset_all();
co_await regs.update_all();
co_await regs.mirror_all(MirrorCheck::Enabled);
```

`reset_all()` changes only model state. It does not drive a DUT reset or advance
simulation time. `update_all()` issues only the writes required by each
register's desired state. `mirror_all()` skips write-only registers and checks
all other registers sequentially in ascending address order. The sequential
ordering is intentional: it is deterministic and does not assume that the
frontdoor can accept concurrent transfers. Reusable reset-check, access, and
bit-bash policies are provided by the optional
[standard register sequences](verification-components/register-sequences.md)
layer. They use generated asynchronous traversal without adding policy to the
core register handles.

Logical register values may be arbitrary width, while each `accesswidth`
transfer must be byte aligned, divide `regwidth`, be no more than 64 bits, and
fit the master's unsigned integral bus-data type. Invalid combinations produce
a path-qualified diagnostic when the handle is used. Generated wide HDL
backdoors, passive prediction, and register-backed memories preserve the
complete `Bits<Width>` value; sparse byte memory itself also has no 64-bit
width limit.

## Generate from SystemRDL, IP-XACT, or RgGen

The complete [generation guide](verification-components/register-generation.md)
explains the source/build/generated-file boundary, SystemRDL naming, IP-XACT
import, reproducible build dependencies, validation, and current limitations.
The shortest native SystemRDL flow is:

Install the optional exporter and generate from SystemRDL:

```sh
uv sync --extra peakrdl
uv run --extra peakrdl peakrdl cpptb registers.rdl \
  -o build/generated/registers.hpp \
  --namespace generated_registers
```

The exporter is a normal PeakRDL plugin. The input may instead be an IP-XACT
component:

```sh
uv run --extra peakrdl peakrdl cpptb component.xml \
  -o build/generated/registers.hpp
```

Native RgGen YAML, JSON, and TOML use the direct importer and do not require an
IP-XACT conversion:

```sh
uv run --frozen cpptb-rggen uart_csr.yml \
  -o build/generated/uart_regs.hpp --namespace uart_regs
```

Generated models expose typed registers, named fields, memories, complete
logical paths, `regwidth` and `accesswidth`, access policies, reset metadata,
and a relocatable base. Generated headers carry a tool-version banner and use
fully-qualified framework names rather than importing names into the user's
namespace:

```cpp
#include "generated/registers.hpp"

generated_registers::RegModel<Master> regs{
    test, master, 0x4000'0000, &backdoor};

regs.control.enable.set_desired(1);
regs.control.mode.set_desired(3);
co_await regs.control.update();
```

SystemRDL is the lossless semantic source. IP-XACT import follows PeakRDL's
documented best-effort property mapping. PeakRDL's current IP-XACT exporter
also warns and drops nested SystemRDL `mem` nodes, so retain the original RDL
when that conversion would lose memory declarations. The regression suite
compiles a native SystemRDL model, round-trips its supported register contract
through IP-XACT, syntax-checks the generated C++, and executes a generated
model against a fake frontdoor master. Unsupported reset references and widths
produce diagnostics containing the complete SystemRDL node path.

The runnable APB example includes `examples/apb_regfile/registers.rdl`
as a small real contract.

The [secworks AES register-model oracle](examples/secworks-aes-regmodel.md)
provides the larger ground-truth workflow. It generates named handles from a
SystemRDL description, programs pinned open-source RTL, and requires its full
720-event register-bus transcript and all result words to match the unchanged
upstream top-level testbench.

## One register workflow in four frameworks

These tabs compare authored verification code, not hidden bus or generator
plumbing. The [APB register-file example](examples/apb-regfile.md) supplies the
shared contract and runnable bus sequence; generated-handle behavior is covered
by the code-generation regression and the complete
[secworks AES oracle](examples/secworks-aes-regmodel.md). Cocotb core does not
include a register abstraction layer, so its model-oriented tabs use an
asynchronous package generated by PeakRDL-python. The UVM tabs use the native
UVM register abstraction layer.

Selecting a framework in one tab group selects it in all of the groups below.

### Frontdoor read and write

Each version writes register zero through APB, reads it back, and checks the
returned value or status.

<div class="cpptb-code-tabs" data-tabs="4" data-tab-group="register-model-comparison" data-tab-label="Frontdoor register access"></div>

<div class="cpptb-code-tab-label">cpptb-vc (C++ DPI)</div>

```cpp
const auto write = co_await regs.register_0.write(0x1234'5678);
test.expect_eq("register write", write.transport.status, MemoryStatus::Okay);

const auto read = co_await regs.register_0.read();
test.expect_eq("register read", read.transport.status, MemoryStatus::Okay);
test.expect_eq("register value", read.data, 0x1234'5678u);
```

<div class="cpptb-code-tab-label">Cocotb</div>

```python
await regs.register_0.write(0x1234_5678)
actual = await regs.register_0.read()
assert actual == 0x1234_5678
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
logic [31:0] actual;
logic error;
int unsigned wait_cycles;

apb_write_word(8'h00, 32'h1234_5678, error, wait_cycles);
expect_eq("register write", error, 0);

apb_read_word(8'h00, actual, error, wait_cycles);
expect_eq("register read", error, 0);
expect_eq("register value", actual, 32'h1234_5678);
```

<div class="cpptb-code-tab-label">UVM RAL</div>

```systemverilog
uvm_status_e status;
uvm_reg_data_t actual;

regs.register_0.write(status, 'h1234_5678, .path(UVM_FRONTDOOR));
`uvm_info("REG", $sformatf("write status=%s", status.name()), UVM_LOW)

regs.register_0.read(status, actual, .path(UVM_FRONTDOOR));
if (status != UVM_IS_OK || actual != 'h1234_5678)
  `uvm_error("REG", $sformatf("read status=%s value=0x%0h",
                              status.name(), actual))
```

### Desired state, update, and mirror

cpptb and UVM retain desired and mirrored state. Cocotb and pure SV show the
small amount of explicit shadow bookkeeping needed when no RAL supplies it.

<div class="cpptb-code-tabs" data-tabs="4" data-tab-group="register-model-comparison" data-tab-label="Desired state and mirror checking"></div>

<div class="cpptb-code-tab-label">cpptb-vc (C++ DPI)</div>

```cpp
regs.register_0.value.set_desired(0x0000'002a);
if (regs.register_0.needs_update()) {
    co_await regs.register_0.update();
}

co_await regs.register_0.mirror(MirrorCheck::Enabled);
test.expect_eq("desired converged", regs.register_0.desired(),
               regs.register_0.mirrored());
```

<div class="cpptb-code-tab-label">Cocotb</div>

```python
desired = 0x2A
mirrored = 0

if desired != mirrored:
    await regs.register_0.write(desired)
    mirrored = desired

actual = await regs.register_0.read()
assert actual == mirrored
mirrored = actual
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
logic [31:0] desired = 32'h2a;
logic [31:0] mirrored = 32'h0;

if (desired != mirrored) begin
  apb_write_word(8'h00, desired, error, wait_cycles);
  expect_eq("update status", error, 0);
  mirrored = desired;
end

apb_read_word(8'h00, actual, error, wait_cycles);
expect_eq("mirror status", error, 0);
expect_eq("mirror value", actual, mirrored);
mirrored = actual;
```

<div class="cpptb-code-tab-label">UVM RAL</div>

```systemverilog
uvm_status_e status;

regs.register_0.value.set('h2a);
if (regs.register_0.needs_update())
  regs.register_0.update(status, .path(UVM_FRONTDOOR));

regs.register_0.mirror(status, UVM_CHECK, UVM_FRONTDOOR);
if (status != UVM_IS_OK)
  `uvm_error("REG", "register mirror failed")
```

### Write and read side effects

This richer contract assumes `status.pending` is write-one-to-clear and
`events.cause` is read-clear. The cpptb implementation is covered by the
register-model unit regression even though the small APB example uses ordinary
read/write fields.

<div class="cpptb-code-tabs" data-tabs="4" data-tab-group="register-model-comparison" data-tab-label="W1C and read-clear behavior"></div>

<div class="cpptb-code-tab-label">cpptb-vc (C++ DPI)</div>

```cpp
// Hardware currently reports pending bits 0 and 2.
regs.status.predict(0x05, RegisterPrediction::Direct);
regs.status.pending.set_desired(0x04);
co_await regs.status.update();  // Encodes a bus write of 0x01.

const auto cause = co_await regs.events.cause.read();
test.expect_eq("event cause", cause.data, 0x03u);
test.expect_eq("read-clear mirror", regs.events.cause.mirrored(), 0u);
```

<div class="cpptb-code-tab-label">Cocotb</div>

```python
# PeakRDL-python exposes the field policy, while hardware applies the effect.
await regs.status.pending.write(0x01)  # Clear pending bit zero.

cause = await regs.events.cause.read()
assert cause == 0x03
assert await regs.events.cause.read() == 0  # Read-clear took effect.
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
// W1C: writing one clears that bit; zero leaves other bits unchanged.
apb_write_word(STATUS_ADDR, 32'h0000_0001, error, wait_cycles);
expect_eq("W1C write", error, 0);

apb_read_word(EVENTS_ADDR, actual, error, wait_cycles);
expect_eq("event cause", actual, 32'h0000_0003);
apb_read_word(EVENTS_ADDR, actual, error, wait_cycles);
expect_eq("read-clear", actual, 0);
```

<div class="cpptb-code-tab-label">UVM RAL</div>

```systemverilog
uvm_status_e status;
uvm_reg_data_t cause;

regs.status.predict('h05, .kind(UVM_PREDICT_DIRECT));
regs.status.pending.write(status, 'h01, .path(UVM_FRONTDOOR));

regs.events.cause.read(status, cause, .path(UVM_FRONTDOOR));
if (cause != 'h03 || regs.events.cause.get_mirrored_value() != 0)
  `uvm_error("REG", "read-clear prediction failed")
```

### Backdoor access

Backdoor operations bypass APB timing. A generated cpptb `RegisterBackdoor` adapter and
UVM HDL path retain model state. Cocotb directly deposits through a simulator
handle; that deposit may be overwritten by the RTL on its next assignment.
Pure SV uses standard hierarchical `force` and `release`, whose persistent
force semantics are intentionally visible in the authored code.

<div class="cpptb-code-tabs" data-tabs="4" data-tab-group="register-model-comparison" data-tab-label="Backdoor register access"></div>

<div class="cpptb-code-tab-label">cpptb-vc (C++ DPI)</div>

```cpp
auto backdoor = generated_regs::make_backdoor<decltype(master)>(dut);
generated_regs::RegModel regs{test, master, 0, &backdoor};

regs.register_0.poke(0xa5a5'5a5a);
const auto actual = regs.register_0.peek();

test.expect_eq("backdoor value", actual, 0xa5a5'5a5au);
test.expect_eq("backdoor mirror", regs.register_0.mirrored(), actual);
```

<div class="cpptb-code-tab-label">Cocotb</div>

```python
from cocotb.triggers import ReadOnly

dut.registers[0].value = 0xA5A5_5A5A  # Simulator deposit.
await ReadOnly()
actual = int(dut.registers[0].value)
assert actual == 0xA5A5_5A5A
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
force i_dut.registers[0] = 32'ha5a5_5a5a;
expect_eq("forced value", i_dut.registers[0], 32'ha5a5_5a5a);
release i_dut.registers[0];
```

<div class="cpptb-code-tab-label">UVM RAL</div>

```systemverilog
uvm_status_e status;
uvm_reg_data_t actual;

regs.register_0.poke(status, 'ha5a5_5a5a);
regs.register_0.peek(status, actual);
if (status != UVM_IS_OK || actual != 'ha5a5_5a5a)
  `uvm_error("REG", "backdoor access failed")
```

### Generate and connect the model

SystemRDL can remain the common contract for cpptb, Cocotb, and UVM. Pure SV
can consume generated address constants or use the addresses directly. Driver
or adapter construction is kept outside the generated model in every case.

<div class="cpptb-code-tabs" data-tabs="4" data-tab-group="register-model-comparison" data-tab-label="Register-model generation and construction"></div>

<div class="cpptb-code-tab-label">cpptb-vc (C++ DPI)</div>

```cpp
// Generate first:
// uv run --extra peakrdl peakrdl cpptb registers.rdl
//   -o build/generated/registers.hpp --namespace generated_registers
#include "generated/registers.hpp"

generated_registers::RegModel<Master> regs{
    test, master, 0x4000'0000, &backdoor};
```

<div class="cpptb-code-tab-label">Cocotb</div>

```python
# Generation step: PythonExporter.export(spec, "generated", asyncoutput=True)
from generated.reg_model import RegModel
from generated.lib import AsyncCallbackSet

async def read_register(addr, width, accesswidth):
    response = await apb.read(addr)
    return response.data

async def write_register(addr, width, accesswidth, data):
    await apb.write(addr, data)

regs = RegModel(callbacks=AsyncCallbackSet(
    read_callback=read_register,
    write_callback=write_register,
))
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
package register_map_pkg;
  localparam logic [7:0] REGISTER_0_ADDR = 8'h00;
  localparam logic [7:0] IDENTIFICATION_ADDR = 8'h10;
endpackage

import register_map_pkg::*;
apb_write_word(REGISTER_0_ADDR, value, error, wait_cycles);
```

<div class="cpptb-code-tab-label">UVM RAL</div>

```systemverilog
// Generate first:
// peakrdl uvm registers.rdl -o generated/registers_uvm_pkg.sv
// The generated block type name is selected by the exporter/input model.
apb_registers_block regs;
regs = new("regs");
regs.build();
regs.lock_model();
regs.default_map.set_sequencer(apb.sequencer, reg_adapter);
```

## Relationship to UVM and Cocotb

The model follows the useful UVM RAL behavior without requiring UVM's factory,
configuration database, phase hierarchy, or adapter/predictor class tree:

| Intent | cpptb-vc | UVM RAL |
|---|---|---|
| Literal frontdoor transaction | `co_await reg.write(value)` | `reg.write(status, value)` |
| Read and predict | `co_await reg.read()` | `reg.read(status, value)` |
| Change desired state only | `reg.set_desired(value)` | `reg.set(value)` |
| Update only when needed | `co_await reg.update()` | `reg.update(status)` |
| Check against mirror | `co_await reg.mirror(Enabled)` | `reg.mirror(status, UVM_CHECK)` |
| Raw backdoor access | `reg.peek()` / `reg.poke()` | `reg.peek()` / `reg.poke()` |
| Passive bus prediction | `RegisterPredictor` | `uvm_reg_predictor` |

Cocotb intentionally provides simulator handles and scheduling rather than a
built-in register model. Generated Cocotb ecosystems commonly use callback
sets for frontdoor reads and writes; `MemoryMappedMaster` and
`RegisterBackdoor` are the corresponding replaceable boundaries here.

References:

- [Accellera UVM 1.2 User's Guide](https://accellera.org/images/downloads/standards/uvm/uvm_users_guide_1.2.pdf)
- [SystemRDL standard and downloads](https://www.accellera.org/downloads/standards/systemrdl)
- [PeakRDL input processing](https://peakrdl.readthedocs.io/en/latest/processing-input.html)
- [PeakRDL exporter plugins](https://peakrdl.readthedocs.io/en/latest/for-devs/exporter-plugin.html)
- [PeakRDL IP-XACT import behavior](https://peakrdl-ipxact.readthedocs.io/en/latest/importer.html)
- [PeakRDL Python callback model](https://peakrdl-python.readthedocs.io/en/latest/generated_package.html)
- [PeakRDL UVM generation](https://peakrdl.readthedocs.io/en/latest/gallery.html#create-a-uvm-register-model)
- [Cocotb hierarchical handles](https://docs.cocotb.org/en/stable/writing_testbenches.html#accessing-the-design)
- [UVM register API](https://verificationacademy.com/verification-methodology-reference/uvm/docs_1.2/html/files/reg/uvm_reg-svh.html)

## Performance qualification

The [secworks AES oracle](examples/secworks-aes-regmodel.md) is the scalable RAL
qualification workload. Its generated model and exact pure-SystemVerilog peer
program the same 3,600 AES cases, and its one-suite run must match the unchanged
upstream bench's complete 720-event register trace and checksum.

```sh
make secworks-aes-regmodel-equivalence
make secworks-aes-regmodel-benchmark
```

Controlled decomposition measured the generated register layer at about 2.6%
over direct use of the same master. The larger remaining gap to pure SV comes
from simulator-boundary scheduling rather than lookup or register prediction.
See [Performance](performance.md) and the benchmark's `PROFILE.md` for the
current result and profiling method.

Sparse expected-memory benchmarks are documented separately in
[Sparse expected memory](verification-components/memory-model.md).
