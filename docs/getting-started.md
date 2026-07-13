# Getting started

Install Verilator, a C++20 compiler, Python 3.11 or newer, CMake, and `uv`.
From a checkout:

```sh
uv sync --frozen
make test
```

Use `examples/multiclock` as the complete project template. A design supplies:

1. RTL sources and a top module.
2. A `.dpi.json` manifest describing sources, generated outputs, clocks,
   hierarchy rules, and optional internal probes.
3. A typed testbench sequence in `testbench.cpp`.
4. A thin adapter that registers the sequence and defines timeout/result
   policy.

Generate bindings with:

```sh
uv run --frozen cpptb-codegen path/to/design.dpi.json
```

The generated C++ DUT exposes explicit `get()` and `set()` operations. Time
advances only when a coroutine awaits a scheduling primitive; writing a signal
or depositing an internal value does not insert a delay.

Continue with [testbench authoring](testbench-authoring.md) and
[scheduling](scheduling.md).
