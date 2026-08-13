# Constrained transactions

A constrained transaction is an ordinary C++ class derived from `Randomized`.
Its `Rand<T>` fields register with the object, and its constructor records named
constraints once:

```cpp
enum class Opcode : uint8_t { Read, Write, Atomic, Reserved };

class Packet final : public Randomized {
  public:
    Rand<Opcode> opcode{*this, "opcode"};
    Rand<uint16_t> length{*this, "length"};
    Rand<uint16_t> address{*this, "address", 0x1000, 0x1fff};

    Packet() {
        constraint("supported opcode", opcode < Opcode::Reserved);
        constraint("packet length",
                   length >= uint16_t{64} && length <= uint16_t{1500});
        constraint("word length", length % uint16_t{4} == 0);
        constraint("aligned address", address % uint16_t{4} == 0);
        constraint("short atomic packet",
                   opcode != Opcode::Atomic || length <= uint16_t{256});
    }
};
```

The optional minimum and maximum arguments on `Rand<T>` narrow the declared
field domain before constraint solving. Integral and enum fields through 64
bits are supported.

## Supported expressions

Constraints form a backend-neutral expression tree rather than executing as
ordinary booleans during construction.

| Category | Operators |
|---|---|
| Arithmetic | `+`, `-`, `*`, `%` |
| Comparison | `==`, `!=`, `<`, `<=`, `>`, `>=` |
| Logic | `&&`, `||`, `!` |
| Membership | `inside()` with values or ranges |

Use typed constants where narrowing would otherwise be ambiguous. Division,
bitwise expressions, implication syntax, and arbitrary C++ function calls are
not currently constraint operators. Calculate those values outside the model
or express the relationship with the supported primitives.

## Randomize in a test

```cpp
Task<void> packet_sequence(Dut dut, TestContext& test) {
    Packet packet;

    for (uint32_t index = 0; index < 1000; ++index) {
        test.randomize(packet);

        co_await RisingEdge{dut.clk_i};
        dut.opcode_i.set(static_cast<uint8_t>(packet.opcode.get()));
        dut.length_i.set(packet.length.get());
        dut.address_i.set(packet.address.get());
    }
}
```

`test.randomize(packet)` selects the current process's stream and configured
constraint backend. If solving fails, the current test stops cleanly with the
call site, backend name, and backend diagnostic. Successful randomization does
not increment the test check count.

Values remain explicit. `get()` reads the latest solved value, while assigning
to a `Rand<T>` only updates its stored C++ value:

```cpp
packet.length = uint16_t{256};
const uint16_t length = packet.length.get();
```

That assignment does not constrain the next solve. Use an inline constraint
when the value must participate in randomization.

## Inline constraints

`randomize_with()` adds one hard constraint for a single call:

```cpp
test.randomize_with(packet, packet.length == uint16_t{256});
test.randomize_with(packet,
                    packet.opcode == Opcode::Read &&
                    packet.address < uint16_t{0x1100});
```

The class constraints remain unchanged. This is useful for directed corner
cases without creating transaction subclasses or mode flags.

## Pre and post hooks

Override hooks only for transaction-local preparation or derived values:

```cpp
class ChecksummedPacket final : public Randomized {
  public:
    Rand<uint16_t> payload{*this, "payload"};
    uint8_t checksum = 0;

    void pre_randomize() override {
        checksum = 0;
    }

    void post_randomize() override {
        checksum = static_cast<uint8_t>(payload.get() ^ (payload.get() >> 8));
    }
};
```

`pre_randomize()` runs before every solve attempt. `post_randomize()` runs only
after a successful assignment. Hooks do not have scheduler access by default
and should not hide DUT writes or timing.

## Nonrepeating RandC fields

`RandC<T>` visits every value in its effective finite domain before beginning
another cycle:

```cpp
class TaggedRequest final : public Randomized {
  public:
    RandC<uint8_t> tag{*this, "tag", 0, 15};
};
```

Hard constraints narrow the cycle. For example, an `inside(tag, {1, 3})`
constraint produces both values once before either repeats. Multiple `RandC`
fields maintain independent cycles; exhausting a short cycle does not reset a
longer one.

## Inspect results outside TestContext

Plain C++ code can choose how to handle a solve failure:

```cpp
Random random{0x1234};
Packet packet;

const RandomizeResult result = packet.randomize(random);
if (!result) {
    std::cerr << result.message << '\n';
}
```

`RandomizeResult::status` distinguishes solved, proven unsatisfiable, search
exhaustion, cycle exhaustion, and backend failure. See
[Solvers and diagnostics](solvers-and-diagnostics.md) for the distinction and
backend selection guidance.
