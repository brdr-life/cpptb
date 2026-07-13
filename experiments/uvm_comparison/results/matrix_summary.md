# UVM Simulator Matrix Results

- Timestamp: `20260705T174727Z`
- Repeats: `3`
- Verilator: `Verilator 5.050 2026-07-01 rev vUNKNOWN-built20260701`
- xezim: `xezim version 0.6.0`

## Revisions

- xezim: `af73fdbe971a4087a96554aacc4b0bab39df4843`
- xezim-core: `abd461e196773d210ef76e2fb512b7c71b6d9bc4`
- GettingVerilatorStartedWithUVM: `181978306d6bd296a815b12cddfd8c9942ddc0f8`
- uvm-core: `78c06547a2a0a29b3dc9dcafae62b75b2ff61544`

## Aggregate Results

### Build-Only Comparison

| Simulator | What This Measures | Mean/Wall s | Notes |
|---|---|---:|---|
| xezim | Per-run parse/elaboration phase | 0.175 | No generated design binary; paid each run. |
| Verilator | One-time generated C++ build | 83.208 | Reused for all subsequent test runs. |

Build ratio, Verilator build / xezim parse-elab: `475.0x`. This is an iteration-cost comparison, not an artifact-equivalence claim.

### Runtime-Only Comparison

| Test | Functional Status | xezim sim phase s | Verilator sim report s | Runtime Ratio |
|---|---|---:|---:|---:|
| data0_test | xezim 3/3; Verilator 2/3 | 2.488 | 0.131 | 19.0x |
| data1_test | xezim 3/3; Verilator 3/3 | 2.620 | 0.130 | 20.2x |
| random_test | xezim 3/3; Verilator 0/3 | 2.430 | 0.123 | 19.7x |
| many_random_test | xezim 3/3; Verilator 0/3 | 2.489 | 0.161 | 15.5x |

The cleanest apples-to-apples runtime rows are `data0_test` after rerun and `data1_test`; the random tests execute under Verilator but report scoreboard UVM errors.

### xezim, Accellera 1800.2-2017-1.0
| Test | Passes | Mean wall s | xezim compile s | xezim sim s | xezim total s | Packets | Finish times |
|---|---:|---:|---:|---:|---:|---|---|
| data0_test | 3/3 | 2.738 | 0.175 | 2.488 | 2.663 | `[[77, 77], [77, 77], [76, 76]]` | `[1565, 1575, 1545]` |
| data1_test | 3/3 | 2.880 | 0.175 | 2.620 | 2.796 | `[[77, 77], [77, 77], [76, 76]]` | `[1565, 1575, 1555]` |
| random_test | 3/3 | 2.675 | 0.175 | 2.430 | 2.605 | `[[73, 73], [73, 73], [73, 73]]` | `[1500, 1500, 1500]` |
| many_random_test | 3/3 | 2.734 | 0.175 | 2.489 | 2.663 | `[[73, 73], [73, 73], [73, 73]]` | `[1500, 1500, 1500]` |

### Verilator, Accellera 1800.2-2017-1.0
| Test | Passes | Mean wall s | Verilator report sim wall s | Packets | Finish times |
|---|---:|---:|---:|---|---|
| data0_test | 2/3 | 0.380 | 0.131 | `[[], [76, 76], [76, 76]]` | `[]` |
| data1_test | 3/3 | 0.256 | 0.130 | `[[76, 76], [76, 76], [76, 76]]` | `[]` |
| random_test | 0/3 | 0.244 | 0.123 | `[[75, 75], [75, 75], [75, 75]]` | `[]` |
| many_random_test | 0/3 | 0.277 | 0.161 | `[[92, 92], [92, 92], [92, 92]]` | `[]` |

### Verilator Build
```json
{
  "pass": true,
  "wall_s": 83.20789733389392,
  "reported_wall_s": 83.075,
  "reported_alloc_mb": 325.812,
  "log": "experiments/uvm_comparison/results/verilator_build.log"
}
```

### Verilator Code Coverage
```json
{
  "line": {
    "pct": 27.5,
    "covered": 2714,
    "total": 9865
  },
  "toggle": {
    "pct": 96.2,
    "covered": 331,
    "total": 344
  },
  "branch": {
    "pct": 12.8,
    "covered": 1789,
    "total": 14024
  },
  "expr": {
    "pct": 10.3,
    "covered": 349,
    "total": 3404
  },
  "fsm_state": {
    "pct": 0.0,
    "covered": 0,
    "total": 0
  },
  "fsm_arc": {
    "pct": 0.0,
    "covered": 0,
    "total": 0
  }
}
```

### xezim Probe Against uvm-core main
```json
{
  "pass": false,
  "wall_s": 3.021159708034247,
  "fatal_messages": [
    "UVM_FATAL @ 0: reporter [SEQ_NOT_DONE] Sequence uvm_test_top.env.penv_in.agent.sequencer.seq already started"
  ],
  "log": "experiments/uvm_comparison/results/xezim_uvm-main_data0_probe.log"
}
```

## Raw Runs

- `xezim_uvm-main_data0_probe` rc=0 wall=3.021s timeout=False log=`experiments/uvm_comparison/results/xezim_uvm-main_data0_probe.log`
- `xezim_2017_data0_test_r1` rc=0 wall=2.750s timeout=False log=`experiments/uvm_comparison/results/xezim_2017_data0_test_r1.log`
- `xezim_2017_data0_test_r2` rc=0 wall=2.726s timeout=False log=`experiments/uvm_comparison/results/xezim_2017_data0_test_r2.log`
- `xezim_2017_data0_test_r3` rc=0 wall=2.739s timeout=False log=`experiments/uvm_comparison/results/xezim_2017_data0_test_r3.log`
- `xezim_2017_data1_test_r1` rc=0 wall=2.731s timeout=False log=`experiments/uvm_comparison/results/xezim_2017_data1_test_r1.log`
- `xezim_2017_data1_test_r2` rc=0 wall=3.148s timeout=False log=`experiments/uvm_comparison/results/xezim_2017_data1_test_r2.log`
- `xezim_2017_data1_test_r3` rc=0 wall=2.761s timeout=False log=`experiments/uvm_comparison/results/xezim_2017_data1_test_r3.log`
- `xezim_2017_random_test_r1` rc=0 wall=2.665s timeout=False log=`experiments/uvm_comparison/results/xezim_2017_random_test_r1.log`
- `xezim_2017_random_test_r2` rc=0 wall=2.672s timeout=False log=`experiments/uvm_comparison/results/xezim_2017_random_test_r2.log`
- `xezim_2017_random_test_r3` rc=0 wall=2.687s timeout=False log=`experiments/uvm_comparison/results/xezim_2017_random_test_r3.log`
- `xezim_2017_many_random_test_r1` rc=0 wall=2.706s timeout=False log=`experiments/uvm_comparison/results/xezim_2017_many_random_test_r1.log`
- `xezim_2017_many_random_test_r2` rc=0 wall=2.749s timeout=False log=`experiments/uvm_comparison/results/xezim_2017_many_random_test_r2.log`
- `xezim_2017_many_random_test_r3` rc=0 wall=2.746s timeout=False log=`experiments/uvm_comparison/results/xezim_2017_many_random_test_r3.log`
- `verilator_clean` rc=0 wall=0.027s timeout=False log=`experiments/uvm_comparison/results/verilator_clean.log`
- `verilator_build` rc=0 wall=83.208s timeout=False log=`experiments/uvm_comparison/results/verilator_build.log`
- `verilator_2017_data0_test_r1` rc=-11 wall=0.621s timeout=False log=`experiments/uvm_comparison/results/verilator_2017_data0_test_r1.log`
- `verilator_2017_data0_test_r2` rc=0 wall=0.261s timeout=False log=`experiments/uvm_comparison/results/verilator_2017_data0_test_r2.log`
- `verilator_2017_data0_test_r3` rc=0 wall=0.257s timeout=False log=`experiments/uvm_comparison/results/verilator_2017_data0_test_r3.log`
- `verilator_2017_data1_test_r1` rc=0 wall=0.261s timeout=False log=`experiments/uvm_comparison/results/verilator_2017_data1_test_r1.log`
- `verilator_2017_data1_test_r2` rc=0 wall=0.248s timeout=False log=`experiments/uvm_comparison/results/verilator_2017_data1_test_r2.log`
- `verilator_2017_data1_test_r3` rc=0 wall=0.259s timeout=False log=`experiments/uvm_comparison/results/verilator_2017_data1_test_r3.log`
- `verilator_2017_random_test_r1` rc=0 wall=0.242s timeout=False log=`experiments/uvm_comparison/results/verilator_2017_random_test_r1.log`
- `verilator_2017_random_test_r2` rc=0 wall=0.252s timeout=False log=`experiments/uvm_comparison/results/verilator_2017_random_test_r2.log`
- `verilator_2017_random_test_r3` rc=0 wall=0.238s timeout=False log=`experiments/uvm_comparison/results/verilator_2017_random_test_r3.log`
- `verilator_2017_many_random_test_r1` rc=0 wall=0.270s timeout=False log=`experiments/uvm_comparison/results/verilator_2017_many_random_test_r1.log`
- `verilator_2017_many_random_test_r2` rc=0 wall=0.271s timeout=False log=`experiments/uvm_comparison/results/verilator_2017_many_random_test_r2.log`
- `verilator_2017_many_random_test_r3` rc=0 wall=0.289s timeout=False log=`experiments/uvm_comparison/results/verilator_2017_many_random_test_r3.log`
- `verilator_coverage_annotate` rc=0 wall=0.766s timeout=False log=`experiments/uvm_comparison/results/verilator_coverage_annotate.log`
- `verilator_coverage_rank` rc=0 wall=0.270s timeout=False log=`experiments/uvm_comparison/results/verilator_coverage_rank.log`
