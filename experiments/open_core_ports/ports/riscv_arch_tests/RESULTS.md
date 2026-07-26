# RISC-V architectural tests: cpptb versus the upstream harness

The second port, and a deliberately different measurement from the first.
`ports/ibex_simple_system` runs one 40.7-million-cycle program and reports
steady-state throughput. This runs 98 programs of about 190,000 cycles each,
where what dominates is the cost of a run rather than the cost of a cycle.

Both sides drive the same Ibex RTL at the same pinned commit, from the same
test binaries, under the same Verilator 5.050. What differs is the framework.

Reproduce with `python3 build_tests.py && python3 run_suite.py`.

## The workload

96 architectural tests from riscv-non-isa/riscv-arch-test, plus two baseline
programs described below. Each test is generated assembly that exercises one
instruction across hundreds of operand cases and writes every result to a
signature region in memory.

Which of the suite's 641 rv32i tests apply is decided by each test's own
`REQUIRED_EXTENSIONS` header against what the Ibex small configuration
implements, so the selection is derived rather than maintained by hand.
`build_tests.py --list` prints the 545 that do not apply and why.

## Result

| | tests | upstream | cpptb | |
| --- | ---: | ---: | ---: | --- |
| I | 39 | `8627 ms` | `9244 ms` | `1.071x` |
| Zca | 26 | `5649 ms` | `5977 ms` | `1.058x` |
| M | 8 | `1808 ms` | `2017 ms` | `1.115x` |
| Misalign | 5 | `1030 ms` | `1128 ms` | `1.096x` |
| MisalignZca | 4 | `858 ms` | `885 ms` | `1.031x` |
| Zihintntl | 4 | `871 ms` | `944 ms` | `1.083x` |
| ZihintntlZca | 4 | `848 ms` | `924 ms` | `1.090x` |
| Zmmul | 4 | `918 ms` | `988 ms` | `1.076x` |
| Zifencei | 1 | `217 ms` | `212 ms` | `0.974x` |
| Zihintpause | 1 | `222 ms` | `220 ms` | `0.990x` |
| **total** | **98** | **`21088 ms`** | **`22552 ms`** | **`1.069x`** |

98 of 98 passed. Passing means more than not crashing: the two harnesses
produced byte-identical signature digests over identical word counts, and
cpptb's independent read of the same memory through the backdoor agreed with
what the program itself computed. A disagreement invalidates the comparison
rather than being reported as a difference in speed.

## Where the time goes

`1.069x` here against `1.140x` for CoreMark is the interesting part, because a
framework with more per-run overhead would look *worse* on short programs, not
better. Two baseline programs in the suite measure that directly instead of
inferring it.

`null` halts immediately, so its wall time is what a run costs before any
simulation happens. `null-loaded` is the same program padded to the size of a
real test image, so the difference between them is the cost of getting the
program into memory. Medians of seven runs:

| | upstream | cpptb | |
| --- | ---: | ---: | --- |
| process start, model construction, reset | `14.9 ms` | `10.9 ms` | `0.73x` |
| loading a 165kB image | `7.9 ms` | `5.8 ms` | `0.73x` |
| simulating a typical test | `195.2 ms` | `221.2 ms` | **`1.133x`** |

cpptb starts a run about 27% faster and loads a program about 27% faster. It
simulates about 13% slower. The suite total is the sum of those working against
each other, which is why it lands below the CoreMark ratio.

The load result is worth stating plainly because it is the opposite of what the
CoreMark port assumed. There, cpptb loading a VMEM at elaboration was recorded
as a difference that "favours neither". Measured, writing 41,000 words through
the memory backdoor is faster than parsing an ELF through libelf and pushing it
across the `simutil_memload` DPI export.

The simulation figure is the cross-check. `1.133x` on 190,000 cycles of
hand-written assembly and `1.140x` on 40.7 million cycles of compiled C are two
independent measurements of the same per-cycle cost, on workloads that share
nothing but the core. That agreement is stronger evidence for "cpptb costs about
13% per cycle on this design" than either number alone, and it retires the
possibility that the CoreMark figure was a startup artifact.

Individual runs vary by a few percent on this host and the per-group ratios
above range from `0.97x` to `1.12x` on samples as small as one test, so the
total and the baselines carry the weight. One host, one design.

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

## What this establishes, and what it does not

Agreement between the harnesses means they drove the RTL identically: same
reset, same memory image, same clocking, same run length. Any divergence in the
port would move the digest.

It does not mean Ibex is architecturally correct. That question needs a
reference model, and the suite's normal flow answers it by running each test on
Sail to capture a golden signature and rebuilding the test with that signature
linked in to self-check. This port deliberately does not do that: it compares
two harnesses driving one design, and adding Sail would answer a question Ibex's
own CI already answers.

## What this exercise found

Nothing in cpptb this time. The four framework defects the CoreMark port
uncovered stayed fixed, and the design elaborated and ran unchanged.

Three things in the port itself, all of which cost real debugging time:

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

And one gap in the suite's coverage for this class of core: the six `Zicsr`
tests pick a scratch CSR from a fixed ladder — `fflags` if F, `vxsat` if V,
`mepc` if U-mode is absent, nothing if Zicntr — and a core with U-mode but no
F, no V and no `time` CSR falls off the end into `#error`. They are excluded
rather than compiled with `ZICNTR_SUPPORTED`, whose branch is `li x11, 0` and
would be six passing tests that exercise no CSR at all.
