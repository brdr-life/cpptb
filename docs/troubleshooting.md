# Troubleshooting

The diagnostics cpptb emits are deliberately specific — each one names the
key, path, or clock it is about. This page collects them in one place:
what each message means, and the fix. Messages are grouped by when they
appear.

## Project and build errors

These stop the run before compilation and exit with code `2`; the message is
on stderr, prefixed `cpptb:`.

| Message | Meaning and fix |
|---|---|
| `build.timing_backend must be one of ('verilator-direct', 'vpi')` | Only the two contract-complete backends are accepted. Pick one — or delete the key, since `verilator-direct` is the default. See [Timing backend support](scheduling.md#timing-backend-support) |
| `build.verilator_args must not pass --binary` … | The timing backend owns the host loop and emits its own `--cc --exe` link; a `--binary` main dispatches no phases. Remove the argument |
| `design.defines`/`build.cxx_flags` `must not set 'CPPTB_…TIMING'` | The timing defines are owned by `timing_backend` so an incomplete phase bridge cannot be assembled by hand. Remove the define and set the key |
| `build.verilator_args must not set OPT_FAST` | Two optimization settings would leave whichever Verilator applies last in charge. Use `build.optimization`, which covers the testbench and the model together |
| `could not find … verilator_timing_main.cpp; looked in: …` | The framework host loop was not found from the resolved framework root. Point `--framework-root` at a cpptb checkout or install prefix |
| `fatal error: 'lz4.h' file not found` in an FST `--wave` build (macOS) | Verilator's FST writer needs lz4, and Apple clang does not search Homebrew's include tree: `brew install lz4 zstd`, then export `CPATH="$(brew --prefix)/include"` and `LIBRARY_PATH="$(brew --prefix)/lib"` |
| An ambiguous top module | More than one candidate root elaborates; the message lists them. Select one with `--top` or `[design] top` |

## Testbench compile errors

- **A `static_assert` naming a hierarchy path.** The discovery scan did not
  see that access, so the generated catalog has no entry for it — typically
  because the access sits behind a preprocessor branch that was inactive
  during the discovery compile, or the testbench file was added without a
  rebuild. Make the access visible in a compiled translation unit and
  rebuild (`--rebuild` if the cache disagrees). See the discovery step in
  [How a build works](how-it-works.md).
- **`hierarchy scope array index is out of range`.** A literal index into a
  scope array is outside the declared bounds; the valid range is part of the
  generated type. Runtime (non-literal) indices are checked at run time
  instead, with the scope path and valid indices in the message.
- **A misspelled signal is an ordinary compile error** — the `Dut` struct
  simply has no such member. `cpptb-codegen --inspect-hierarchy` prints what
  was generated.

## Run-time errors

| Message | Meaning and fix |
|---|---|
| `ERROR <test>: simulator did not produce a result file` | The simulator process died before writing its result. The reason is in `build/cpptb/<target>/results/<test>.log` — usually one of the aborts below, a crash, or a `$fatal` |
| `result file has an unsupported schema version` | The binary and the runner disagree on the result schema — a stale build against a newer checkout. Rebuild (`--rebuild`) |
| `unknown test '…'; available tests: …` | Test names are the registered C++ function names. `cpptb list` prints the compiled catalog |
| `clock '…' period must be positive and even` | `start_clock` needs a period that splits into two equal half-periods. See [Clocking](clocking.md#input-clocks) |
| `clock '…' timing is not representable at the simulator precision` | Period halves and phase must be whole multiples of the simulator precision |
| `clock '…' must be a writable one-bit DUT port` | Only a one-bit input can be scheduler-driven. A DUT-produced clock is awaited, not started — see [DUT-produced clocks](clocking.md#dut-produced-clocks) |
| `clock '…' has conflicting configurations across tests` | Two tests in one binary register the same pin with different periods or phases. Per-test configurations are legal only when they agree |
| `cannot await ReadWrite/ReadOnly after ReadOnly in the same timestep; await NextTimeStep{}, Delay{…}, or an edge first` | Phase order within a timestep is enforced. Leave `ReadOnly` before asking for an earlier phase — see [Simulator phases](scheduling.md#simulator-phases) |
| A write rejected during `ReadOnly` | `set()`, `deposit()`, `force()`, and `release()` are illegal in the sample phase, and legality is checked at the call — the failure points at the offending line, not the flush |
| `simulator watchdog expired` | The in-simulation cycle watchdog (`run.timeout_cycles`, default 1,000,000) fired. The wait graph printed with it shows every parked process and what it waits on — see [Scheduling](scheduling.md#wait-graphs-and-deadlock-diagnostics). Raise the key only when the test legitimately runs longer |

## Reading a hang

A test that times out — by `run.timeout_cycles`, by a registered
`simulation_timeout`, or by end-of-simulation starvation — captures the
scheduler wait graph *before* cancellation, prints it, and stores it in the
result JSON. The graph names each process, its spawn site, and the exact
edge, phase, event, queue, lock, or semaphore it is parked on, with a
conservative deadlock classification. Start there rather than with a
waveform: it usually names the missing `set()` or the never-published event
directly.

## Exit codes

`0` — every selected test passed. `1` — a test failed or errored. `2` — the
run never started (project, configuration, generation, or build error). See
[cpptb command line](cli.md#exit-codes).
