# Standard register sequences

Three checks are worth running against almost any register map: that every
register comes out of reset holding its documented value, that the frontdoor
and backdoor paths agree with each other, and that every safe read/write bit
can actually be flipped and observed. Rather than write them once per project,
`cpptb_vc/register_sequences.hpp` provides them as reusable whole-model
sequences — kept out of the core register handles so they stay test policy
rather than model behavior:

```cpp
#include "cpptb_vc/register_sequences.hpp"

co_await reset_dut(dut); // Authored explicitly by the test.
regs.reset_all();        // Model state only; no signal drive or delay.

const auto reset = co_await register_reset_check(test, regs);
const auto access = co_await register_access_check(test, regs);
const auto bash = co_await register_bit_bash(test, regs);
```

None of these functions drives reset, starts a clock, or inserts a delay. Bus
timing comes from the model's existing frontdoor master. Raw backdoor work is
synchronous and uses generated SystemRDL HDL paths.

## Sequence behavior

| Function | What it verifies | Default path |
|---|---|---|
| `register_reset_check` | Stable readable bits with a known generated reset value match the DUT | Frontdoor |
| `register_access_check` | Backdoor deposits are visible through frontdoor reads and safe frontdoor writes are visible through backdoor reads | Mixed |
| `register_bit_bash` | Each safe read/write bit can be inverted, observed, and restored | Frontdoor |

Reset and bit-bash can use the generated backdoor explicitly:

```cpp
const auto reset = co_await register_reset_check(
    test, regs, {.path = AccessPath::Backdoor});

const auto bash = co_await register_bit_bash(
    test, regs, {.path = AccessPath::Backdoor});
```

Requesting a backdoor when one was not generated throws an actionable error
containing both the sequence name and logical register path. It never silently
falls back to the frontdoor.

## Policy and restoration

The default policy is deliberately conservative:

- reset checks skip unknown reset bits, write-only and volatile fields, and
  whole registers containing a readable field with a read side effect;
- access checks compare stable readable bits after a backdoor deposit;
- their reverse direction tests ordinary stable `ReadWrite` fields only;
- bit-bash skips registers containing write-once or write/read side-effect
  policies, and tests only stable ordinary `ReadWrite` bits; and
- access-check and bit-bash restore the original register value before moving
  to the next generated handle.

This is the safe common denominator, not a claim that specialized fields are
untestable. Write ordinary coroutines for interrupt status, FIFOs, indirect
registers, destructive reads, or project-specific effects where the expected
state transition is part of the test.

## Results

Every function returns `RegisterSequenceResult`. Checks are recorded directly
on the owning `TestContext`; the result contains execution detail rather than a
second pass/fail channel:

```cpp
const auto result = co_await register_bit_bash(test, regs);

test.require("bit bash exercised at least one bit", result.bits_tested != 0);
inspect(result.bits_tested, result.registers_skipped,
        result.frontdoor_writes);
```

Available counters are `registers_visited`, `registers_tested`,
`registers_skipped`, `bits_tested`, and frontdoor/backdoor read/write counts.
They are useful for detecting an unexpectedly empty test without forcing a
coverage database on users who do not need one.

## Generated traversal

Generated models expose `for_each_register_async(visitor)` in deterministic
address order. The standard sequences use it internally, and environments can
build a project-specific sequence with the same primitive:

```cpp
struct ReadStableRegisters {
    template <typename Register>
    coro::Task<void> operator()(Register& reg) {
        if (reg.descriptor().fields.empty()) {
            const auto response = co_await reg.read();
            test.expect(reg.path(), response.okay());
        }
    }

    TestContext& test;
};

ReadStableRegisters sequence{test};
co_await regs.for_each_register_async(sequence);
```

The visitor is an ordinary object and can hold filtering, counters, protocol
configuration, or project policy. There is no sequencer, factory, phase
system, or implicit process.

## Equivalent authored flow

The complete runnable benchmark contains the same one-register reset, access,
and bit-bash workload in both implementations.

<div class="cpptb-code-tabs" data-tabs="2" data-tab-group="register-sequences" data-tab-label="Standard register sequence"></div>

<div class="cpptb-code-tab-label">cpptb-vc (C++ DPI)</div>

```cpp
const auto reset_frontdoor = co_await register_reset_check(test, regs);
const auto reset_backdoor = co_await register_reset_check(
    test, regs, {.path = AccessPath::Backdoor});
const auto access = co_await register_access_check(test, regs);
const auto bash_frontdoor = co_await register_bit_bash(test, regs);
const auto bash_backdoor = co_await register_bit_bash(
    test, regs, {.path = AccessPath::Backdoor});
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
check32(storage & 'hff, 'h5a, "reset frontdoor value");
check32(storage & 'hff, 'h5a, "reset backdoor value");

storage = 'haa;
check32(storage & 'hff, 'haa, "access frontdoor read");
storage = 'h55;
check32(storage & 'hff, 'h55, "access backdoor read");

for (int unsigned bit_index = 0; bit_index < 8; bit_index++) begin
  candidate = original ^ (32'h1 << bit_index);
  storage = candidate;
  check32(storage[bit_index], candidate[bit_index], "bit-bash value");
end
storage = original;
```

Run the semantic and isolated timing comparisons independently:

```sh
make feature-test FEATURE=register_sequences
make feature-benchmark FEATURE=register_sequences
```

The semantic gate executes 100,000 iterations and requires exact transactions,
checks, simulated time, and checksum agreement. Timing is published only from
an admitted serial host-load window and must pass the repository's unchanged
`1.10x` hard guard.
