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
| `channel` | 0.955x | 0.942x | 0.960x | 0.949x | 0.60% | `passed` | `False` |

The paired median is the guard. A value above `1.10x` is a valid
failure only when both order strata exceed `1.05x` and the independent
median ratio is within 5% relative of the paired median. Other hard-limit
crossings are classified as `invalid_environment`.

Raw execution order and every completed sample are preserved in
`latest.jsonl` and `latest.json`.
