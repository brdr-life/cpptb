# Contributing

cpptb changes should preserve both simulator semantics and the established
performance envelope.

## Development setup

```sh
uv sync --frozen
cmake -S . -B build/cmake -DBUILD_TESTING=ON
cmake --build build/cmake
ctest --test-dir build/cmake --output-on-failure
make test
```

## Feature changes

Every new runtime feature requires:

- Focused C++ unit or conformance coverage.
- An end-to-end C++ DPI testbench use case.
- An equivalent pure SystemVerilog implementation.
- Registration in `benchmarks/registry.py` when it has a measurable runtime
  path.
- A dedicated `make feature-test FEATURE=name` semantic check.
- A controlled `make feature-benchmark FEATURE=name` run.

Stop and investigate when the final C++ DPI to SystemVerilog process-wall ratio
exceeds `1.10`. Do not weaken the guard or compare unequal workloads to make a
result pass.

## Generated files

After changing a manifest, RTL interface, or generator:

```sh
make cpptb-codegen-test
make cpptb-codegen-frontend-check
```

Commit intentional generated output changes with their source changes.

## Pull requests

Keep implementation changes, generated output, and benchmark evidence easy to
review. Describe simulator versions used, commands run, semantic results, and
any remaining portability or performance risk.
