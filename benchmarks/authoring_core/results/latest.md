# Authoring-core C++ DPI vs pure SystemVerilog benchmark

- Iterations per sample: `100000`
- Initial adjacent warmed pairs: `15`
- Absolute hard guard: `C++ DPI / pure SV <= 1.10x`
- Peripheral preflight: `passed`

| Kernel | Paired median | One-sided 95% upper | Status | Extra batch | Normalized/control |
|---|---:|---:|---|---:|---:|
| `control` | 1.025x | 1.089x | `passed` | `False` | 1.000x |
| `task_value` | 1.088x | 1.106x | `passed_inconclusive` | `True` | 1.062x |
| `clock_cycles` | 1.090x | 1.116x | `passed_inconclusive` | `True` | 1.063x |
| `timeout` | 0.862x | 0.920x | `passed` | `False` | 0.841x |
| `wait_until` | 1.071x | 1.111x | `passed_inconclusive` | `True` | 1.045x |
| `event` | 1.088x | 1.107x | `passed_inconclusive` | `True` | 1.061x |
| `channel` | 1.085x | 1.103x | `passed_inconclusive` | `True` | 1.058x |
| `all` | 0.976x | 1.032x | `passed` | `False` | 0.952x |

The absolute paired ratio is the guard. Normalization against `control` is diagnostic only.
Raw execution order and every sample are preserved in `latest.json`.
