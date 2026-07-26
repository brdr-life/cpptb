# Firmware

Committed so a port runs without a RISC-V toolchain. Rebuild only when the
workload changes; see the parent README for the toolchain and commands.

| VMEM | Words | Source ELF sha256 | Workload |
| --- | ---: | --- | --- |
| `coremark.vmem` | 14566 | `4fb5e4b7fe449471e6c3ac8f0d08d6e4491f789c7254827b6e5087ce8af71bc6` | CoreMark 1.0, 100 iterations, ~40.7M cycles |
| `hello_test.vmem` | 358 | `2c627352b6215e37c3ba8633784cce9f72b362401f259daecbacb6b9fbe6e3d2` | smoke test, ~600 cycles |

Both were built from Ibex at the commit pinned in `sources.toml`, with the
pinned GCC toolchain, and converted with `bin2vmem.py`.

CoreMark is the workload to measure. It validates its own result, so a run that
silently computed the wrong thing is detectable rather than merely fast:
upstream reports `Correct operation validated` and a score of `1.230382` over
100 iterations.

The iteration count is a compile-time constant, so changing it means rebuilding:

```sh
make -C ../../deps/ibex/examples/sw/benchmarks/coremark clean
make -C ../../deps/ibex/examples/sw/benchmarks/coremark ITERATIONS=100
```

100 iterations was chosen to put a single run near 45 seconds. At the upstream
default of 10 it finishes in about 4.5 seconds, which is too short to measure
against process startup and host noise.

`hello_test` is a smoke test only. At roughly 600 cycles it is far too short to
measure anything.
