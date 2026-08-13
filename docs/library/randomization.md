# Randomization

<!-- api-headers: include/cpptb/random.hpp include/cpptb/randomized.hpp include/cpptb/z3_random_backend.hpp -->

The deterministic value generator and the constrained-transaction layer.
All in `namespace cpptb`. [Random stimulus](../random-stimulus.md) and the
[randomization guides](../randomization/constrained-transactions.md) teach
usage; this page is the surface. Nothing here suspends a coroutine or
touches the DUT — randomization is pure computation.

## The generator

### Random

```cpp
constexpr Random();                        // seed 1 (kDefaultRandomSeed)
explicit constexpr Random(uint64_t seed);

uint64_t next_u64();                       // raw xoshiro256** step
Value    randint(Value minimum, Value maximum);  // inclusive; unbiased;
                                                 // throws if min > max
Bits<W>  randbits<W>();                    // masked to W bits
range_value choice(range);                 // throws on an empty range
auto     weighted_choice(range);           // of Weighted{value, weight}
void     shuffle(range);                   // Fisher-Yates in place
uint64_t seed() const;   void reseed(uint64_t seed);
```

Integral and enum values through 64 bits; signed ranges are exact. The
algorithm identifier `"xoshiro256ss-v1"` is part of the replay contract —
recorded into every result, never silently changed.

In a test, `test.random()` returns the **calling process's** stream,
derived from the master seed and the process's creation-order ID; process
topology is therefore part of the replay input, and no process ever
consumes another's draws. The master seed defaults to 1, set per run with
`CPPTB_RANDOM_SEED`. A standalone `Random model_random{0x1234}` is fine
for reference models and unit tests.

## Constrained transactions

### Randomized

```cpp
class Randomized {                         // non-copyable, non-movable
    ConstraintHandle constraint(Constraint expr, std::string_view label = {});
    ConstraintHandle soft_constraint(Constraint expr, std::string_view label = {});
    ConstraintHandle distribution(Distribution dist, std::string_view label = {});

    virtual void pre_randomize()  {}       // before every solve attempt
    virtual void post_randomize() {}       // after a successful assignment

    RandomizeResult randomize(Random& random,
                              ConstraintBackend& backend = default_constraint_backend());
    RandomizeResult randomize_with(Random& random, Constraint expr,
                              ConstraintBackend& backend = default_constraint_backend());
};
```

The transaction base class. Declare fields as members, constraints in the
constructor. In a test, prefer `test.randomize(item)` /
`test.randomize_with(item, expr)` — same solve, but it uses the process
stream and configured backend, records solver metadata into the result,
and turns a failed solve into a fatal check.

### Rand
### RandC
### RandArray
### RandBits

```cpp
Rand<Value>      field{*this, "name", minimum, maximum};   // solvable scalar
RandC<Value>     mode{*this, "mode"};    // nonrepeating: cycles its domain
RandArray<V, N>  lanes{*this, "lanes"};  // N independent Rand<V>, operator[]
RandBits<W>      payload{*this, "payload"};  // wide; constrain per 32-bit word
```

`Rand<V>::get()` (or implicit conversion) reads the last solved value.
Assigning to a `Rand` sets the stored value only — it does not constrain
the next solve.

### Constraint expressions

Built from field references with ordinary operators — arithmetic
`+ - * %`, comparisons, and logical `&& || !` — plus:

```cpp
inside(field, values_and_ranges...);   // set membership; range(lo, hi)
dist(field, weighted(value, weight)...);   // weighted distribution
```

Division, bitwise operators, implication syntax, and arbitrary C++ calls
are not constraint operators.

### ConstraintHandle

```cpp
void enable() const;  void disable() const;
void set_enabled(bool) const;  bool enabled() const;  bool soft() const;
```

Runtime control of a named constraint — the `constraint_mode` equivalent.

## Results and backends

### RandomizeResult

```cpp
struct RandomizeResult {
    RandomizeStatus status;    // Solved, Unsatisfiable, CycleExhausted,
                               // SearchExhausted, BackendError
    RandomizeEngine engine;    // Sampling or Solver
    std::string message;
    explicit operator bool() const;   // true iff Solved
};
```

### ConstraintBackend

```cpp
class RandomSearchBackend;         // pure sampling, bounded attempts
class AdaptiveConstraintBackend;   // sampling first, solver fallback — the default
class Z3RandomBackend;             // Z3-backed, for hard constraint sets

ConstraintBackend& default_constraint_backend();
test.set_random_backend(backend);  // per-test override
```

The result records which engine solved each transaction
(`random_sampling_solves` / `random_solver_solves`), so a constraint set
that quietly fell back to the solver is visible in CI.

## See also

- [Constrained transactions](../randomization/constrained-transactions.md),
  [Value generation](../randomization/value-generation.md),
  [Reproducibility](../randomization/reproducibility.md) — the guides.
- [Functional coverage](coverage.md) — recording what the stimulus hit.
