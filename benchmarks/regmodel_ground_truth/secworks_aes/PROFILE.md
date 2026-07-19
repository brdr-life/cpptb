# secworks AES performance profile

This profile explains the gap between the generated cpptb register-model
testbench and its exact pure-SystemVerilog peer. Both benches execute the same
register waveform, DUT cycles, checks, and result checksum.

## Reproduce

```sh
make -C benchmarks/regmodel_ground_truth/secworks_aes build
python3 benchmarks/regmodel_ground_truth/secworks_aes/run_equivalence.py \
  --skip-build
python3 benchmarks/regmodel_ground_truth/secworks_aes/run_benchmark.py \
  --skip-build --repeats 180 --runs 16 --allow-overhead
```

The build compiles both measured implementations with Verilator
`OPT_FAST=-O3`. The benchmark runs one process at a time, alternates process
order, validates the complete workload after every pair, and rejects the timing
result if normalized one-minute host load exceeds `0.30`.

## Historical diagnostic result

The July 18, 2026 profiling run used Verilator 5.050:

| Workload | Pure SV | cpptb RegModel | Ratio |
|---|---:|---:|---:|
| 180 suites / 3,600 AES cases | 200.7 ms | 318.5 ms | 1.587x |

The one-minute load average moved from 3.74 to 4.08 on an eight-logical-CPU
host, so normalized load was at least `0.467`, above the current `0.30`
admission limit. The ratio is therefore diagnostic rather than accepted
performance evidence. Exact comparison passed for all three cpptb decomposition
modes, the pure-SV peer, and the unchanged upstream oracle: 720 ordered bus
events, 20 cases, 80 checked words, and checksum `46264475`.

## Callback accounting

A compile-gated runtime profile of 200 suites reported:

```text
steps=88002 init=1 clock_edges=0 delays=88001
outputs_changed=88002 output_transfers=88001
```

The workload contains one reset delay plus 440 authored delays per suite. The
optimized wrapper therefore crosses DPI only at the 88,001 testbench timing
boundaries. Its generated 2 ns clock runs in SystemVerilog and makes no DPI
edge callback because no C++ process is waiting on that clock.

Pure SV resumes its authored task directly inside Verilator at each timing
boundary. cpptb resumes the generated SV timer owner, enters the DPI runtime,
updates C++ scheduler time, resumes the parked C++ coroutine, transfers changed
drives, and schedules the next deadline. Sampling shows the DUT's NBA/evaluate
work in both profiles; the excess samples are in Verilator delay/trigger
scheduling, DPI callback/context handling, C++ scheduler resumption, and TLS or
allocator support around those transitions.

## Layer decomposition

The benchmark fixture supports three semantically identical modes through
`AES_REGMODEL_MODE`: generated `regmodel`, direct `master`, and a `fused`
long-lived bus sequence. A 12-run same-binary mode rotation measured:

| Mode | Median | Relative layer cost |
|---|---:|---:|
| Generated RegModel | 308.3 ms | 1.026x over direct master |
| Direct bus master | 300.4 ms | 1.005x over fused sequence |
| Fused bus sequence | 299.0 ms | Baseline cpptb scheduling path |

The register model is not the source of the headline slowdown. Removing both
the generated register handles and per-access child tasks leaves almost all of
the pure-SV gap intact.

## Experiments

Clock callbacks are now armed lazily by signal and edge kind. A 20-pair A/B
run measured the gated wrapper at 325.8 ms and the previous unconditional
clock callback at 354.2 ms, a `0.917x` paired ratio. Once an edge kind is used,
its subscription stays armed so normal clock-driven benches avoid repeated
subscription publication.

Returning the next timer deadline from the scheduler step removed a DPI query,
but a controlled 20-pair A/B measured only `0.994x` paired improvement. That
extra ABI was removed. Conditional output pulls remain separate because they
protect workloads whose outputs do not change on every scheduler step.

## Conclusion

The retained clock gate removes avoidable work and improves this timer-driven
bench by about 8%. The remaining roughly 1.5-1.6x ratio is the cost of crossing
and resuming the C++ scheduler at each authored timing boundary in this
fine-grained workload. Closing it further requires an architectural reduction
in boundary resumptions, such as a proven run-ahead or batched command model;
register-handle or coroutine-wrapper micro-optimization cannot remove it.
