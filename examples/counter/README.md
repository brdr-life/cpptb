# Counter

This is the smallest end-to-end cpptb project. The user-facing sequence is
[`testbench.cpp`](testbench.cpp): it explicitly drives reset and enable, waits
for clock edges, inserts a `1ps` settle delay, and reads the count.

```sh
make cpp-dpi-counter-run
make cpp-dpi-counter-sv-run
```

Both implementations report the same iterations, checks, primary-clock cycles,
and failures. Regenerate the typed DUT and DPI wrapper with:

```sh
uv run --frozen cpptb-codegen examples/counter/counter.dpi.json
```
