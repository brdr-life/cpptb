# Sparse expected memory

`cpptb_vc::SparseMemory` is a protocol-independent verification component for
expected memory contents, images, byte enables, permissions, and access
callbacks. It is separate from the register abstraction layer: use it when a
monitor or reference model needs scalable byte-addressable storage rather than
desired and mirrored register state.

The model does not drive a bus or advance simulation time. Tests can use it
directly or connect it to passive transactions from APB, AXI-Lite, Wishbone,
or a custom protocol.

## Define regions

Configure only the address regions the test needs. Unwritten bytes retain the
region's fill value without allocating backing storage:

```cpp
SparseMemory memory;
memory.add_region(MemoryRegionConfig{
    .name = "registers",
    .base = 0x0000,
    .size = 0x1000,
    .permission = MemoryPermission::ReadWrite,
    .byte_order = MemoryByteOrder::LittleEndian,
    .fill = 0,
});
memory.add_region(MemoryRegionConfig{
    .name = "boot-rom",
    .base = 0x8000,
    .size = 0x4000,
    .permission = MemoryPermission::Read,
});
memory.load_file(0x8000, "firmware.bin");
```

Regions may be writable, read-only, or inaccessible and may select little- or
big-endian integer interpretation. Overlap and out-of-range accesses produce
explicit statuses.

## Access data

Byte writes, integer word reads and writes, byte enables, `load()`, `fill()`,
`inspect()`, and `dump_file()` are explicit operations. Byte-enable bit `i`
always selects address lane `address + i`, independent of byte order. On a
big-endian 32-bit region, bit zero therefore selects the most-significant byte
of the word.

Use `read_into()` when a hot path already owns storage:

```cpp
std::array<uint8_t, 64> cache_line;
const auto status = memory.read_into(address, cache_line);
test.require_eq("cache-line read", status, MemoryStatus::Okay);
```

Word reads and writes use fixed stack storage internally. Callback-free byte
writes consume caller spans directly. `read_bytes()` remains the convenient
owning API when a returned `std::vector` is useful.

## Add access policy

A replaceable callback can observe or modify an access and can translate its
result before or after storage is touched:

```cpp
class PeripheralPolicy : public MemoryAccessCallback {
  public:
    void after_access(MemoryAccessEvent& access) override {
        if (access.status == MemoryStatus::DecodeError)
            access.status = MemoryStatus::SlaveError;
    }
};
```

Keep timing in the protocol component. The callback is ordinary synchronous
C++ and must not assume that a clock or delay is inserted around it.

## Predict from a passive monitor

Connect the model to any monitor publishing a generic `MemoryTransaction`:

```cpp
SparseMemory memory;
memory.add_region({.name = "ram", .base = 0, .size = 64 * 1024});

auto predictor = make_memory_predictor<Transaction>(
    test, memory, "APB memory transaction");
auto prediction_connection = monitor.observed().connect(predictor);

co_await Join{sequence(master),
              monitor.run(expected_transaction_count)};

test.expect_eq("memory mismatches", predictor.mismatches(), uint64_t{0});
```

The complete `memory_model_apb_test` in
`examples/apb_regfile/testbench.cpp` combines an APB monitor, writable
storage, a read-only image, and translated unmapped-address errors. The same
memory model has no dependency on APB.

## Performance qualification

The exact `memory_model` C++ DPI and pure-SystemVerilog pair runs the same APB
pin sequence, passive prediction, byte-enable updates, checks, and checksum:

```sh
make feature-test FEATURE=memory_model
make feature-benchmark FEATURE=memory_model
```

The bus-free `memory_model_direct` pair isolates sparse storage and checking
without simulated time:

```sh
make feature-test FEATURE=memory_model_direct
make feature-benchmark FEATURE=memory_model_direct
```

Keep both measurements. The direct workload identifies container cost; the
APB workload covers normal monitor and scheduler composition.

See [Register abstraction layer](../memory-register-models.md) when the model
needs named registers, fields, access policy, desired state, and mirrored
prediction rather than general expected byte storage.
