# Authoring-core C++ DPI vs pure SystemVerilog benchmark

- Result status: `success`
- Iterations per sample: `100000`
- Initial adjacent warmed pairs: `16`
- Conditional extra pairs: `16`
- Absolute hard guard: `C++ DPI / pure SV <= 1.10x`
- Peripheral preflight: `skipped`
- Measurement environment: `valid`

| Kernel | Paired median | DPI-first | SV-first | Independent | Disagreement | Status | Extra batch |
|---|---:|---:|---:|---:|---:|---|---:|
| `event` | 1.001x | 1.012x | 0.917x | 0.980x | 2.13% | `passed` | `False` |

The paired median is the guard. A value above `1.10x` is a valid
failure only when both order strata exceed `1.05x` and the independent
median ratio is within 5% relative of the paired median. Other hard-limit
crossings are classified as `invalid_environment`.

Raw execution order and every completed sample are preserved in
`latest.jsonl` and `latest.json`.
