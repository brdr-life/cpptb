# Random value generation

Use `test.random()` when stimulus can be expressed as direct choices rather
than a network of field constraints. It returns the deterministic stream owned
by the currently running test process:

```cpp
Task<void> command_driver(Dut dut, TestContext& test) {
    auto& random = test.random();

    for (uint32_t index = 0; index < 100; ++index) {
        const uint8_t opcode = random.randint<uint8_t>(0, 7);
        const uint16_t address = random.randint<uint16_t>(0x1000, 0x1fff);
        co_await send_command(dut, opcode, address);
    }
}
```

Random calls calculate C++ values immediately. They never suspend the current
coroutine or interact with the DUT.

## API reference

| Operation | Result | Notes |
|---|---|---|
| `randint<T>(minimum, maximum)` | Integral `T` | Both endpoints are inclusive; signed and unsigned types through 64 bits are supported |
| `choice(values)` | One copied range value | Requires a nonempty sized random-access range |
| `weighted_choice(entries)` | One copied entry value | Entries use `weighted(value, weight)`; zero weight is never selected |
| `randbits<Width>()` | `Bits<Width>` | Supports any positive compile-time width |
| `shuffle(values)` | In-place permutation | Uses Fisher-Yates on a sized random-access range |
| `next_u64()` | `uint64_t` | Exposes the raw versioned stream when building another deterministic policy |

Invalid input is reported immediately. Examples include a reversed
`randint()` range, an empty `choice()`, all-zero weights, or a total weight that
overflows `uint64_t`.

## Choices and weights

```cpp
constexpr std::array modes{Mode::Read, Mode::Write, Mode::Flush};
constexpr std::array burst_mix{
    weighted(uint16_t{1}, 8),
    weighted(uint16_t{4}, 4),
    weighted(uint16_t{16}, 1),
};

auto& random = test.random();
const Mode mode = random.choice(modes);
const uint16_t burst_length = random.weighted_choice(burst_mix);
```

Weights are relative. The entries above select lengths 1, 4, and 16 with
probabilities 8/13, 4/13, and 1/13. `weighted_choice()` is value generation;
it is separate from the constrained-transaction `dist()` policy.

## Packed values

`randbits<Width>()` fills every word of a `Bits<Width>` and masks unused bits
in the most significant word:

```cpp
const Bits<17> flags = test.random().randbits<17>();
const Bits<256> payload = test.random().randbits<256>();

dut.flags_i.set(flags);
dut.payload_i.set(payload);
```

For a packed value that participates in constraints with other randomized
fields, use [`RandBits<Width>`](policies-and-composition.md#fixed-arrays-and-packed-values)
instead.

## Shuffle without hidden state

```cpp
std::array<uint8_t, 4> byte_lanes{0, 1, 2, 3};
test.random().shuffle(byte_lanes);

for (const uint8_t lane : byte_lanes) {
    co_await drive_lane(dut, lane);
}
```

The container belongs to the test. The framework stores no copy and adds no
queueing or scheduling behavior.

## Standalone generators

Reference models and ordinary C++ tests can construct a generator directly:

```cpp
Random model_random{0x1234};
const uint16_t tag = model_random.randint<uint16_t>(0, 4095);
```

Two `Random` objects constructed with the same seed and called in the same
order produce the same sequence. Test processes should normally use
`test.random()` so the seed is recorded in the test result and the process
stream remains isolated. See [Seeds, streams, and replay](reproducibility.md).

## When to use a transaction model

Direct generation is clearest when each value is independent or can be derived
in a few lines. Move to a [`Randomized`](constrained-transactions.md) class when:

- several sequences share the same legality rules;
- one field restricts another;
- a directed test needs to add a one-call constraint;
- nonrepeating `RandC` values are useful; or
- named failures would make an illegal model easier to diagnose.
