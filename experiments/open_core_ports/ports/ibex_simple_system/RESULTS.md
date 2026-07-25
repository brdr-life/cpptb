# Ibex Simple System: cpptb versus the upstream harness

The first port. Both sides run the same design at the same pinned Ibex commit,
the same CoreMark binary, and the same Verilator 5.050, so what differs is the
framework driving the simulation.

Reproduce with `python3 compare.py --runs 5`.

## The workload

CoreMark, 10 iterations, about 4.14 million cycles and 2.75 million retired
instructions per run. It validates its own result, so a run that computed the
wrong answer is detectable rather than merely fast. Every run on both sides is
checked for that evidence, and a mismatch invalidates the comparison instead of
being reported as a speedup.

## Result

Ten alternating runs on one idle host, five per side, every run passing its
semantic check.

| | median wall | cycles | CoreMark |
| --- | ---: | ---: | --- |
| upstream harness | `4571 ms` | 4136197 | validated, `1.230458` |
| cpptb port | `5250 ms` | 4136198 | validated, `1.230458` |

**`1.148x`**, cpptb over upstream. Both produce `Total ticks: 4063528`,
10 iterations, and the same score. The one-cycle difference in the reported
cycle count is where each side starts counting, not a difference in the run.

This is one host and one workload. It is a real measurement of a real workload,
not a claim about the framework in general, and it has not been repeated on
other hardware.

## What the number does and does not include

Both sides run with `+ibex_tracer_enable=0`. Left enabled, Ibex's tracer writes
every retired instruction to disk, which costs more than the simulation itself:
the same 4,136,197 cycles take `9.687s` with the tracer and `4.694s` without,
and produce a 281MB log. A comparison that did not fix this on both sides would
mostly measure file I/O.

The upstream harness loads firmware from an ELF at run time through libelf and
the `simutil_memload` DPI export. The cpptb port loads a VMEM during
elaboration through the `SRAMInitFile` parameter, so it links no ELF reader.
That is a real difference in what each binary does, and it favours neither in
steady state, since both finish loading before the measured run begins.

## Ergonomics

| | upstream | cpptb port |
| --- | ---: | ---: |
| testbench source | 152 lines of C++ | 71 lines of C++ |
| supporting harness | `verilator_sim_ctrl`, `verilator_memutil`, `memutil_dpi`, `ibex_pcounts` from `vendor/lowrisc_ip` | none |
| build description | fusesoc core files plus edalize | one `cpptb.toml` |
| system dependencies | libelf, lz4, srecord, fusesoc | none beyond Verilator |

The upstream harness is not 152 lines in isolation; it sits on shared C++
support vendored from `lowrisc_ip` for reset sequencing, memory loading and
performance-counter reporting. The cpptb port replaces all of it with the
framework, and its `cpptb.toml` names the sources directly, so building it
needs no fusesoc.

Against that, the port needed the design's file list, include directories,
defines and parameters transcribed from what fusesoc generates. That
transcription was mechanical but not automatic, and is the least pleasant part
of the port.

## What this exercise found in cpptb

Four framework defects, each a blocker that a design of this size hits and the
existing test suite did not.

1. **Packed type names collided.** `ibex_multdiv_fast.sv` declares a different
   `mult_fsm_e` in each of two generate blocks. Both wanted the C++ name
   `MultFsmE` and codegen refused. Names are now qualified by their declaring
   scope when they collide, which also unblocked hierarchy access to all 2120
   signals.
2. **Parameters were passed twice.** The generated wrapper is the top module
   and already binds parameters on the DUT instance, but `-G` was also passed
   to Verilator, which rejected parameters the top does not have. No project in
   the repository used `design.parameters`, so this path was untested.
3. **String parameters were emitted as `int`.** `localparam int SRAMInitFile =
   "firmware/coremark.vmem"` coerces the path to a number, and the memory
   loaded nothing while the build succeeded. String-valued parameters are now
   emitted untyped.
4. **The watchdog was not configurable.** `cpptb build` hard-coded a 1,000,000
   cycle timeout, which stops CoreMark a quarter of the way through. It is now
   `[run] timeout_cycles` in `cpptb.toml`.

## What the exercise found about porting

The reset sequence is the subtle one. Ibex's flops reset on `negedge rst_ni`,
so a testbench that holds reset low from time zero never produces that edge.
The core then boots in user mode instead of machine mode, and the only symptom
is the firmware taking an illegal-instruction trap on its first machine-mode
CSR access, thousands of cycles later and far from the cause. Upstream avoids
this by starting with reset released and then asserting it, which the port now
does too.

Diagnosing that needed hierarchy access to `priv_lvl_q`, which was itself
blocked by the first defect above.
