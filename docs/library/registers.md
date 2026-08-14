<!-- api-headers: include/cpptb_vc/register_model.hpp include/cpptb_vc/register_sequences.hpp include/cpptb_vc/register_coverage.hpp include/cpptb_vc/memory_mapped.hpp tools/codegen/cpptb_codegen/register_codegen.py -->

# Register models

The register abstraction layer's API surface, one entry per name. Everything
lives in `namespace cpptb::vc`; the model itself is generated from a
SystemRDL, IP-XACT, or RgGen contract by
[the register generator](../verification-components/register-generation.md).
[Registers & memory](../memory-register-models.md) covers the semantics —
staged versus mirrored state, access policies, prediction — that these
signatures implement.

Two rules cover timing for the whole page. Register and field frontdoor
operations return `Task` and are always awaited; they serialize on a
per-register lock, so concurrent coroutines access one register in order.
Memory operations return a conditionally awaitable operation type — always
`co_await` them, and a backdoor-path request simply completes without
suspending. Everything else on this page is synchronous and consumes no
simulation time.

## The generated model

The exporter emits one class per address map (default name `RegModel`,
`--class-name` overrides), templated over any
bus master satisfying the `MemoryMappedMaster` concept from
[Verification components](components.md):

```cpp
template <MemoryMappedMaster Master>
class RegModel;

RegModel(TestContext test, Master& master, uint64_t base_address = 0,
         /* optional backdoor and RegisterUserEffectPolicy* */);
```

A frontdoor-only model needs just the first three arguments. Registers are
direct members (`regs.control`), register files nest, and SystemRDL arrays
become compile-time indexed views (`regs.bank.at<1>()`).

### make_backdoor

```cpp
auto backdoor = peripheral_regs::make_backdoor<decltype(master)>(dut);
RegModel<decltype(master)> regs{test, master, base, &backdoor};
```

Builds the generated `DutBackdoor` from `hdl_path` metadata — one object
satisfying every backdoor interface the block needs.

### reset_all

```cpp
void reset_all();
```

Returns every register's staged and mirrored state to its reset value and
validity mask. Model state only; it does not drive a reset signal.

### update_all

```cpp
Task<void> update_all();   // co_await
```

Frontdoor-writes every register whose staged state differs from its mirror,
in ascending address order, skipping the rest.

### mirror_all

```cpp
Task<void> mirror_all(MirrorCheck check = MirrorCheck::Enabled);   // co_await
```

Frontdoor-reads every readable register and refreshes its mirror;
`MirrorCheck::Enabled` also checks each read against the previous mirror.
Write-only registers produce no bus traffic.

### set_auto_predict

```cpp
void set_auto_predict(bool enabled);
```

Fans out to every register handle. Turn it off when a
[`RegisterPredictor`](#registerpredictor) observes the bus, so traffic is
not predicted twice.

### for_each_register

```cpp
template <typename F> void for_each_register(F&& function);
template <typename F> void for_each_memory(F&& function);
template <typename F> Task<void> for_each_register_async(F& function);
```

Typed traversal in deterministic address order; `for_each_field` lives on
each register handle. The async form takes its visitor by **lvalue
reference** — pass a named object, which is how the standard sequences
accumulate their statistics across a traversal.

### register_handles

```cpp
std::span<RegisterHandle<Master>* const> register_handles();
```

The homogeneous handle span — generated only when no register in the block
is wider than 64 bits. It is the constructor input for
[`RegisterPredictor`](#registerpredictor); `for_each_register` remains
available on every block.

## Register handles

`RegisterHandle<Master>` is non-copyable and validated at construction.
`AccessPath` is not a parameter here: frontdoor versus backdoor is the
method name (`read`/`write` versus `peek`/`poke`).

### read and write

```cpp
Task<read_response_type>  read();                 // co_await
Task<write_response_type> write(uint64_t value);  // co_await
```

The exact bus transaction, split into `access_width` transfers when the
register is wider than the bus. Responses carry the register-level view and
the transport's:

```cpp
struct RegisterReadResponse  { data, transport, valid_mask,
                               transfers_completed, failed_address; bool okay(); };
struct RegisterWriteResponse { transport, transfers_completed,
                               failed_address; bool okay(); };
```

`transport.status` is the bus `MemoryStatus`; `failed_address` names the
transfer that failed on a multi-beat register. Every frontdoor operation
also has an overload taking a
[`RegisterAddressMap&`](#registeraddressmap) as its final argument (for
`mirror`, the map comes first: `mirror(map, check)`).

### stage

```cpp
void stage(uint64_t value);
```

Stages a value in the model — no bus access, no simulation time. On a
register with fields only writable fields are staged; UVM's `reg.set()`.

### staged

```cpp
uint64_t staged() const;
uint64_t staged_valid_mask() const;
```

The staged value and which of its bits are known. A field with no reset
value starts unknown rather than silently zero.

### mirrored

```cpp
uint64_t mirrored() const;
uint64_t mirrored_valid_mask() const;
```

What the model predicts the DUT holds, with its validity mask.

### needs_update

```cpp
bool needs_update() const;
```

True when staged and mirrored state differ on a known bit — exactly the
predicate [`update`](#update) applies.

### update

```cpp
Task<write_response_type> update();   // co_await
```

Encodes the write each field's access policy needs (W1C, W1S, toggle, …) to
move the DUT from the mirrored to the staged state, and issues it — or does
nothing, without a bus transaction, when `needs_update()` is false. Throws
`std::logic_error` if writable staged bits are unknown, and warns when a
staged state is unreachable through the field's write effect.

### mirror

```cpp
Task<read_response_type> mirror(MirrorCheck check = MirrorCheck::Enabled);  // co_await
```

Frontdoor read; with checking enabled, compares readable known bits against
the previous mirror through `expect_eq` before updating prediction.

### predict

```cpp
void predict(uint64_t value, RegisterPrediction p = RegisterPrediction::Direct);
```

Informs the model of a value it did not transact itself. `Direct`
overwrites staged and mirrored state; `Read`/`Write` apply the fields' read
and write effects instead. `predict_transfer_read`/`_write` are the
per-bus-beat entry points [`RegisterPredictor`](#registerpredictor) uses.

### peek and poke

```cpp
uint64_t peek();             // immediate backdoor read
void     poke(uint64_t v);   // immediate backdoor deposit
```

Straight to HDL storage through the generated backdoor, no bus, no delay —
and both update the mirror as a `Direct` prediction. They throw when the
model was built without a backdoor; `has_backdoor()` tells you first.

### reset

```cpp
void reset();
```

One register's staged and mirrored state back to reset; the model-wide form
is [`reset_all`](#reset_all).

Metadata rounds out the handle: `name()`, `path()`, `address()`,
`end_address()`, `width()`, `descriptor()`, and the backdoor mapping
(`hdl_path()` when exactly one slice, `hdl_slices()` always).

## Field handles

`RegisterFieldHandle` is a lightweight copyable view onto its parent
register. `stage()`, `staged()`, `mirrored()`, and the validity masks work
as on registers, with values right-justified to the field. `stage()` on a
non-writable field throws.

```cpp
Task<read_response_type>  read();               // whole-register read, field extracted
Task<write_response_type> write(uint64_t v);    // stage + parent update()
```

A field write is a read-modify-write of the enclosing register — sibling
fields are preserved, and a field read predicts read side effects on
readable siblings. There is deliberately no field-level `peek`/`poke`;
backdoor field access is an explicit read-modify-write of the parent.

### raw

Fields with a SystemRDL `encode` are wrapped in an enum-typed handle:
`staged()`, `mirrored()`, `read()`, and `write()` speak the generated
`enum class`, while validity masks stay numeric. `raw()` returns the
underlying numeric field handle for reserved encodings and negative tests.

## Memory handles

`RegisterMemoryHandle<Master>` addresses elements three ways — entry index,
byte offset from the memory's base, and absolute bus address — with scalar
and caller-owned span forms of each:

| Coordinates | Semantic (awaitable) | Raw backdoor (synchronous) |
|---|---|---|
| Entry index | `read(index)`, `write(index, v)`, `read_into(first, span)`, `write(first, span)` | `peek(index)`, `poke(index, v)`, `peek_into(first, span)`, `poke(first, span)` |
| Byte offset | `read_offset(...)`, `write_offset(...)` | `peek_offset(...)`, `poke_offset(...)` |
| Absolute address | `read_absolute(...)`, `write_absolute(...)` | `peek_absolute(...)`, `poke_absolute(...)` |

Semantic operations take a final `AccessPath` (default `Frontdoor`) or a
`RegisterAddressMap&`; their responses echo which path served the request
and report failures by element index. Raw `peek`/`poke` always use physical
HDL storage — memories carry no mirror, so unlike register `peek`/`poke`
there is no prediction side effect. Misaligned offsets and out-of-range
indices throw.

### slice

```cpp
Slice slice(uint64_t first_index, std::size_t size);
```

A non-owning window with slice-relative indexing and the same
semantic/raw operation surface; whole-slice span forms require the span
size to match exactly.

Metadata and conversions: `size()` (entries), `width()`, `element_bytes()`,
`base_address()`, `end_address()`, `address(index)`,
`index_from_offset()`, `index_from_absolute()`, and `contains_absolute()`
— the only conversion that returns `false` instead of throwing.

## Standard sequences

Three reusable whole-model sequences from `register_sequences.hpp`. None
drives reset, starts a clock, or inserts a delay; failures land on the
`TestContext` keyed by register path, and each returns statistics:

```cpp
struct RegisterSequenceResult {
    uint64_t registers_visited, registers_tested, registers_skipped,
             bits_tested, frontdoor_reads, frontdoor_writes,
             backdoor_reads, backdoor_writes;
};
```

### register_reset_check

```cpp
Task<RegisterSequenceResult> register_reset_check(
    const TestContext& test, Model& model,
    RegisterResetCheckOptions options = {});   // {.path = Frontdoor}
```

Reads every register and checks known reset bits that are stable to read.

### register_access_check

```cpp
Task<RegisterSequenceResult> register_access_check(
    const TestContext& test, Model& model);
```

Frontdoor/backdoor agreement in both directions: poke then read, write
then peek, original values restored. Requires a generated backdoor and
throws with the register named when there is none.

### register_bit_bash

```cpp
Task<RegisterSequenceResult> register_bit_bash(
    const TestContext& test, Model& model,
    RegisterBitBashOptions options = {});      // {.path = Frontdoor}
```

Walks ones and zeros through the stably writable, stably readable bits of
each register. Volatile fields and fields with read or write side effects
are excluded conservatively and counted as skipped, not failed.

## Passive prediction

### RegisterPredictor

```cpp
RegisterPredictor predictor{test, regs.register_handles()};
auto connection = monitor.observed().connect(predictor);
regs.set_auto_predict(false);
```

An analysis subscriber that decodes every monitored bus transaction and
updates the owning register's mirror — traffic from any initiator, not just
this model's frontdoor. `add_alias(handle, map)` registers alternate decode
windows; counters report `reads()`, `writes()`, `failed()`, `unmapped()`,
and `invalid_byte_enables()`. Construction validates for null and
overlapping handles. `WideRegisterPredictor<Width, Master>` is the >64-bit
twin.

## Access coverage

### RegisterAccessCoverage

```cpp
RegisterAccessCoverage coverage{regs.descriptor(), base_address};
auto connection = monitor.observed().connect(coverage);   // frontdoor bins
coverage.sample_register(regs.ctrl, MemoryOperation::Write);  // backdoor bins
auto snap = coverage.snapshot();
```

Opt-in access coverage: per register, field, and memory, a read/write ×
frontdoor/backdoor bin matrix, plus unique-index sets for memories. It
subscribes to the same monitor port as a scoreboard for frontdoor traffic;
the `sample_*` overloads (defaulting to `AccessPath::Backdoor`) record what
no bus monitor can see. `snapshot()` is the entire query surface —
`coverage_percent()`, `covered_bins()`, per-entry counters, and
`find(path)`; there is no built-in report printer. A model that never
constructs one executes no coverage code.

## Extension points

### RegisterBackdoor

```cpp
template <std::unsigned_integral Data>
class RegisterBackdoor {
    virtual Data peek(const RegisterDescriptor&, uint64_t address) = 0;
    virtual void poke(const RegisterDescriptor&, uint64_t address, Data value) = 0;
};
```

The abstract base a hand-written backdoor implements; memory, wide, and
wide-memory variants exist with the same shape, and their bulk-span methods
have working defaults over the scalar ones. The generated
[`make_backdoor`](#make_backdoor) adapter implements every applicable base
from `hdl_path` metadata.

### RegisterAddressMap

```cpp
RegisterAddressMap<Master> map{"window", master, window_base};
map.route(regs.control.descriptor(), 0x10);
co_await regs.control.read(map);      // same logical register, other window
```

A named alternate address view: per-descriptor offsets, its own master and
base, and optional custom frontdoors (`RegisterFrontdoor` /
`RegisterMemoryFrontdoor`, one awaitable `read`/`write` pair each) for
indirect or paged access procedures. All views share one logical
staged/mirrored state. Routing is chainable, validated eagerly, and rolled
back on overflow.

### RegisterUserEffectPolicy

```cpp
class RegisterUserEffectPolicy {
    virtual bool encode_write(const RegisterUserEffectBitContext&) = 0;
    virtual RegisterUserEffectBitResult predict_write(const RegisterUserEffectBitContext&) = 0;
    virtual RegisterUserEffectBitResult predict_read(const RegisterUserEffectBitContext&) = 0;
};
```

Defines SystemRDL `ruser`/`wuser` field behavior. Without a policy those
bits become unknown in the mirror; a policy can also declare a bit unknown
after an effect. Field-level batch overrides exist for performance.

## Wide registers

Registers and memories wider than 64 bits use `WideRegisterHandle`,
`WideRegisterFieldHandle`, and `WideRegisterMemoryHandle`: the same surface
with `data_type = Bits<Width>` instead of `uint64_t`, values passed by
const reference, and complete reset values carried as word arrays. Enum
fields and the predictor have wide twins; the model's constructor absorbs
the difference, so testbench code changes only its value types.

## See also

- [Registers & memory](../memory-register-models.md) — the semantics guide
  these signatures implement.
- [Generate a register model](../verification-components/register-generation.md)
  — producing the model from SystemRDL, IP-XACT, or RgGen.
- [Standard sequences](../verification-components/register-sequences.md) —
  the usage guide for the three whole-model checks.
- [Verification components](components.md) — the bus masters, monitors,
  and analysis plumbing a model connects to.
