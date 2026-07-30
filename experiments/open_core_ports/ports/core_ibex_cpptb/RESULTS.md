# core_ibex on cpptb, against the UVM baseline

Both harnesses drive the same Ibex, elaborated from the same fusesoc
description with the same parameters, and run the same programs compiled by the
same code. `ports/core_ibex_uvm` runs the UVM environment on Verilator 5;
`ports/core_ibex_cpptb` is that environment rewritten against cpptb, scoped to
`core_ibex_base_test` and `core_ibex_mcounteren_lock_test`, which between them
are 944 of the 944 directed entries. [README.md](README.md) says what is ported
and what is not.

Everything below is one run per column, named, and each run's `results.json`
names the logs it describes.

| | run | when |
| --- | --- | --- |
| UVM baseline | `ports/core_ibex_uvm/build/directed/opentitan-all944` | 2026-07-29 14:44Z |
| cpptb | `ports/core_ibex_cpptb/build/directed/cpptb-all944-final` | 2026-07-29 17:49Z |
| cpptb, spurious responses forced on | `ports/core_ibex_cpptb/build/directed/cpptb-all944-spurious` | 2026-07-29 17:40Z |
| replay | `ports/core_ibex_cpptb/build/replay/default9-v2` | 2026-07-29 17:42Z |
| injection | `ports/core_ibex_cpptb/build/inject/imem-add01`, `.../dmem30` | 2026-07-29 17:16Z |

## The directed testlist

`--config opentitan`, four entries at a time, all 944.

| | entries | UVM | cpptb |
| --- | ---: | ---: | ---: |
| **passed** | 944 | **912** | **912** |
| self-check failed | | 26 | 26 |
| build failed | | 4 | 4 |
| double faults | | 1 | 1 |
| timeout | | 1 | 1 |

Per group:

| group | entries | UVM passed | cpptb passed | what a pass is |
| --- | ---: | ---: | ---: | --- |
| riscv-tests | 93 | 89 | 89 | the program's own `RVTEST_PASS` |
| riscv-arch-tests | 107 | 105 | 105 | the RVFI co-simulation against Spike, and nothing else |
| epmp-tests | 744 | 718 | 718 | the exit code recovered from the trace, plus the co-simulation |

**943 of the 944 entries reach the identical outcome on the two harnesses, and
all 744 ePMP exit codes recovered from the execution trace agree exactly.**
`run_directed.py --against` makes that join and prints it as part of the run, so
it is in `cpptb-all944-final`'s own output rather than assembled afterwards:

```
against opentitan-all944 (opentitan, complete, 2026-07-29T14:44:50Z)
  943 of 944 entries reach the same outcome
    scall                       there=wall-clock timeout   here=cycle timeout
  744 of 744 ePMP exit codes recovered from the trace agree
  9,240 simulator-seconds there, 145 here (64x)
```

The one that differs is `scall`:

| | UVM | cpptb |
| --- | --- | --- |
| `scall` | wall-clock timeout | cycle timeout |

`scall` ends on a deliberate `ecall`, which `riscv_test.h`'s `trap_vector` sends
to `write_tohost`; neither testbench watches that address, so the entry can only
run out of something. It runs out of the 300-second wall-clock budget on the
baseline and out of the 5,000,000-cycle budget here, because cpptb reaches five
million cycles inside 300 seconds and the baseline does not. Same test, same
non-pass, and the cycle timeout is the more informative of the two.

The four build failures and the 26 ePMP self-check failures are the same
entries with the same details on both sides. They are upstream defects that
`ports/core_ibex_uvm/README.md` diagnoses in full: three vendored riscv-tests
use pre-1.10 CSR names, `jalr-01` writes `la x0, 5b`, and the 26 are two bugs
introduced by one commit on `lowrisc/riscv-isa-sim`'s `mseccfg_tests` branch.
Nothing about either harness moves them.

### What the run did

Counters the cpptb testbench keeps, summed over the 944:

| | |
| --- | ---: |
| cycles simulated | 14,335,393 |
| instructions retired | 881,470 |
| instructions co-simulated against Spike | 871,825 |
| synchronous traps | 9,625 |
| instruction-bus grants | 1,423,736 |
| data-bus grants | 188,167, of which 157,777 writes |
| PMP instruction-fetch errors | 75,619 |
| double faults seen | 100 |
| fetch-enable off pulses | 10,508 |
| scrambling-key answers | 218 |
| uninitialised instruction fetches | 192 |
| uninitialised data reads | 0 |

Two of those are worth reading as gaps rather than as work done. **No directed
entry reads uninitialised data memory**, so the dside path that returns random
data, writes it into both memory models and raises a bad-integrity response
never fires in this testlist -- on either harness. And `iside_errors` is zero:
nothing in the 944 injects a bus error on an instruction fetch, so
`run_cosim_imem_errors` and `run_cosim_prune_imem_errors` are ported and
exercised only to the extent of running.

## Timing

Same machine, same four-at-a-time, idle box.

| | UVM | cpptb | ratio |
| --- | ---: | ---: | ---: |
| wall clock, whole testlist | 2,365 s | 103 s | 23x |
| simulator-seconds, summed over the 944 | 9,240 | 145 | 64x |
| per entry, shortest | 1.81 s | 0.023 s | 79x |
| per entry, median | 7.72 s | 0.077 s | 100x |
| per entry, longest | 300 s (the budget) | 35.2 s | |

Per group, in simulator-seconds:

| group | entries | UVM | cpptb | ratio |
| --- | ---: | ---: | ---: | ---: |
| riscv-tests | 93 | 1,829 | 58.8 | 31x |
| riscv-arch-tests | 107 | 1,106 | 18.0 | 61x |
| epmp-tests | 744 | 6,304 | 67.8 | 93x |
| **total** | **944** | **9,240** | **144.6** | **64x** |

The ePMP column has the largest ratio because those entries are short and
numerous: 744 process starts, resets and backdoor loads, where cpptb's
per-run floor is 23 ms and the baseline's is 1.81 s. That floor is UVM coming
up, the factory resolving a test, and eight components building; the arch tests,
which run longer per entry, show the throughput ratio instead.

The two are not comparable per cycle from this table, because the two harnesses
do not simulate the same number of cycles on the same program -- the memory
delays are drawn from independent streams. The replay below is where the cycle
counts are equal by construction.

| | cycles | seconds | cycles/s |
| --- | ---: | ---: | ---: |
| UVM, while recording | 375,100 | 257.3 | 1,458 |
| cpptb, replaying | 375,100 | 3.30 | 113,714 |

78x, and the recording is not free: `add-01` takes 15.8 s in the plain run and
20.5 s with the recording on, and one of the ePMP entries 7.3 s against 13.1 s.
Discounting that, the underlying figure is between 45x and 55x, which is what
the whole-testlist number says as well.

### Build and edit times

| | UVM | cpptb |
| --- | ---: | ---: |
| rebuild after a testbench-source edit | 207 s | 133 s |
| rebuild with nothing changed | 30 s | 0.7 s |
| edit a *test* and see a result | no rebuild; 1.8 s and up | no rebuild; 0.024 s and up |

The last row is the one that matters for this testlist. The 944 entries are C
and assembly compiled by the runner, so changing one costs a compile and a run
on either harness and no simulator build at all.

The two rebuild figures are not measuring quite the same thing and should not be
read too closely. cpptb regenerates its bindings, recompiles the testbench and
re-links; Verilator's own dependency tracking then decides how much of the
design to redo, and for the baseline that depends entirely on which file the
edit touched -- the 207 s is an edit to a file most of the environment includes.

## Replay

Nine entries across the three groups. The baseline records every input of the
cpptb wrapper and thirteen fields of its outputs, one line per posedge; the port
drives the inputs at its drive point and compares the outputs at the edge.

| entry | group | cycles | instructions | recording | UVM s | replay s |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| `empty` | riscv-tests | 3,584 | 99 | 0.4 MB | 2.0 | 0.04 |
| `mcounteren_test` | riscv-tests | 73,790 | 5,030 | 7.7 MB | 40.8 | 0.84 |
| `access_pmp_overlap` | riscv-tests | 3,674 | 113 | 0.4 MB | 2.0 | 0.05 |
| `add-01` | riscv-arch-tests | 32,646 | 3,239 | 3.4 MB | 20.5 | 0.24 |
| `Fencei` | riscv-arch-tests | 3,579 | 88 | 0.4 MB | 1.8 | 0.05 |
| `cli-01` | riscv-arch-tests | 3,758 | 135 | 0.4 MB | 2.5 | 0.04 |
| `test_pmp_csr_1_lock00_rlb0_mmwp0_mml0_sec_00` | epmp-tests | 6,957 | 281 | 0.7 MB | 13.1 | 0.08 |
| `test_pmp_ok_1_u0_rw00_x0_l0_match0_mmwp0_mml0` | epmp-tests | 7,218 | 299 | 0.8 MB | 14.7 | 0.07 |
| `pmp_mseccfg_test_rlb1_l0_0_u0` | riscv-tests | 239,894 | 16,806 | 25.3 MB | 159.9 | 1.88 |
| **total** | | **375,100** | **26,090** | **39.5 MB** | **257.3** | **3.30** |

**Every one of the 375,100 cycles matched on all thirteen fields, and this
port's co-simulation scoreboard accepted all 26,090 instructions of the
baseline's own runs.** The instruction column is the same number twice: what the
baseline's `ibex_cosim_scoreboard` reported and what this port's `CosimBridge`
counted, on the same run, from two separate pieces of code. They agree on every
entry.

No divergence has been seen, on the first attempt or since. That is worth
stating precisely because it is unusual: the icache port's replay found a real
difference the moment it existed. Here it did not, and the reason is that the
one thing most likely to be wrong -- the half-cycle offset between a
clocking-block write at a negedge and one at a posedge -- was worked out from
the LRM before the first run rather than after it. What the replay does prove is
that it was worked out correctly: a `grant_driver` that waited one falling edge
fewer would put every grant a cycle early and diverge on the first bus access.

### Showing the replay is live

`IBEX_REPLAY_PERTURB=N` moves one bit of the first instruction returned at or
after cycle N.

| perturbed at | injected at cycle | caught at cycle |
| ---: | ---: | ---: |
| 500 | 505 | 506 |
| 5,000 | 5,653 | 5,677 |
| 20,000 | 20,379 | 20,380 |

The 24-cycle gap in the middle row is the pipeline: the corrupted instruction
was an `add` whose result did not reach a bus pin or the RVFI outputs until it
retired.

## Showing the checks are live

One bit of one memory read response per run, over a range of responses, with
`IBEX_NO_COSIM=1` as the control.

| | injections | caught | silent | crashed Spike | caught with the reference model off |
| --- | ---: | ---: | ---: | ---: | ---: |
| `add-01`, instruction bus | 40 | 38 | 0 | 2 | 29 |
| `pmp_mseccfg_test_rlb1_l0_0_u0`, data bus | 30 | 17 | 13 | 0 | **0** |

The data-bus row is the one to read. **Nothing in the program's own checking
caught a single corrupted load.** The co-simulation caught 17 of 30; the 13 it
did not are loads whose value never reached anything the program looked at,
which is the corruption being harmless rather than the check being asleep.

```
cpptb: testbench.cpp:1184: Cosim mismatch DUT generated load at address
  80000360 with data 5 but data 4 was expected with byte mask 1
```

On the instruction bus the program's own checking does catch most corruptions,
because a changed instruction usually breaks the program: 29 of 40 without the
reference model. The co-simulation catches 38 of 38, which is the other nine
plus everything the program would have caught anyway, and catches them at the
instruction rather than thousands of cycles later at a failed self-check.

Two of the forty instruction-bus injections end in
`*** stack smashing detected ***` from inside Spike.
`ports/core_ibex_uvm/README.md` records the same crash on two of its own
riscv-dv runs, so it is the reference model rather than the port, and
`inject.py` reports it as itself rather than as a catch or a silent pass.

## Two stimulus questions the comparison answered

### `zero_delays` is never set on the baseline

`ibex_mem_intf_response_agent_cfg` declares

```systemverilog
rand bit zero_delays;
constraint zero_delays_c { zero_delays dist {1 :/ zero_delay_pct,
                                             0 :/ 100 - zero_delay_pct}; }
```

with `zero_delay_pct` at 50, which reads as "half the runs have no protocol
delays at all". **Nothing ever randomizes that object.** `core_ibex_base_test`
creates the two configs with `type_id::create` and writes fields into them, and
no `randomize` in the tree names either, so `zero_delays` is 0 on every run and
both delay distributions are always drawn.

This port drew it at 50% for its first full run, which is what the constraint
reads as and not what the baseline does. It is a knob now,
`IBEX_ZERO_DELAYS`, defaulting to the baseline's behaviour. The run it affected
passed 911 rather than 912 for an unrelated reason and is superseded; the
current run is `cpptb-all944-final`.

### The 20% spurious-response draw fires 99.6% of the time on Verilator

`core_ibex_vseq::pre_body` enables spurious dside responses with

```systemverilog
enable_spurious_response dist {1 :/ cfg.spurious_response_pct,
                               0 :/ 100 - cfg.spurious_response_pct};
```

and `spurious_response_pct` is 20. **The baseline's logs say
`Enabling spurious responses for this test` on 940 of its 944 runs**, which is
99.6% and not 20%. That is a fourth instance of the `dist` behaviour
`ports/core_ibex_uvm/README.md` already documents three of.

So "what upstream asks for" and "what the baseline executes" are two different
runs, and the port can make both. `IBEX_SPURIOUS_DSIDE_PCT` defaults to
upstream's 20; at 100 it matches what the baseline actually does.

| | runs with spurious responses | spurious responses driven | passed |
| --- | ---: | ---: | ---: |
| cpptb, `IBEX_SPURIOUS_DSIDE_PCT=20` | 0 of 940 | 0 | 912 |
| cpptb, `IBEX_SPURIOUS_DSIDE_PCT=100` | 940 of 940 | 274,305 | 912 |
| UVM baseline | 940 of 944 | not counted | 912 |

Both cpptb runs reach the same 912 and the same 943-of-944 agreement with the
baseline, so the answer to "does the spurious-response path change any verdict
in this testlist" is no. It is exercised: 274,305 responses on the second run.

The first row is worth being precise about. With one seed for the whole
testlist, a per-run 20% draw either fires on every entry or on none of them,
because every entry makes the same draw. That is a real limitation of running
944 entries at one seed and it applies to the baseline as well, which is
presumably why the `dist` defect went unnoticed: 940 of 944 looks like a
decision that was made once.

## What this port checks less than the UVM environment

The full list is in [README.md](README.md) under "Divergences". The three that
change what is checked rather than how:

* **Eight of the ten test classes are not ported**, with the interrupt and debug
  agents. No directed entry reaches them; the riscv-dv entries that do are
  hollow on the baseline, by its own accounting.
* **Neither harness runs assertions or functional coverage.** `prim_assert.sv`
  gives Verilator the dummy macros, so the design's SVA compiles away on both
  sides, and `ports/core_ibex_uvm` builds with `DV_FCOV_DISABLE`. This port does
  not write `core_ibex_tb_top`'s `NoAlertsTriggered` assertion, which is the
  same amount of checking and less pretence -- but it means **neither harness
  notices a spurious alert**, and the alert outputs are checked only by
  `replay.py`.
* **The ordering inside a clock edge is fixed here and left to the scheduler
  there.** Upstream's cosim agent forks six tasks over five clocking blocks;
  here they run in one coroutine, data-side first and RVFI last. They agree, but
  one of them is making a defined choice and the other is not.

## Lines

| | lines |
| --- | ---: |
| upstream, the parts this port replaces | 3,833 SystemVerilog |
| upstream, the parts it does not (the eight classes, the interrupt agent) | 2,547 SystemVerilog |
| this port | 383 SystemVerilog + 1,968 C++ |
| this port's tooling | 1,467 Python, standard library only |

The SystemVerilog wrapper is 383 lines against `core_ibex_tb_top.sv`'s 425, and
about half of it is the port list the interfaces used to be. Nothing in the
2,351 lines of design-facing code is generated; the 136-source RTL list and the
18 parameters and defines in `cpptb.toml` are.
