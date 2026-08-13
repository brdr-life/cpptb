# Seeds, streams, and replay

Every cpptb test has a master 64-bit seed and a versioned random algorithm. The
seed is retained in structured results and printed by the reference runner, so
a random failure can be repeated without reconstructing hidden global state.

## Select a seed

The reference runner accepts decimal and hexadecimal seeds:

```sh
uv run --frozen cpptb-run run --seed 0x1234 packet_sequence -- \
  build/cpptb/obj/Vdpi_packet_core
```

Direct simulator invocations use the equivalent environment variable:

```sh
CPPTB_TEST=packet_sequence \
CPPTB_RANDOM_SEED=0x1234 \
  build/cpptb/obj/Vdpi_packet_core
```

An invalid seed fails test selection with a clear message. With no override,
the framework uses deterministic seed `1`. A higher-level harness can choose a
fresh seed, but should always pass and retain it explicitly.

## Result metadata

A completed test records both seed and algorithm:

```json
{
  "schema_version": 5,
  "test_name": "packet_sequence",
  "random_seed": 4660,
  "random_algorithm": "xoshiro256ss-v1",
  "constraint_backend": "adaptive",
  "random_sampling_solves": 1000,
  "random_solver_solves": 0
}
```

The algorithm identifier is part of the replay contract. An implementation may
add another algorithm later, but it must not silently change the sequence
produced by `xoshiro256ss-v1`.

## Process-owned streams

Each registered root or spawned process receives a stream derived from the
master seed and stable process creation order:

```cpp
Task<void> writer(Dut dut, TestContext test) {
    auto& random = test.random();
    for (uint32_t index = 0; index < 100; ++index) {
        co_await send_word(dut, random.next_u64());
    }
}

Task<void> backpressure(Dut dut, TestContext test) {
    auto& random = test.random();
    for (uint32_t index = 0; index < 100; ++index) {
        co_await RisingEdge{dut.clk_i};
        dut.ready_i.set(random.randint<uint8_t>(0, 1));
    }
}

Task<void> traffic_test(Dut dut, TestContext& test) {
    co_await Join{writer(dut, test), backpressure(dut, test)};
}
```

Scheduling interleaving does not make one process consume another process's
random values. Replaying the same test, seed, and process creation topology
reproduces both streams.

Creating, removing, or reordering spawned processes can change derived stream
IDs. Treat process topology as part of the replay input, just like call order
within a process.

## Replay workflow

1. Retain the test name, seed, algorithm, build identity, and configuration from
   the failing result.
2. Run the exact test in a fresh simulator process with `--seed`.
3. Keep process creation order and random call order unchanged while reducing
   the failure.
4. Add the reproducing seed as a directed regression when it exposes a real DUT
   or testbench defect.

Avoid deriving stimulus from wall-clock time, memory addresses, thread IDs, or
a separate unrecorded generator. Reference models that need an independent
stream can construct `Random` with an explicitly derived and recorded seed.

## Reproducibility and solvers

Both built-in constraint backends consume the supplied process stream.
`RandomSearchBackend` replays candidate generation, and `Z3RandomBackend` uses
the stream to select among satisfying models. A backend adapter should never
use an unseeded solver RNG or hidden global random source.

Constraint changes naturally change the number or order of consumed values.
Seed replay guarantees a stable sequence for the same model and implementation;
it does not promise that an edited transaction class maps an old seed to the
same fields.

## Exact performance peers

The randomization benchmarks are also replay-contract tests:

| Feature | Workload |
|---|---|
| `random_stimulus` | `randint`, weighted choice, 65-bit packed generation, and shuffle |
| `constrained_packet` | Four related fields with ranges, alignment, modulo, and a cross-field rule |
| `constraint_extensions` | Membership, weighted range, soft default, disabled mode, nested object, array, and 65-bit field |

Each C++ workload has a pure-SystemVerilog twin that consumes the same random
words and must produce the same 100,000 DUT transactions and checksum:

```sh
make feature-test FEATURE=constraint_extensions
make feature-benchmark FEATURE=constraint_extensions
```

See [Performance](../performance.md#constraint-extensions) for the current
measurements and environment qualification rules.
