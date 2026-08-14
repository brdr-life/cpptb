# Sparse expected memory

`cpptb_vc::SparseMemory` is a protocol-independent verification component for
expected memory contents, images, byte enables, permissions, and access
callbacks. It is separate from the register abstraction layer: use it when a
monitor or reference model needs scalable byte-addressable storage rather than
staged and mirrored register state.

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
big-endian integer interpretation. An access that is not fully contained in
one mapped region — an unmapped address or a range straddling a region
boundary — returns `MemoryStatus::DecodeError`, and a read or write that the
containing region's permission forbids returns `MemoryStatus::SlaveError`.
`add_region()` rejects a region that would overlap an existing one by throwing
rather than by status.

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

Attach the policy with the callback constructor or `set_callback()`; passing
`nullptr` detaches it:

```cpp
PeripheralPolicy policy;
SparseMemory memory{policy};   // Attach at construction...
memory.set_callback(&policy);  // ...or attach, replace, or detach later.
```

`before_access()` runs before the region and permission checks and before any
storage is touched. It can rewrite a write's data and byte-enable lanes, and
setting a non-`Okay` status vetoes the access: the built-in checks and the
storage update are skipped and that status is returned. `after_access()` runs
once the checks and any storage update are complete — on a read, `data` then
holds the returned bytes — so it is the place to inspect a finished access or
translate its final status, as `PeripheralPolicy` does above.

Both hooks receive one `MemoryAccessEvent` containing:

- `operation` — `MemoryOperation::Read` or `MemoryOperation::Write`;
- `address` — the first accessed byte address;
- `data` — a mutable span holding the write payload or, after a read, the
  returned bytes;
- `byte_enable` — one entry per data byte on writes, where nonzero enables
  the lane; empty on reads;
- `status` — the access result, writable from either hook; and
- `region` — the containing region's name, empty when no single region
  contains the access.

The callback observes the `read_bytes()`, `read_into()`, `read_word()`, and
write operations. The direct `load()`, `fill()`, `inspect()`, and
`dump_file()` maintenance operations bypass it.

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

The complete `memory_model_apb_test` in the
[APB register-file example](../examples/apb-regfile.md)'s
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

## Related APIs

- [Library reference: verification components](../library/components.md) lists
  the `SparseMemory` and `MemoryPredictor` signatures and which operations
  take simulation time.
- [Verification components](../verification-components.md) introduces the
  `AnalysisPort` fan-out and the passive monitors that feed a predictor.
- [Register abstraction layer](../memory-register-models.md) provides named
  registers, fields, access policy, staged state, and mirrored prediction
  when the model needs more than general expected byte storage.
