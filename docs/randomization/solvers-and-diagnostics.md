# Solvers and diagnostics

Randomized transaction classes produce a backend-neutral constraint problem.
The backend decides how to find a satisfying assignment, but it does not change
the transaction API, seed source, or field access.

## Choose a backend

| Backend | Dependency | Best fit | Failure knowledge |
|---|---|---|---|
| `AdaptiveConstraintBackend` | None; optional fallback chosen by the application | Default policy: sample common constraints and invoke a configured solver only after genuine search exhaustion | Preserves the result and diagnostics from the engine that ran |
| `RandomSearchBackend` | None | Ranges, membership, distributions, and constraints with reasonable acceptance | Proves empty direct domains; otherwise distinguishes search exhaustion |
| `Z3RandomBackend` | Z3 for translation units that include it | Tightly coupled arithmetic or models whose legal assignments are sparse | Proves unsatisfiability and reports named hard constraints from an unsat core |

The dependency-free adaptive policy is the default. It uses deterministic
sampling with a 4096-candidate budget. Without a configured fallback,
exhaustion remains an actionable `SearchExhausted` result. Z3 is optional: use
it only in applications that need sparse solving or proof-quality diagnostics.

## Default deterministic search

```cpp
Packet packet;
test.randomize(packet); // AdaptiveConstraintBackend sampling fast path
```

The backend folds simple bounds and `inside()` sets into candidate domains,
applies active distributions within those domains, prefers satisfiable soft
constraints, and tests the remaining expressions. The default search budget is
4096 candidates.

Standalone code can choose another budget:

```cpp
RandomSearchBackend search{16384};
Random random{0x1234};

const RandomizeResult result = packet.randomize(random, search);
```

A `SearchExhausted` result does not mean the model is impossible. Its message
lists named constraints and rejection counts and suggests `Z3RandomBackend`
when deterministic sampling could not find a solution.

## Adaptive Z3 fallback

Configure one policy object when most models are inexpensive but a few need a
solver:

```cpp
#include "cpptb/z3_random_backend.hpp"

Z3RandomBackend z3_only{0};
AdaptiveConstraintBackend constraints{z3_only};

Task<void> packet_sequence(Dut dut, TestContext& test) {
    test.set_random_backend(constraints);

    Packet packet;
    test.randomize(packet); // samples first, falls back only on exhaustion
    co_await drive_packet(dut, packet);
}
```

Construct the fallback Z3 backend with `0` because the adaptive policy already
performed the sampling pass. Both backend objects must outlive every
`test.randomize()` call that uses them.

Applications that deliberately use the process-wide default policy can install
the same fallback once:

```cpp
Z3RandomBackend z3_only{0};
default_adaptive_constraint_backend().set_fallback(z3_only);
```

Keep `z3_only` alive for the complete test run. Explicit per-test policy is
usually easier to isolate and reason about.

## Optional direct Z3 backend

Only code that selects Z3 needs its header and linker dependency:

```cpp
#include "cpptb/z3_random_backend.hpp"

Task<void> coupled_sequence(Dut dut, TestContext& test) {
    Z3RandomBackend z3;
    test.set_random_backend(z3);

    CoupledTransaction item;
    test.randomize(item);
    co_await drive_transaction(dut, item);
}
```

With the installed CMake package, enable the optional adapter when building
cpptb and link its explicit target:

```sh
cmake -S . -B build -DCPPTB_WITH_Z3=ON
```

```cmake
target_link_libraries(my_testbench PRIVATE cpptb::z3)
```

For a non-CMake build, add the compiler and linker flags reported by:

```sh
pkg-config --cflags --libs z3
```

`Z3RandomBackend` first runs a small deterministic fast search, then invokes Z3
when needed. Construct it with `0` to force every solve through Z3:

```cpp
Z3RandomBackend z3_only{0};
test.set_random_backend(z3_only);
```

Satisfying Z3 models are selected using the same test random stream. The same
seed replays; different seeds can select different valid assignments.
Membership, distribution support, soft preference, and independent `RandC`
cycles retain their user-visible semantics across both backends.

The backend object must outlive every `test.randomize()` call that uses it.
`TestContext` stores a reference, not an owning copy.

Z3 caches the translated persistent model for each `Randomized` object.
Repeated solves, including `randomize_with()` calls, reuse it; changing a
constraint or distribution mode invalidates that object's cached model.
`cache_builds()`, `cache_hits()`, and `clear_cache()` are available for focused
diagnostics and tests.

## Result metadata

Structured test result schema 5 records:

```json
{
  "constraint_backend": "adaptive",
  "constraint_backend_version": "4.13.3.0",
  "random_sampling_solves": 999,
  "random_solver_solves": 1
}
```

The version is supplied by the configured fallback when present. The solve
counters report the engine that actually produced each result, making replay
and performance reports distinguish an inexpensive sampling run from solver
fallback.

## Failure statuses

| Status | Meaning | Typical response |
|---|---|---|
| `Solved` | A complete assignment was produced | Use the fields |
| `Unsatisfiable` | The backend proved the active hard model has no solution | Fix conflicting constraints or modes |
| `SearchExhausted` | Sampling used its candidate budget without finding a solution | Inspect rejection counts, increase attempts, or use Z3 |
| `CycleExhausted` | Internal signal that one or more `RandC` domains completed | The owning `Randomized` object resets only those cycles and retries automatically |
| `BackendError` | Invalid model, incompatible policies, malformed backend result, or misuse | Follow the actionable message |

`test.randomize()` converts any externally visible failure into a fatal test
requirement at the call site. Call `item.randomize(random, backend)` directly
when code needs to branch on status.

## Write useful constraint names

Names are part of the diagnostic surface:

```cpp
constraint("DMA length is a whole beat", length % beat_bytes == 0);
constraint("atomic requests stay in one cache line",
           opcode != Opcode::Atomic || length <= cache_line_bytes);
```

Prefer a stable statement of intent over an expression transcription. Search
diagnostics report which named constraints rejected candidates; Z3 unsat cores
report the named hard constraints needed for the contradiction. Nested object
labels are qualified automatically, such as `header.default route`.

## Common errors

| Message shape | Cause |
|---|---|
| `minimum exceeds maximum` | A field, `range()`, or `randint()` was constructed with reversed bounds |
| `multiple active distributions` | More than one enabled `dist()` policy targets the same field |
| `distribution ... has no legal values` | Hard constraints removed the complete weighted support |
| `nested randomized object ... must be randomized through its owning object` | A child was solved independently of its root |
| `search exhausted ... rejected N` | Legal assignments were too sparse for the configured search budget |
| `returned the wrong assignment size` | A custom backend violated the `ConstraintBackend` contract |

## Backend extension point

A custom backend implements one small interface:

```cpp
class ConstraintBackend {
  public:
    virtual std::string_view name() const noexcept = 0;
    virtual std::string_view version() const noexcept { return {}; }
    virtual RandomizeResult solve(const ConstraintProblem& problem,
                                  Random& random) = 0;
};
```

The problem carries typed field descriptors, named hard and soft constraints,
active distributions, and current `RandC` exclusions. A backend must return one
raw assignment per variable and use the supplied `Random` for replayable model
selection. Backend-specific types should not leak into transaction classes.

## Related APIs

- [Constrained transactions](constrained-transactions.md) covers the
  transaction classes and `randomize()` calls whose constraint problems
  these backends solve.
- [Seeds, streams, and replay](reproducibility.md) explains the seed and
  process-stream contract every backend consumes.
- [Randomization library reference](../library/randomization.md) lists the
  signatures for the backends, `ConstraintBackend`, and `RandomizeResult`.
