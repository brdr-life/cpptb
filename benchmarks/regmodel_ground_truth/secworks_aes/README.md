# secworks AES register-model ground truth

This suite runs the unmodified upstream secworks AES top-level testbench as a
correctness oracle, then repeats the same 20 NIST AES-128/AES-256 encrypt and
decrypt cases through the generated cpptb register model.

The DUT RTL is reused from the pinned, BSD-2-Clause-licensed copy under
`benchmarks/framework_comparison/open_cores/third_party/secworks_aes/`.
`upstream/tb_aes.v` is copied verbatim from the same pinned commit:
`80dc4718e1dcbbdb4b0dd1bdb393d8f7b98981dc`. The source is
<https://github.com/secworks/aes/blob/80dc4718e1dcbbdb4b0dd1bdb393d8f7b98981dc/src/tb/tb_aes.v>.

Generated C++ register bindings and simulator artifacts live under
`build/benchmarks/regmodel_ground_truth/secworks_aes/`.
The measured callback accounting, layer decomposition, and retained/rejected
optimization experiments are recorded in [`PROFILE.md`](PROFILE.md).

## Run

```sh
make secworks-aes-regmodel-equivalence
make secworks-aes-regmodel-benchmark
```

The equivalence command requires all 720 observed register-bus events from the
upstream oracle, cpptb, and pure-SV peer to match exactly. It also compares all
20 cases, 80 result words, and the final checksum.

The benchmark defaults to 180 suites, or 3,600 AES cases, and runs cpptb and
pure SV one process at a time. Override the workload with `REPEATS` up to 200
and the sample count with `RUNS`:

```sh
REPEATS=100 RUNS=6 make secworks-aes-regmodel-benchmark
```

`RUNS` must be an even number of at least four so each implementation starts
the same number of measured pairs. The runner rejects timing evidence when the
one-minute load average divided by the logical CPU count exceeds `0.30` at
admission or during sampling.

The existing `1.10x` cpptb/pure-SV policy is enforced after that environment
gate. The runner writes the complete result to
`build/benchmarks/regmodel_ground_truth/secworks_aes/benchmark-latest.json`
before returning an error when the environment is invalid or the ratio is over
the guard. Pass
`--allow-overhead` directly to `run_benchmark.py` only for profiling an already
acknowledged ratio regression; it does not bypass the host-load gate.
