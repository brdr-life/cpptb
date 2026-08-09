# RISC-V architectural tests: cpptb versus the upstream harness

The second port, and a deliberately different measurement from the first.
`ports/ibex_simple_system` runs one 40.7-million-cycle program and reports
steady-state throughput. This runs many programs of a few hundred thousand
cycles each, where what dominates is the cost of a run rather than of a cycle.

It runs in three configurations against two Ibex builds, and one of them
answers a different question entirely: with Spike bound in as a reference
model, per-instruction correctness rather than agreement between harnesses.

Both sides drive the same Ibex RTL at the same pinned commit, from the same
test binaries, under the same Verilator 5.050. What differs is the framework.

Reproduce with `python3 build_tests.py && python3 run_suite.py`; see
[README.md](README.md) for the other two configurations.

## The workload

Architectural tests from riscv-non-isa/riscv-arch-test, plus two baseline
programs described below. Each test is generated assembly that exercises one
instruction across hundreds of operand cases and writes every result to a
signature region in memory.

Which tests apply is decided by each test's own `REQUIRED_EXTENSIONS` header
and `params` block against what the configuration implements, so the selection
is derived rather than maintained by hand: 98 for `small`, 193 for `bmfull`.
`build_tests.py --list` prints those that do not apply and why.

`bmfull` roughly doubles the set. `RV32BFull` brings in Zba, Zbb, Zbc and Zbs;
`RV32ZcaZcbZcmp` brings in Zcb; `PMPEnable` with 16 regions brings in
`tests/priv/pmp`, which is most of the increase. It is also a materially
different core -- PMP checkers, a writeback stage, a branch-target ALU, a
single-cycle multiplier -- elaborated from identical RTL, so it exercises
cpptb's code generation rather than only running more programs.

## Result

Three configurations, each a run of the whole applicable suite.

| | Ibex config | tests | passed | cpptb / upstream |
| --- | --- | ---: | ---: | ---: |
| `small` | `small` | 98 | 98 | `1.086x` |
| `bmfull` | `maxperf-pmp-bmfull` | 193 | 177 | `0.997x` |
| `cosim` | `small` + Spike, both sides | 98 | 98 | `0.989x` |

**No test in any configuration had the two harnesses disagree.** That is the
result the port is judged on, and it is separate from whether a test passed:
passing means both harnesses produced byte-identical signature digests over
identical word counts, and cpptb's independent read of the same memory through
the backdoor agreed with what the program itself computed.

The 16 that did not pass are all PMP tests, and both harnesses say the same
thing about every one of them: 12 never complete, 3 take an early trap, 1
reports a failure. That is a statement about the core or about the tests, not
about the port, and `run_suite.py` reports the two categories separately so a
port that faithfully reproduces a core's behaviour cannot be made to look
broken by it. Whether Ibex or the tests are at fault is not established here.

### The portable timing backend

The same port also builds and runs unmodified with `timing_backend = "vpi"`,
the standard-VPI scheduler intended for simulators other than Verilator. The
`small` configuration passes 98 of 98 under it at `1.153x` against upstream
(the direct backend's `1.086x` plus the callback tax), with signature digests
identical to the direct-backend runs. The co-simulation configuration was
verified to build under vpi; its test programs are provisioned per-host and
were not staged on this machine, so it was not re-run.

## Where the time goes

`1.086x` on the small core and `0.997x` on the larger one is the interesting
part, and the direction is not obvious: cpptb reaches parity on the design that
is *more* expensive to simulate. Repeat runs of the whole suite move the total
by one or two percent -- `1.100x` and `1.086x` on two runs of `small` -- so the
third digit is not meaningful and the gap between the two configurations is.

Two baseline programs in the suite separate the fixed cost of a run from the
cost of simulating. `null` halts immediately; `null-loaded` is the same program
padded to the size of a real test image. Medians of fifteen runs, because at
this scale individual runs vary by a factor of two:

| | upstream | cpptb | |
| --- | ---: | ---: | --- |
| `small`, start a run | `15.3 ms` | `10.9 ms` | `0.71x` |
| `bmfull`, start a run | `16.1 ms` | `11.5 ms` | `0.71x` |
| `small`, simulate a test | `198.0 ms` | `218.2 ms` | **`1.102x`** |
| `bmfull`, simulate a test | `360.9 ms` | `370.7 ms` | **`1.027x`** |

cpptb starts a run about 29% faster, consistently, on both cores. It simulates
between 3% and 12% slower depending on the design. The suite total is those two
working against each other: on `bmfull` a test takes nearly twice as long to
simulate, so the fixed-cost advantage covers more of a smaller relative penalty
and the totals meet.

The per-cycle penalty falling from `1.102x` to `1.027x` on a bigger core is
what one would expect if cpptb's overhead is roughly constant per cycle rather
than proportional to the work: as each cycle costs more to evaluate, the same
overhead is a smaller share of it. Two designs is not enough to call that a
trend, but it is consistent with the CoreMark port's `1.140x`, which was
measured on the small core over 40.7 million cycles of a completely different
workload.

**A correction.** An earlier version of this file reported that cpptb loaded a
165kB image about 27% faster than upstream's libelf path, from a single pair of
measurements. With fifteen samples the difference between the `null` and
`null-loaded` baselines is 3-8 ms against a run-to-run spread of about 8 ms, so
it is not resolvable at this timing precision and that claim was not supported.
The fixed-cost figure above is, being four independent measurements that agree.

## Ergonomics

| | upstream | cpptb port |
| --- | ---: | ---: |
| harness written for this design | 123 lines of C++ | 142 lines of C++ |
| support it sits on | 1383 lines vendored from `lowrisc_ip` | none |
| system dependencies | libelf, lz4 | none beyond Verilator |
| firmware loading | ELF parsing through a DPI export | `mem[i].deposit(word)` |

The honest headline is that this testbench is *longer* than upstream's harness,
not shorter. It parses a VMEM, walks the image into memory and computes a
digest — work upstream gets from libelf and `MemArea` rather than work it
avoids. Counting only the file each project wrote makes cpptb look worse than
it is; counting the vendored support underneath makes it look better than it
is. Both numbers are above.

What is not a matter of counting is the memory path. Upstream reaches the RAM
through `MemArea`, `DpiMemUtil` and the `simutil_memload` SystemVerilog export,
which exists so that C++ can write an array it cannot otherwise see — 1383
lines of vendored machinery whose purpose is to cross that boundary. cpptb's
generated hierarchy makes the array a typed object and the crossing collapses
to one line. The 142 lines here are almost entirely the *test logic* that
upstream does not have, because upstream has no equivalent of running 98
programs from one build.

That is also what makes one build serve 98 tests. The CoreMark port loads
firmware through the `SRAMInitFile` parameter and `$readmemh` at elaboration,
which bakes the path into the build; doing that here would mean 98 elaborations
of a design that takes 56 seconds to elaborate. Loading at run time is not a
workaround for that, it is the thing the backdoor is for.

## Co-simulation

Agreement between two harnesses means they drove the RTL identically: same
reset, same memory image, same clocking, same run length. It does not mean the
core is right, because neither side knows what right is.

The `cosim` configuration does. It binds in Ibex's own co-simulation checker
from `dv/verilator/simple_system_cosim`, unmodified, so Spike runs in lockstep
and every retired instruction and every data memory access is compared against
it. A single test checks about 316,000 instructions this way.

**98 of 98 passed on both harnesses**, at `612 ms` median for cpptb and
`623 ms` for upstream, against `234 ms` for the same tests without a reference
model. Checking every instruction costs about 2.6x, and costs both sides the
same: `0.989x` cpptb over upstream.

### Both harnesses run Spike, and are compared

Co-simulation is a property of the build, not of the comparison, so upstream's
own `lowrisc:ibex:ibex_simple_system_cosim` target is built too and every test
runs under both. Each side carries its own Spike; only the framework driving it
differs. `run_suite.py` therefore requires that a co-simulation mismatch appear
on **both sides or neither** -- one harness seeing a divergence the other does
not would be the strongest possible evidence of a defect in the port, because
the reference model is identical.

That is checked against a real failure rather than assumed. Rebuilding the tests
without the `menvcfgh` workaround makes both harnesses fail, with the same
message and the same addresses:

    both saw a co-simulation mismatch:
      Synchronous trap was expected at ISS PC: 104900
      but the DUT didn't report one at PC 129200:  I-add-00
    both saw a co-simulation mismatch:
      Synchronous trap was expected at ISS PC: 103900
      but the DUT didn't report one at PC 129200:  I-addi-00

So the `menvcfgh` divergence is upstream's flow, reproduced, and not something
this port introduces. The same holds for the 16 PMP failures under `bmfull` and
for every other non-passing test recorded here: an error that does not appear on
both sides is reported as a port defect, not as a result.

### It is upstream's harness, not a reimplementation

The checker, the bind target, the DPI layer and Spike itself are Ibex's,
unmodified. Only `simple_system_cosim.cc` is replaced, because it subclasses the
harness cpptb exists to replace, and `cosim_glue.cc` passes the same arguments
it did: `GetIsaString()`'s `rv32imc`, `0x100080`, `0x100001`,
`add_memory(0, 0xFFFF0000)`, and the same five parameters read off the design.

That claim is checked rather than asserted, by running upstream's own
co-simulation workload through this port. Ibex's cosim README says to build
CoreMark with `SUPPRESS_PCOUNT_DUMP=1` because "the co-simulator system doesn't
produce matching performance counters in spike so any read of those CSRs results
in a mismatch and a failure". Both halves of that reproduce here:

| CoreMark build | result |
| --- | --- |
| default | mismatch at 40.7M cycles: `Register write data mismatch to x15 DUT: 1a45786 expected: 1a4b830` |
| `SUPPRESS_PCOUNT_DUMP=1` | **2,794,236 instructions matched, no mismatch**, CoreMark validated |

Reproducing upstream's documented failure *and* their documented fix is stronger
evidence that the port is faithful than a passing run alone would be.

### Why the workaround is the same kind upstream uses

The `menvcfgh` gap is not a consequence of running co-simulation differently. It
is a consequence of running a *wider workload* through it: upstream's cosim
flows run CoreMark and riscv-dv's generated programs, and neither writes that
CSR, so the disagreement has never had the chance to appear.

Upstream handles the same class of problem the same way -- by not exercising the
CSR that diverges, rather than by fixing the model:

- CoreMark must be built with `SUPPRESS_PCOUNT_DUMP=1`, above.
- riscv-dv's implemented-CSR list for Ibex has `MENVCFG // (lower 32 bits)` and
  no `MENVCFGH` entry at all, though it lists both halves of `mstatus`. Its
  `MINSTRET` and `MINSTRETH` are commented out under:

      // TODO: Bring back commented out CSRs, these are currently removed as
      // they can cause co-sim mismatches. These must be investigated and fixed

So `-DIBEX_NO_U_MODE` here is the same move at the same layer: adjust what the
workload exercises, not the reference model. The alternative is to add the
missing CSR to Spike, which is a few lines and would let these tests run with
U-mode declared truthfully, at the cost of carrying a patch against a pinned
dependency. That is worth reporting upstream either way.

Two caveats. The binaries are not identical to the `small` ones -- they skip the
boot-time `menvcfg` writes and nothing else a test exercises. And this
establishes correctness for the instructions these tests execute, a large but
finite set; it is not a proof.

## What this exercise found

Nothing in cpptb this time. The four framework defects the CoreMark port
uncovered stayed fixed, and both designs elaborated and ran unchanged.

Three things in the first configuration, all of which cost real debugging time:

1. **The discovery pass runs the testbench.** `cpptb build` compiles the
   testbench a second time with `-DCPPTB_HIERARCHY_DISCOVERY` and executes it to
   learn which signals are clocks and which hierarchy paths need a transport.
   The first version of this testbench returned early when `ACT_FIRMWARE` was
   unset, which it always is at build time, so discovery recorded a testbench
   that never starts a clock. The generated SystemVerilog came out with
   `CALENDAR_CLOCK_COUNT = 0`, nothing toggled `IO_CLK`, and every run died at
   time zero reporting "scheduler starvation" — a message about the scheduler
   for what was really a build-time problem. Anything a testbench touches must
   be reachable on every path, including paths taken when the environment is
   empty.

2. **The upstream ELF loader ignores absolute addresses.** `dpi_memutil.cc`
   places segments at `phdr.p_paddr - low`, relative to the lowest address in
   the file. The first linker script here left Ibex's 0x80-byte vector table as
   a hole, so the lowest address was the reset vector and the whole image loaded
   0x80 bytes low. The core fetched the wrong instruction at reset and spun in a
   trap loop. Nothing about the ELF looks wrong; only a trace shows it.
   `target/vectors.S` fills the table, and `build_tests.py` now refuses to emit
   an image that does not start at the RAM base.

3. **The suite has no "run on the DUT and report a signature" mode.**
   `-DSIGNATURE` makes `RVTEST_SIGUPD` store results, but
   `tests/env/riscv_arch_test.h` then includes `sail_macros.h`, which `#undef`s
   the target's halt and IO macros and redefines them for Sail's HTIF and CLINT.
   On Ibex the first store to Sail's CLINT traps. `-DRVTEST_SELFCHECK` suppresses
   that but requires a reference model. The port sets the header's own include
   guard, `-D_SAIL_MACROS_H`, which is not a supported switch because there is
   no supported switch.

## What the second configuration and the co-simulation found

Adding `maxperf-pmp-bmfull` and Spike lockstep turned up five more, none of them
in cpptb and all of them costly to diagnose.

1. **A `Cosim*` is not a `SpikeCosim*`.** `SpikeCosim` inherits from two bases,
   `simif_t` and `Cosim`, so the `Cosim` subobject does not start at the object's
   address. `get_spike_cosim` returns `void*`, which is exactly why the compiler
   cannot catch handing back the unadjusted pointer. Everything compiles, links
   and runs until the first retired instruction, at which point a call to
   `riscv_cosim_set_mip` arrives inside `mem_t::load_store` with a null buffer
   and segfaults. Upstream writes `static_cast<Cosim *>(...)` for this reason and
   the cast is load-bearing.

2. **`bit [31:0]` crosses DPI as a pointer, not a `uint32_t`.** Declaring
   `create_cosim`'s arguments as `uint32_t` compiles and links cleanly, then
   passes the low half of a pointer as the value. It surfaces from inside Spike
   as `error: bad number of pmp regions: '1373685940' from the dtb`. `bit` alone
   really is a scalar, so the two kinds sit side by side in the same signature.

3. **Spike has no `menvcfgh`.** `riscv/processor.cc` on the ibex_cosim branch
   registers `CSR_MENVCFG` and has no entry for the high half, so on RV32 a write
   traps. Ibex implements both as read-only zero, which is a legal WARL choice,
   and accepts it. The suite's boot code writes `menvcfgh` whenever U-mode is
   supported, so every test died in common code before reaching anything it
   tested. The reference model is the one behind the specification here, and it
   is worth being explicit that co-simulation found a disagreement in which the
   DUT was right. Upstream never hits it because its cosim flow runs CoreMark,
   whose startup does not touch `menvcfg`.

4. **Slang rejects implicit parameter shorthand.** Upstream's bind file writes
   `.SecureIbex,` for `.SecureIbex(SecureIbex)`. That is standard for ports and a
   Verilator extension for parameter overrides, so cpptb's slang front end
   refuses it. `cosim_bind.sv` writes them out; nothing else changes.

5. **PMP tests hang on the upstream harness.** Several `tests/priv/pmp` programs
   never terminate. The cpptb testbench stops itself at its cycle limit and says
   which test it was; the upstream harness has no cycle limit at all and runs
   until something outside kills it, which is why `run_suite.py` imposes its own
   timeout. Whether the tests or the core are at fault is not established here.

Two smaller ones worth recording:

- **cpptb rebuilds do not track sources added through `verilator_args`.**
  Editing `cosim_glue.cc` and rebuilding is a no-op, because only
  `[testbench] sources` and the RTL list are hashed. `--rebuild` is the
  workaround; the symptom is a fix that appears not to work.
- **cpptb rejects `.svh` in the source list.** Defensible, since `.svh` is
  conventionally an include, but upstream's fusesoc core compiles
  `cosim_dpi.svh` directly as SystemVerilog because it holds the DPI import
  declarations. `configure.py` copies it in as `.sv` rather than patching the
  fetched tree.

And one gap in the suite's coverage for this class of core: the six `Zicsr`
tests pick a scratch CSR from a fixed ladder — `fflags` if F, `vxsat` if V,
`mepc` if U-mode is absent, nothing if Zicntr — and a core with U-mode but no
F, no V and no `time` CSR falls off the end into `#error`. They are excluded
rather than compiled with `ZICNTR_SUPPORTED`, whose branch is `li x11, 0` and
would be six passing tests that exercise no CSR at all.
