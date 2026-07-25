# Firmware

Committed so a port runs without a RISC-V toolchain. Rebuild only when the
workload changes; see the parent README for the toolchain and commands.

| VMEM | Words | Source ELF sha256 | Workload |
| --- | ---: | --- | --- |
| `coremark.vmem` | 14566 | `38571cd44117a41060af37df2796b7731561618fdcc2a4ef5448aba723124b3d` | CoreMark 1.0, 10 iterations, ~4.1M cycles |
| `hello_test.vmem` | 358 | `2c627352b6215e37c3ba8633784cce9f72b362401f259daecbacb6b9fbe6e3d2` | smoke test, ~600 cycles |

Both were built from Ibex at the commit pinned in `sources.toml`, with the
pinned GCC toolchain, and converted with `bin2vmem.py`.

CoreMark is the workload to measure. It validates its own result, so a run that
silently computed the wrong thing is detectable rather than merely fast:
upstream reports `Correct operation validated`, `crcfinal 0xfcaf`, and a score
of `1.230458` over 10 iterations.

`hello_test` is a smoke test only. At roughly 600 cycles it is far too short to
measure anything.
