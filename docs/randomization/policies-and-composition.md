# Policies and composite fields

Hard constraints define legality. Membership, distributions, soft defaults,
and runtime handles layer selection policy onto that legal space. Composite
fields let one transaction model retain the same structure as the protocol it
represents.

## Membership sets and ranges

`inside()` accepts individual values, inclusive `range()` entries, or an
initializer list:

```cpp
class Request final : public Randomized {
  public:
    Rand<uint8_t> opcode{*this, "opcode"};
    Rand<uint16_t> length{*this, "length"};

    Request() {
        constraint("selected opcodes", inside(opcode, {1, 3, 5}));
        constraint("selected lengths",
                   inside(length, uint16_t{64},
                          range(uint16_t{128}, uint16_t{255}),
                          uint16_t{1500}));
    }
};
```

The range type must exactly match the field type. Individual values only need
to be convertible. Membership constraints are hard legality constraints and
can narrow a `RandC` cycle.

## Weighted distributions

`dist()` applies weights after the hard constraints have established the legal
domain:

```cpp
distribution(
    "packet length mix",
    dist(length,
         weighted(uint16_t{64}, 5),
         weighted(range(uint16_t{128}, uint16_t{255}), 3),
         weighted(uint16_t{1500}, 1)));
```

Each weight belongs to the complete entry. The range `128..255` receives total
weight 3, then a legal value inside that selected range is chosen uniformly.
This matches the total-range interpretation of SystemVerilog `:/`, not a
per-value `:=` weight.

Zero-weight entries are excluded. At least one active entry must have positive
weight, and its intersection with the hard domain must be nonempty. Only one
distribution may be active for a field; duplicate active policies return an
actionable backend error instead of being combined implicitly.

Use `random.weighted_choice()` for an immediate independent C++ value. Use
`dist()` when weighting a field that also participates in constraints.

## Soft constraints

A soft constraint supplies a default without weakening hard legality:

```cpp
class RoutedRequest final : public Randomized {
  public:
    Rand<uint8_t> route{*this, "route", 0, 7};

    RoutedRequest() {
        soft_constraint("default route", route == uint8_t{2});
    }
};

RoutedRequest request;
test.randomize(request); // prefers route 2
test.randomize_with(request, request.route == uint8_t{7}); // hard override
```

Soft constraints are considered in declaration order. A soft constraint is
kept when it can coexist with all hard constraints and earlier accepted soft
constraints. A conflicting hard class or inline constraint wins.

## Constraint handles

Every class constraint and distribution returns a `ConstraintHandle`. Retain a
handle when a test needs to change a mode at runtime:

```cpp
class ModeRequest final : public Randomized {
  public:
    Rand<uint8_t> opcode{*this, "opcode"};
    ConstraintHandle legacy_mode;

    ModeRequest() {
        constraint("normal opcodes", inside(opcode, {1, 2, 3}));
        legacy_mode = constraint("legacy opcode", opcode == uint8_t{7});
        legacy_mode.disable();
    }
};

request.legacy_mode.enable();
const bool active = request.legacy_mode.enabled();
request.legacy_mode.set_enabled(false);
```

Handles expose `enable()`, `disable()`, `set_enabled(bool)`, `enabled()`, and
`soft()`. Changing a handle invalidates cached solve policy, but it does not
consume random state, solve the object, drive the DUT, or advance time.

When two hard modes conflict, disable one before enabling the other. The
framework intentionally does not infer a constraint-mode group.

## Nested randomized objects

A child object uses the protected parent constructor. Its fields, constraints,
names, and hooks join the root transaction's single solve:

```cpp
class Header final : public Randomized {
  public:
    Rand<uint8_t> kind{*this, "kind"};
    Rand<uint8_t> route{*this, "route", 0, 7};

    explicit Header(Randomized& parent) : Randomized(parent, "header") {
        constraint("legal kind", inside(kind, {1, 2, 4}));
        soft_constraint("default route", route == uint8_t{2});
    }
};

class Packet final : public Randomized {
  public:
    Header header{*this};
    Rand<uint8_t> priority{*this, "priority", 0, 3};

    Packet() {
        constraint("priority route",
                   priority != uint8_t{3} || header.route == uint8_t{1});
    }
};
```

Randomize `Packet`, not `packet.header`. Direct child randomization returns a
clear backend error because it would omit the owning object's cross-field
constraints. Child fields and diagnostics use qualified names such as
`header.route`.

## Fixed arrays and packed values

`RandArray<T, N>` creates a fixed number of scalar randomized elements.
`RandBits<Width>` creates enough bounded 32-bit randomized words to represent
the packed value:

```cpp
class Payload final : public Randomized {
  public:
    RandArray<uint8_t, 4> bytes{*this, "bytes"};
    RandBits<137> data{*this, "data"};

    Payload() {
        constraint("distinct prefix", bytes[0] != bytes[1]);
        constraint("marker", data.word(4) == uint32_t{0x1ff});
    }
};

Payload payload;
test.randomize(payload);

const std::array<uint8_t, 4> bytes = payload.bytes.get();
const Bits<137> data = payload.data.get();
```

Array elements can participate directly in scalar constraints. Packed
constraints address `word(index)` lanes, with word zero holding the least
significant bits. The top word is automatically bounded to the declared width.
`get()` reconstructs the complete `std::array` or `Bits<Width>` value.

The current API deliberately supports fixed-size arrays. Dynamic random arrays,
whole-value arbitrary-width arithmetic, and variable-size allocation policies
remain future work rather than hidden solver-specific behavior.

## Related APIs

- [Constrained transactions](constrained-transactions.md) covers the hard
  constraints, `Rand` fields, and inline `randomize_with()` calls these
  policies layer onto.
- [Solvers and diagnostics](solvers-and-diagnostics.md) explains how each
  backend applies distributions and soft constraints and lists the
  actionable backend errors.
- [Randomization library reference](../library/randomization.md) lists the
  signatures for `inside()`, `range()`, `dist()`, `weighted()`,
  `ConstraintHandle`, `RandArray`, and `RandBits`.
