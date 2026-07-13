# Reconstructed pre-feature runtime experiment

An earlier scheduler investigation reconstructed a pre-feature coroutine
runtime from independent transcript captures and compared it with the evolving
peripheral-suite runtime. The exercise helped separate scheduler overhead from
testbench and transport changes, but the exact historical executable could not
be recovered. The result was therefore diagnostic rather than a trustworthy
long-term baseline.

The machine-specific snapshots, absolute source paths, raw build provenance,
and duplicate framework sources were removed while preparing the public
repository. They remain available in Git history. The accepted architectural
conclusions and reproducible current measurements are captured in
`benchmarks/authoring_core/OPTIMIZATION_NOTES.md` and
`benchmarks/baselines/`.
