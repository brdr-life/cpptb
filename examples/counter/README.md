# Counter

This is the smallest end-to-end cpptb project. The user-facing sequence is
[`testbench.cpp`](testbench.cpp): an explicitly registered coroutine receives
the generated `Dut dut` and library-owned `TestContext`, drives reset and
enable, waits for clock edges, and checks the count.

The repository root `Makefile` supplies shared regression aliases. This
directory also contains an optional standalone [`Makefile`](Makefile); it is a
thin wrapper over `cpptb build`, `cpptb list`, and `cpptb test` and contains no
code-generation or simulator logic.

```sh
make cpp-dpi-counter-run
make cpp-dpi-counter-sv-run
make cpp-dpi-counter-suite-test
```

The same workflow can be run from this directory:

```sh
make cpp-dpi-counter-build
make list
make run-all
make run TEST=counter_reset_defaults
```

Both implementations run the same fixed `kCountCycles` workload and report the
same checks, primary-clock cycles, and failures. Build the typed DUT, DPI
wrapper, transport adapter, and simulator with:

```sh
cpptb build
```

The testbench owns timing with `test.start_clock(dut.clk, 10_ns)`.
It registers `counter_sequence` and `counter_reset_defaults`; `cpptb test`
discovers both and runs one fresh simulator process per test. The repository's
benchmark target selects `counter_sequence` to preserve the exact pure-SV
comparison workload.
