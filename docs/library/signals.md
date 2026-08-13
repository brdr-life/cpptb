# Signals and the Dut

<!-- api-headers: include/cpptb/dpi_static_binding.hpp include/cpptb/packed_bits.hpp include/cpptb/hierarchy.hpp include/cpptb/inout.hpp -->

The operations available on a generated `Dut`'s members — ports, unpacked
arrays, inouts, and the inferred internal hierarchy — and the wide-value
type behind them. [Hierarchy](../hierarchy.md) and
[Interfaces](../interfaces.md) teach the concepts; this page is the
surface.

A generated `Dut` is a plain aggregate of typed, stateless proxy members —
cheap to copy, safe to pass by value. Widths, directions, and array bounds
are compile-time properties, so a misspelled signal, a write to an output,
or `force()` on a port is a *compile* error.

## Ports

### get
### set
### set_now

```cpp
value_type get() const;
void set(value_type value);       // writable ports only; queues (write model)
void set_now(value_type value);   // writable ports only; immediate
```

The `value_type` follows the width: `uint32_t` through 32 bits, `uint64_t`
through 64, `Bits<W>` above. Values are masked to the declared width on
both read and write. Under the default write model, `set()` queues and
flushes at the timestep's ReadWrite point — a write after an awaited edge
lands on the *next* edge — while `set_now()` deposits immediately
(cocotb's `setimmediatevalue()`). A `get()` between a `set()` and its
flush returns the simulator's value, not the queued one, and writing from
`ReadOnly` fails at the offending call site.

One-bit ports convert implicitly to the `Signal` that edge triggers take:
`co_await RisingEdge{dut.clk}` just works.

`force()`/`release()` on any port is a compile error by design — force is
for inferred hierarchical objects only.

### at

```cpp
auto at(int32_t index) const;       // unpacked arrays; also operator[]
```

Unpacked-array ports index by their **declared** SystemVerilog bounds — a
`[1:3]` array indexes 1..3 — one `at()` per dimension, with the final
dimension yielding an element carrying the scalar port surface
(`get`/`set`/`set_now`). Out-of-range indices are a runtime error naming
the array and its declared range.

### drive
### high_z

```cpp
value_type get() const;             // inout ports
void drive(value_type value) const; // enable the testbench driver
void high_z() const;                // disable it
```

`inout` ports replace `set()` with explicit drive intent; both operations
are immediate. See [Interfaces and inouts](../interfaces.md).

## Wide values

### Bits

```cpp
template <std::size_t W> class Bits;    // canonical, zero-extended

static Bits from_uint(uint64_t value);
static Bits from_uint128(uint128_t value);
static Bits from_hex(std::string_view text);   // "0x" and "_" tolerated
static Bits from_words(word_array words);

uint32_t word(size_t index) const;   void set_word(size_t index, uint32_t v);
bool bit(size_t index) const;        void set_bit(size_t index, bool value);
Bits<K> slice(size_t lsb) const;     void set_slice(size_t lsb, const Bits<K>&);
unsigned to_uint() const;            // W ≤ 32
uint64_t to_uint64() const;          // W ≤ 64
uint128_t to_uint128() const;        // W ≤ 128
```

The value type of everything wider than 64 bits. Comparison is `==`/`!=`
only — there are no arithmetic, bitwise, or shift operators; manipulate
through words, bits, and slices. Out-of-range access is a runtime error,
not undefined behavior. `Random::randbits<W>()` produces one directly:
`dut.payload_i.set(test.random().randbits<256>())`.

## Internal hierarchy

### deposit
### force
### release

```cpp
value_type get() const;                    // dut.block.sub.name.get()
void deposit(value_type value) const;      // one blocking assignment, immediate
void force(value_type value) const;        // override drivers; persists
void release() const;                      // variable keeps last forced value;
                                           // net returns to its drivers
```

Hierarchy operations are **all immediate** — none queues under the write
model and none advances time; a `get()` in the same instant observes a
preceding `deposit()`/`force()`. `deposit()` exists only on assignable
objects (variables, not resolved nets); `force()`/`release()` exist on
both. One-bit hierarchy signals convert to `Signal` for edge waits.
Four-state variants (`get_logic`, `deposit_logic`, `force_logic`) and
typed views (`get_as<View>()`, `deposit_as(view)`) carry the same timing.

### Memory

```cpp
Element at(int32_t index) const;           // declared bounds; also operator[]
void get_into(int32_t first_index, std::span<value_type> values) const;
void deposit(int32_t first_index, std::span<const value_type> values) const;
```

Inferred memories index by declared bounds, one `at()` per dimension; each
element carries the hierarchy-signal surface. The span forms move whole
regions in one call — the backdoor path
[memory models](../memory-register-models.md) build on.

## Timing summary

| Operation | Timing |
|---|---|
| `port.set(v)` | Deferred — flushes at the timestep's ReadWrite point |
| `port.set_now(v)` | Immediate |
| `port.get()` | Immediate; never sees your own queued write |
| `inout.drive/high_z` | Immediate |
| Hierarchy `get/deposit/force/release`, memory access | Immediate |

## See also

- [Hierarchy](../hierarchy.md) — what is inferred, what can be forced, and
  the usage-pruned transport contract.
- [Four-state values](../four-state.md) — the `_logic` variants and their
  current limits.
