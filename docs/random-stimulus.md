# Randomization

cpptb separates random value generation from constrained transaction solving.
Both use deterministic, test-owned random streams, but they solve different
authoring problems:

| Need | Use |
|---|---|
| Compare complete cpptb and pure-SV authoring styles | [Side-by-side examples](randomization/examples.md) |
| Pick values, shuffle data, or generate packed bits | [`test.random()`](randomization/value-generation.md) |
| Describe legal relationships between transaction fields | [`Randomized`](randomization/constrained-transactions.md) |
| Add membership, weighting, defaults, or runtime modes | [Policies and composite fields](randomization/policies-and-composition.md) |
| Prove coupled constraints or diagnose an unsatisfiable model | [Solver backends](randomization/solvers-and-diagnostics.md) |
| Measure exercised behavior with explicit sampling | [Functional coverage](randomization/functional-coverage.md) |
| Reproduce failures across concurrent processes | [Seeds and replay](randomization/reproducibility.md) |

Randomization is stimulus construction only. It does not drive a signal,
start a clock, wait for an edge, or advance simulation time.

## Start with values

For most sequences, ordinary value generation is the smallest useful API:

```cpp
enum : uint8_t { Read, Write, Flush, Fence };

Task<void> packet_sequence(Dut dut, TestContext& test) {
    auto& random = test.random();
    constexpr std::array<uint8_t, 4> opcodes{Read, Write, Flush, Fence};
    constexpr std::array length_mix{
        weighted(64u, 5), weighted(256u, 3), weighted(1500u, 1)};

    for (uint32_t transaction = 0; transaction < 1000; ++transaction) {
        const uint32_t address =
            random.randint<uint32_t>(0x1000, 0x1fff);
        const uint8_t opcode = random.choice(opcodes);
        const uint32_t length = random.weighted_choice(length_mix);
        const Bits<256> payload = random.randbits<256>();

        co_await drive_packet(dut, address, opcode, length, payload);
    }
}
```

Use this style when values can be generated directly and legality is easy to
see in the sequence. See [Random value generation](randomization/value-generation.md)
for the complete API.

## Model related fields

Use a randomized transaction when legality spans multiple fields or is reused
by several sequences:

```cpp
enum class Opcode : uint8_t { Read, Write, Atomic, Reserved };

class Packet final : public Randomized {
  public:
    Rand<Opcode> opcode{*this, "opcode"};
    Rand<uint16_t> length{*this, "length"};
    Rand<uint16_t> address{*this, "address", 0x1000, 0x1fff};
    RandC<uint8_t> tag{*this, "tag", 0, 15};

    Packet() {
        constraint("supported opcode", opcode < Opcode::Reserved);
        constraint("legal length",
                   length >= uint16_t{64} && length <= uint16_t{1500});
        constraint("word-sized packet", length % uint16_t{4} == 0);
        constraint("aligned address", address % uint16_t{4} == 0);
        constraint("short atomic packet",
                   opcode != Opcode::Atomic || length <= uint16_t{256});
    }
};

Task<void> packet_sequence(Dut dut, TestContext& test) {
    Packet packet;
    for (uint32_t index = 0; index < 1000; ++index) {
        test.randomize(packet);
        co_await drive_packet(dut, packet.opcode.get(), packet.length.get(),
                              packet.address.get(), packet.tag.get());
    }
}
```

`test.randomize()` uses the current process stream and turns a solve failure
into a source-attributed fatal test failure. The transaction remains an
ordinary C++ object whose values are read explicitly with `get()`.

See [Constrained transactions](randomization/constrained-transactions.md) for
fields, operators, inline constraints, hooks, and `RandC` behavior.

## Feature guide

| Feature | API | Guide |
|---|---|---|
| Runnable framework comparison | Exact cpptb and pure-SV peers | [Examples](randomization/examples.md) |
| Inclusive integral range | `random.randint<T>(minimum, maximum)` | [Value generation](randomization/value-generation.md) |
| Select or shuffle values | `choice()`, `weighted_choice()`, `shuffle()` | [Value generation](randomization/value-generation.md) |
| Arbitrary-width packed value | `random.randbits<Width>()` | [Value generation](randomization/value-generation.md) |
| Scalar randomized field | `Rand<T>` | [Constrained transactions](randomization/constrained-transactions.md) |
| Nonrepeating finite cycle | `RandC<T>` | [Constrained transactions](randomization/constrained-transactions.md) |
| Per-call legality | `test.randomize_with(item, expression)` | [Constrained transactions](randomization/constrained-transactions.md) |
| Values and ranges | `inside()`, `range()` | [Policies and composition](randomization/policies-and-composition.md) |
| Weighted solve policy | `dist()`, `weighted()` | [Policies and composition](randomization/policies-and-composition.md) |
| Preferred default | `soft_constraint()` | [Policies and composition](randomization/policies-and-composition.md) |
| Runtime mode switch | `ConstraintHandle` | [Policies and composition](randomization/policies-and-composition.md) |
| Nested transaction | `Randomized(parent, name)` | [Policies and composition](randomization/policies-and-composition.md) |
| Fixed array or packed field | `RandArray<T, N>`, `RandBits<Width>` | [Policies and composition](randomization/policies-and-composition.md) |
| Adaptive dependency-free solving | `AdaptiveConstraintBackend` | [Solvers and diagnostics](randomization/solvers-and-diagnostics.md) |
| Direct sampling control | `RandomSearchBackend` | [Solvers and diagnostics](randomization/solvers-and-diagnostics.md) |
| Coupled constraint solving | `Z3RandomBackend` | [Solvers and diagnostics](randomization/solvers-and-diagnostics.md) |
| Functional coverage | `Covergroup<T>`, `Coverpoint<T>` | [Functional coverage](randomization/functional-coverage.md) |
| Seed and process replay | `--seed`, `CPPTB_RANDOM_SEED` | [Reproducibility](randomization/reproducibility.md) |

## Design boundaries

The randomization layer is intentionally independent of the simulator and
scheduler. It can be used in a coroutine, reference model, monitor helper, or
plain C++ unit test. Solver dependencies are optional, and selecting another
backend does not change the authored transaction class.

The current composite fields are fixed `RandArray<T, N>` arrays and
`RandBits<Width>` packed values constrained through 32-bit words. Dynamic
random arrays, a complete clone of the SystemVerilog constraint language,
coverage-guided solving, and `solve before` ordering are not currently part of
the API. Functional coverage is deliberately separate from stimulus and does
not steer randomization.

## Performance qualification

Random APIs have exact C++ and pure-SystemVerilog benchmark peers. The pairs
consume the same versioned random stream, drive the same DUT transactions, and
must produce the same response checksum before timing is considered:

```sh
make feature-test FEATURE=random_stimulus
make feature-test FEATURE=constrained_packet
make feature-test FEATURE=constraint_extensions
make feature-test FEATURE=coverage_sampling
```

See [Performance](performance.md#deterministic-random-stimulus) for current
measurements and the `1.10x` C++/pure-SV guard.
