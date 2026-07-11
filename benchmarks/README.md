# Feature benchmark registry and serial regression

`benchmarks/registry.py` is the side-effect-free index of authoring and
integration benchmarks. Use the Make targets rather than calling individual
runners when selecting work from the registry:

```sh
make feature-list
make feature-test FEATURE=event
make feature-benchmark FEATURE=event
make feature-regression
make registry-check
```

`feature-test` performs the selected entry's semantic execution.
`feature-benchmark` builds and measures one exact registry name.
`feature-regression` validates the registry, builds every required target
before measurement, and then invokes runner commands one at a time in stable
registry order. Commands never use parallel make. The authoring runner always
receives one `--example` and writes under
`benchmarks/authoring_core/results/<example>/`, so one feature cannot replace
another feature's raw samples or report.

## Load settling and results

Before every measured entry, the regression polls normalized one-minute load
(`load_average_1m / logical_cpu_count`). The default threshold is `1.00`, the
poll interval is 5 seconds, and the timeout is 60 seconds. Every probe plus
the threshold, poll interval, timeout, and elapsed time is persisted. A settle
timeout classifies that entry as `invalid_environment`, skips its measurement,
and continues with later entries. Load evidence is never used to rescale or
otherwise normalize benchmark samples.

Runner failures also do not stop later entries. Aggregate status uses this
precedence:

```text
failed > invalid_environment > passed_inconclusive > passed
```

Per-entry orchestration output is stored in
`benchmarks/results/regression/entries/<feature>/latest.json` and `latest.md`.
The atomic aggregate index is
`benchmarks/results/regression/latest.json` and `latest.md`. Authoring and
peripheral runners retain their own result directories and raw journals.

The multiclock entry is `equivalence_only`: the adapter requires one
`CPP_DPI_MULTICLOCK_RESULT` and one `PURE_SV_MULTICLOCK_RESULT`, with an exact
match of `iterations`, `checks`, `sim_cycles`, and `failures`, and zero
failures. It does not interpret elapsed time as a performance result.

The peripheral suite is an independently selected registry entry. Its
diagnostics are not a universal preflight or gate for authoring features.
The aggregate index exposes its separate diagnostic status so a threshold
crossing remains visible even when it does not fail the authoring gate.
