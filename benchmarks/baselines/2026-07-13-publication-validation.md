# Publication validation: 2026-07-13

- Full serial registry: `passed` (`29/29` entries)
- Authoring workloads: `25/25` passed
- Authoring iterations: `100000`
- Measured adjacent pairs per feature: `16`
- Hard guard: `C++ DPI / pure SystemVerilog <= 1.10x`
- Worst authoring ratio: `mem_probe_deposit`, `1.057x`
- Samples normalized: `false`
- Environment: macOS arm64, Verilator `5.050`, Apple clang `21.0.0`

| Feature | C++ DPI / SystemVerilog | Status |
|---|---:|---|
| `control` | 0.985x | passed |
| `task_value` | 0.954x | passed |
| `clock_cycles` | 0.965x | passed |
| `timeout` | 0.803x | passed |
| `task_timeout` | 0.758x | passed |
| `wait_until` | 0.944x | passed |
| `event` | 0.949x | passed |
| `channel` | 0.954x | passed |
| `all` | 0.924x | passed |
| `wide64` | 1.006x | passed |
| `wide_echo_137` | 1.037x | passed |
| `wide_slice` | 1.021x | passed |
| `fixed_mac` | 1.024x | passed |
| `array_index` | 1.026x | passed |
| `array_wide` | 1.036x | passed |
| `mem_rw` | 1.006x | passed |
| `hier_probe` | 0.996x | passed |
| `mem_backdoor` | 1.042x | passed |
| `mem_probe_read` | 1.044x | passed |
| `mem_probe_deposit` | 1.057x | passed |
| `mem_probe_read_deposit` | 1.034x | passed |
| `signal_edge` | 0.957x | passed |
| `array_multidim` | 1.017x | passed |
| `force_release` | 1.018x | passed |
| `packed_view` | 0.986x | passed |

The counter, multi-clock, and timer-only examples passed exact equivalence on
iterations, checks, primary-clock cycles, and failures.

## Four-mode peripheral suite

- Iterations: `10000`
- C++ DPI / pure SystemVerilog: `0.975x`
- C++ VPI / pure SystemVerilog: `7.76x`
- cocotb / pure SystemVerilog: `24.68x`
- C++ DPI one-sided 95% upper median bound: `0.988x`
- Environment validity: `valid`
- Guard status: `passed`
