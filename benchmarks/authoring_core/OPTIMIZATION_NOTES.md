# Authoring-core optimization notes

## Implemented

- Compile C++ DPI and pure-SystemVerilog authoring binaries symmetrically with
  `OPT_FAST=-O3`. A 16-pair, 300,000-iteration experiment measured the C++ DPI
  task-value CPU ratio at about `0.952x` relative to pure SV, with exact
  semantic counters and checksum.
- Require a second 16-pair batch before an initial confirmed threshold crossing
  can become a hard failure.

## Profile summary

A 10-second macOS sampling profile of a 10,000,000-iteration task-value run
showed that most time is shared Verilator timing and DPI scheduling. The
incremental synchronous `Task<uint32_t>` path consists of a coroutine-frame
allocation plus scheduler adopt, finish, and reclaim bookkeeping.

Matched 300,000-iteration experiments measured:

- C++ task value versus C++ control: about `1.009x` total CPU.
- SV task value versus SV control: about `1.002x` total CPU.
- Net synchronous-task architecture cost: roughly `0.7%`.

## O3 regression snapshot

The full serial feature regression on 2026-07-11 completed without a hard
failure. Valid paired medians ranged from `0.6695x` to `1.0711x` relative to
the matching pure-SV testbenches. Clock cycles (`1.0619x`), wait-until
(`1.0517x`), and channel (`1.0399x`) were `passed_inconclusive` after 32 pairs:
their medians passed, but environmental variance left the one-sided 95% upper
median bounds above `1.10x`.

The focused task-value run exercised the new confirmation path and passed at
`1.0142x` after 32 pairs. Its later full-regression sample passed at `1.0138x`
after the initial 16 pairs, so the saved `latest` artifact contains that newer
measurement.

## Parked

These ideas remain available if future profiles show a meaningful need:

1. Add a size-bucketed coroutine-frame free list to reduce task frame
   allocation and destruction overhead.
2. Lazily adopt awaited tasks into the scheduler only when they first park,
   allowing synchronous tasks to skip scheduler state registration.
3. Apply generated DPI output words only when `STEP_OUTPUTS_CHANGED` is set.
   The initial experiment measured about `1.3%` CPU improvement, but opposing
   order strata made the result inconclusive.
4. Replace per-deadline generated timer processes with a persistent timer
   service.
5. Add authoring A/A admission checks, randomized pair order, and a secondary
   child-CPU diagnostic alongside the wall-time guard.

Frame pooling and lazy adoption should remain behind focused microbenchmarks:
their expected gain is small compared with compiler optimization, while lazy
adoption changes cancellation and timeout ownership.
