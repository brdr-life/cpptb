# Ibex's UVM testbench on Verilator 5

`dv/uvm/core_ibex` is Ibex's real verification environment: a UVM testbench with
an interrupt agent, a debug agent, two memory-interface agents, an RVFI
scoreboard against Spike, and around 57 test classes. Upstream's
`yaml/rtl_simulation.yaml` names six simulators -- VCS, Questa, DSim, Riviera,
qrun, Xcelium -- and Verilator is not one of them.

**Status: the testbench builds, elaborates and runs the whole environment on
Verilator 5.050.** UVM comes up, the factory resolves a test by name, both
memory agents serve the core, the stimulus sequences run, and the RVFI
scoreboard co-simulates against Spike -- the last of those being the part that
makes this a baseline rather than a demo.

**The core executes in lockstep with Spike.** Three bugs stood between the
build and a running test, all of them X-initialisation or solver behaviour that
only shows up on a 2-state simulator; all three are found, reduced and fixed
below. `core_ibex_base_test` now retires instructions with no cosim mismatch.

**No test completes yet**, for a different reason: throughput. The run reaches
about 195 cycles per second, so a riscv-dv program does not finish. See
"Still open: throughput".

## Why this matters here

Everything else in `experiments/open_core_ports` compares a cpptb port against
an upstream flow running the same stimulus. For flow 5, the riscv-dv random
programs, there was no upstream side to compare against, because the upstream
side is UVM and nothing local could run it. `ports/riscv_dv` says so in as many
words. This is that missing baseline.

## Building

```sh
python3 ../../fetch.py uvm_core     # Accellera's UVM, pinned in sources.toml
python3 build_tb.py --config small
```

An SMT solver is required at run time, not build time. Verilator does not solve
constraints itself: it shells out to `z3 --in`. Without one, every
`randomize()` in the testbench fails and the failure reads as a testbench
problem -- `Randomization failed!` from `` `DV_CHECK_RANDOMIZE_FATAL `` -- with
the real explanation only in a warning at the top of the run.
`local_deps.py` now installs z3 alongside the other packages:

```sh
python3 ../../local_deps.py
eval "$(python3 ../../local_deps.py --env)"
```

`--jobs` defaults to half the cores. The C++ compile of UVM plus the design
peaks at well over a gigabyte per job, and `-j 12` on a 16 GB machine is killed
by the OOM reaper part way through. The finished binary is about 270 MB.

The sources come from upstream's own `ibex_dv.f` and `ibex_dv_defines.f`,
expanded rather than transcribed, and the parameters come from
`util/ibex_config.py` -- the same tool `scripts/compile_tb.py` calls -- with
`-pvalue+` translated to Verilator's `-G`. An upstream change to either is a
change in what gets built, not a silent divergence.

## What it took

Eleven source edits and two C++ shims. All of them are applied to copies under
`build/overlay/` by `build_tb.py`, never to the fetched tree, and every one is
an exact-text replacement: if upstream changes those lines the build stops with
a message naming the file instead of quietly building something else.

They fall into four groups.

### 1. lowRISC's DV code excludes itself under Verilator

`clk_rst_if.sv` wraps its UVM includes and imports in `` `ifndef VERILATOR ``,
so under Verilator the interface compiles without `` `DV_CHECK_FATAL `` and
fails at the first use of it. `prim_assert.sv` does the same thing in the RTL:
it includes the dummy assertion macros and, alone among its branches, does not
define `INC_ASSERT` -- which is what `unused_assert_connected` in
`prim_count.sv` lives inside, and what `core_ibex_tb_top.sv` drives
unconditionally.

Both guards date from when no open simulator could elaborate UVM. Removing the
first and matching the second is most of what "porting" meant here.

Two other `` `VERILATOR `` guards are deliberately left alone: `pins_if.sv`
picks a strength-free `assign`, which Verilator genuinely needs, and
`dv_fcov_macros.svh` sets `DV_FCOV_DISABLE_CP`. The second is the switch to
flip when functional coverage is wanted.

### 2. A genuine LRM violation upstream

IEEE 1800 13.2.2 makes it illegal to write an automatic task's output argument
after a timing control. `csr_rd_sub` and `mem_rd_sub` in `csr_utils_pkg.sv` do
exactly that: they hand their own output formals to `uvm_reg::read` inside a
`fork ... join_any; disable fork`. Verilator enforces the rule and the
commercial simulators do not.

The fix writes automatic locals inside the fork and copies them out once it has
joined, which is what the code already means -- the outer `join` guarantees the
fork has finished before the task returns. Changing the formals to `ref`, the
usual advice, was tried first: Verilator rejects that too, which looks like an
over-strict reading of the same clause.

`csr_utils_pkg.sv` is in the compile only because `push_pull_agent_pkg` imports
`dv_lib_pkg` which imports it. Nothing in `dv/uvm/core_ibex` names either task.

### 3. Verilator features and bugs

| What | Where | Handling |
| --- | --- | --- |
| Clocking block with `default output negedge` | `irq_if.sv` | Event moved to `@(negedge clk)`; a write lands at the same edge |
| `force`/`release` inside an interface used as a virtual interface | `dv_macros.svh` | The two arms of `` `DV_CREATE_SIGNAL_PROBE_FUNCTION `` become a fatal; nothing in Ibex ever calls them |
| `$readmemh` into a class property | `core_ibex_base_test.sv` | Reimplemented with `$fscanf`, same load semantics |
| `event e = null;` fails to compile | `ibex_mem_intf_response_sequencer.sv` | Initialiser dropped; null is the default anyway |
| 1024-bit `uvm_hdl_data_t` used as a queue index | `core_ibex_test_lib.sv` | Narrowed with a part select |
| Covergroup transition bins over enum items | `core_ibex_fcov_if.sv` | **Not fixed.** See below |

The last four are Verilator bugs rather than missing features, and two announce
themselves as such: `event e = null` produces C++ with no matching `operator=`,
and the covergroup produces `%Error: Internal Error: AstNode is not of expected
type, but instead has type 'ENUMITEMREF'`. `int'(wide_value)` as a queue index
is a third -- it is another internal error, which is why the narrowing is a part
select and not a cast.

The `force`/`release` row is worth reading twice. Verilator represents a
forceable signal as `VlForceVec` and cannot build virtual-interface triggers for
one, so twelve signals in `core_ibex_dut_probe_if.sv` were errors at their
declarations. Every call site in the Ibex tree passes `SignalProbeSample`; the
force and release arms are dead code that cost the whole interface. They are
replaced by a fatal rather than deleted, so a future caller finds out.

### 4. The plumbing UVM needs and Verilator does not ship

`shims/uvm_dpi_verilator.cc` compiles UVM's vendor-neutral DPI -- reporting,
regex, and the plusarg list -- and supplies the HDL backend that `uvm_hdl.c`
refuses to pick, since it selects on `VCS`, `QUESTA` or `XCELIUM` and otherwise
stops at `#error "hdl vendor backend is missing"`. The backend here is plain
VPI, which is why the build passes `--vpi`: without it
`uvm_cmdline_processor` sees no arguments and `+UVM_TESTNAME` never reaches
`run_test()`.

`shims/date_c_linkage.cc` exists because `ibex_dv.f` lists a `.c` file and
Verilator compiles everything it is handed with `$(CXX)`, so `get_unix_timestamp`
came out name-mangled and the link failed.

### A trap worth knowing about

Verilator's skip-identical check hashes the command line and the files it read
last time. Neither notices when a source moves from the upstream tree into
`build/overlay/`, because an include path is not on the command line and the
upstream file has not changed. An overlay then silently has no effect and the
same error repeats, which reads exactly like the fix not working. The build
passes `--no-skip-identical`.

## Running

```sh
python3 build_programs.py --count 2 --instructions 300
cd build && ./obj/core_ibex_tb \
  +UVM_TESTNAME=core_ibex_base_test \
  +bin=elf/gen_0.bin \
  +signature_addr=8ffffffc \
  +timeout_in_cycles=4000000
```

`+bin` is a **flat binary**, not an ELF: `core_ibex_base_test` loads it byte by
byte with `$fread` from `` `BOOT_ADDR `` and hands the same file to the cosim
agent. Passing an ELF leaves memory at zero and the run dies a long way
downstream, with the core executing `0x00000000` and the double-fault detector
hitting its threshold.

`build_programs.py` generates with the same pyflow the `ports/riscv_dv` port
uses and links with riscv-dv's own `scripts/link.ld`, which already targets
0x8000_0000. It needs four patches to the generator, applied to a copy under
`build/pygen` by the same exact-text discipline as the SystemVerilog overlays.

Three are genuine bugs on pyflow's signature-handshake path, which is what
core_ibex uses to talk to the program and which evidently nobody has run:

| Bug | Effect |
| --- | --- |
| `test_result_t.TEST_RESULT` -- that member is on `signature_type_t` | `AttributeError`, generation aborts |
| `instr.extend(("a string"))` is not a tuple | the program comes out one letter per line |
| `instr_stream.append(instr)` where every other producer extends | a line reading `['li x22, 0x8ffffffc', ...]` |

The fourth is not a bug but the piece of Ibex's own riscv-dv customisation a
program cannot run without: `ibex_asm_program_gen.sv` overrides
`gen_program_header` to align `_start` to 0x80, because Ibex takes its reset
vector at `boot_addr + 0x80`. pyflow reads none of Ibex's SystemVerilog
extension, so `_start` lands at 0x8000_0000 and the core fetches zeros. Its two
debug-ROM jumps become self-loops here, because pyflow's `gen_debug_rom` is
`# TODO / pass` and the rv32imc target sets `support_debug_mode = 0`.

## Where it stops

**The DUT reads zero at its reset vector; Spike reads the program.** Every one
of the 22 mismatches is the same one:

```
Cosim mismatch DUT didn't write to register x5, but a write was expected
```

x5 is `t0`, and the first instruction of `_start` is `csrr t0, mhartid`. So the
mismatch is on instruction one: Spike executes the program, the DUT executes
`0x0000`. The tracer agrees -- its first entry is `80000080 0000 c.unimp`.

The backdoor load is not the problem. With `+UVM_VERBOSITY=UVM_FULL` the test
reports `Init mem [0x80000000] = 0x6f`, matching the first byte of the binary,
at time 201932; the first fetch is at 229900. The ELF has `_start` at
0x8000_0080. `$fread` into a scalar was suspected and cleared: it works on
Verilator 5.050 in isolation.

### One X-dependence found and fixed

Every agent starts its `run_phase` with

```systemverilog
wait (vif.<cb>.reset === 1'b0);
```

meaning "block until we are out of reset". A clocking block input has no
sampled value before its first clocking event, so a 4-state simulator reads X,
`=== 1'b0` is false, and the wait blocks as intended. **Verilator has no X**:
the sampled variable reads 0, the comparison is true at time 0, and every
driver and monitor is released while reset is still asserted. Reduced case,
with reset genuinely asserted until t=100:

```
0    wait on clocking-block input woke     <-- wrong
100  rst deasserted
100  wait on raw wire woke                 <-- right
```

Ten sites across seven files. `build_tb.py` fixes them by regex -- sample the
clocking block once before testing it, which is what a 4-state simulator gets
for free -- and fails the build if the count changes. `+verilator+rand+reset+1`
does not help: it does not reach clocking-block sampled variables.

This is real and it changed behaviour, but it is not the whole story.

### Fixed: a constraint Verilator silently drops

**Verilator silently ignores an inline constraint that references a variable
named `item`.** `randomize()` returns 1 and the target gets a random value.
The reproducer is `shims/verilator_constraint_item_name.sv`:

```
named item : addr=21766c9b expected=80000084
named itm  : addr=80000084 expected=80000084
```

Two handles of the same type, the same constraint, one name apart. Renaming to
`itm`, `req`, `rsp`, `m_item`, `member_item` or `data` all work; only `item`
fails. It happens for a local as well as a class member, and renaming the class
type makes no difference. With a plain scalar named `item` the generated C++
does not even compile: `'item' was not declared in this scope`.

`local::item.addr`, the LRM's explicit qualifier for "resolve this in the scope
that called randomize()", fails the same way, which rules out the unqualified
name falling back to the wrong scope.

(`this.item.addr` does not compile, with `Can't find definition of
scope/variable: 'item'`. That is not evidence of anything: inside an inline
constraint `this` refers to the object being randomized, so `this.item` asks
for a member the seq_item does not have, and the error is correct.)

`ibex_mem_intf_response_seq` ties its response to the monitored request with
`addr == item.addr`, and `item` is a member of the sequence. So every response
went to a random address, found it uninitialised, and returned `0x0000` -- which
is what the IMEM sequence is specified to do for uninitialised memory. The core
was answered with `c.unimp` at its reset vector and the cosim scoreboard
reported a mismatch on instruction one.

`build_tb.py` copies everything that `with` block reads into locals first.
After the fix the responses are correct and the core executes the program:

```
[MEMDBG I] t=215900 req.addr=80000080 item.addr=80000080 rw=0 data=f14022f3 uninit=0
[MEMDBG I] t=233900 req.addr=80000084 item.addr=80000084 rw=0 data=82634301 uninit=0

Time     Cycle  PC        Insn      Decoded instruction
261900   29     80000080  f14022f3  csrrs x0,mhartid,x0
```

`0xf14022f3` is `csrr t0, mhartid`, the first instruction of `_start`, fetched
and executed from the right address with the right data.

A note on how this was found, because it cost several rebuilds: five reduced
cases modelled on the constraint all worked, and the bisect only reproduced it
once it ran *inside* the real sequence. The distinguishing feature -- the
handle being a class member rather than a local -- is invisible from the
constraint text.

### Fixed: the asynchronous reset never fires

`clk_rst_if.sv` declares its reset driver with no initialiser and then asserts
it:

```systemverilog
logic o_rst_n;          // X on a 4-state simulator
...
o_rst_n <= 1'b0;        // apply_reset()
```

On a 4-state simulator that is **X -> 0**, a real falling edge, so every
`always_ff @(posedge clk or negedge rst_ni)` in the design takes its reset
branch. Verilator zero-initialises it, so the same assignment is **0 -> 0**.
There is no edge, the asynchronous resets never fire, and the design keeps its
zero-initialised state while `rst_n` sits at 0 looking perfectly asserted.

The symptom is three levels from the cause. `priv_lvl_q` resets to
`PRIV_LVL_M` (`2'b11`) and stayed at `2'b00`, so the core booted in **User
mode**; `illegal_csr_priv = (csr_addr[9:8] > priv_lvl_q)` then makes
`csrr t0, mhartid` an illegal CSR access, and the first instruction of every
program trapped without writing its destination register.

Starting `o_rst_n` deasserted gives `apply_reset` the 1 -> 0 edge it assumes.
Confirmed before writing the fix by running with `+verilator+rand+reset+1` and
`+2`, which make it start non-zero: both produce `priv_lvl_q=3`. Neither is a
fix -- they randomise every signal in the design.

```
before:  rd=0 wdata=00000000 trap=1 pc=80000080  priv_lvl_q=0  (User)
after:   rd=5 wdata=00000000 trap=0 pc=80000080  priv_lvl_q=3  (Machine)
         rd=6 wdata=00000000 trap=0 pc=80000084
```

This one is arguably lowRISC's rather than Verilator's: relying on X to
generate a reset edge is fragile, and the initialiser is the clean fix.

### The core now runs in lockstep with Spike

With all three fixes in, `core_ibex_base_test` executes the program and the
cosim scoreboard is quiet: **2,186 instructions retired, no mismatches**, PCs
advancing normally through the generated code. The run ends on the harness
timeout, not on a failure.

### Still open: throughput

A clean build reaches cycle 46,808 in 240 seconds -- about **195 cycles per
second**. Instrumented builds are far slower still; either way a full riscv-dv
program is out of reach. The likely cause is
that Verilator shells out to `z3` as a subprocess for every `randomize()`, and
the memory response sequence randomizes once per bus access. That is a cost per
transaction, not per cycle, so it dominates.

Measured, not guessed. `+verilator+solver+file+<path>` logs every SMT query:
over 10,270 cycles the run makes **2,545 randomize() calls and 86,059
`check-sat` round-trips** -- about 34 solver queries per randomize, and one
randomize every four cycles. Verilator appears to search bit by bit to get a
uniform solution, so the cost scales with the number of rand fields, not with
how constrained they are.

`ibex_mem_intf_seq_item` has nine rand fields and the response sequence pins
five of them to the monitored request by equality, so most of that work is
spent re-deriving values that are already determined. Reducing the randomize
footprint on that path is the obvious lever; `cfg.zero_delays` is not, because
it changes the delay value rather than the number of solver calls.

`rand_mode(0)` on the eight determined fields is applied and it helps, but not
by the order of magnitude the per-field theory predicted. Same program, same
wall clock, solver logging on both sides: **10,270 cycles before, 13,515
after**, about +30%. Queries per solve did drop sharply -- roughly 34 to 7 --
so the field-count theory holds for the cost of one solve, but the number of
solves rose enough to eat most of the gain, and the run-to-run variance on this
machine (163 to 195 cycles/s on unchanged builds) is wide enough that the +30%
should be treated as indicative rather than measured. Correctness is unchanged:
zero cosim mismatches.

So the solver is a real cost but not the whole story, and the next measurement
should separate simulation time from solver time rather than inferring one from
the other.

Worth trying, in order: `zero_delays` on the response agent config to see how
much of the cost is the `rvalid_delay` `dist`; and checking whether Verilator
can be pointed at an in-process solver rather than a subprocess.

Traced end to end with `--debug-mem`, which instruments three points: the
instruction bus in `core_ibex_tb_top`, the monitor's clocking-block samples
against the raw wires, and the response sequence's item.

Everything up to the response sequence is correct.

```
[TBDBG]  t=215900 | dut req=1 addr=80000080 gnt=1 | vif req=1 addr=80000080 gnt=1
[MONDBG] t=215900 cb: addr=80000080 req=1 gnt=1 | raw: addr=80000084 req=1 gnt=1
[MONDBG] t=233900 cb: addr=80000084 req=1 gnt=1 | raw: addr=80000088 req=0 gnt=1
[MEMDBG I] t=233900 req.addr=7674a22b item.addr=80000084 rw=0 data=00000000 uninit=1
```

The trace above shows the memory path working. What remains is one step later:
the tracer decodes the first instruction as `csrrs x0`, and the scoreboard says

```
Cosim mismatch DUT didn't write to register x5, but a write was expected
```

`0xf14022f3` encodes `rd = x5`, so `rvfi_rd_addr` is being reported as 0 where
Spike expects a write to x5. The instruction word, its address and its decode
are all correct, so this is a narrower question than the one before it: why
RVFI reports no destination register for an instruction that has one. It is the
next thing to look at.

The evidence that got here, for reference. The response sequence does

```systemverilog
if (!req.randomize() with {
      addr       == item.addr;
      read_write == item.read_write;
      data       == item.data;
      intg       == item.intg;
      be         == item.be;
      if (p_sequencer.cfg.zero_delays) { rvalid_delay == 0; }
      else { rvalid_delay dist { ... }; }
      error      == enable_error;
    }) begin
  `uvm_fatal(`gfn, "Cannot randomize response request")
end
```

and comes out with `req.addr = 0x7674_a22b`, a random value, **having returned
success**. The `addr == item.addr` constraint is silently not applied. Nothing
between that call and the print touches `req.addr`.

Everything downstream follows from it: the sequence reads the memory model at a
random address, finds it uninitialised, returns `0x0000` for it as the IMEM
sequence is meant to, and the core executes `c.unimp` at its reset vector.
That is the one bug behind 22 of the 27 test classes.

The reduction is not finished. Five candidate shapes were tried standalone and
all behave correctly: the equality alone, plus a constant-weight `dist`, plus a
variable-weight `dist` reading a config object, plus an enum equality, and a
null handle dereference inside a constraint (which Verilator catches loudly, so
`p_sequencer.cfg` is not null). So the trigger is something else in that block,
and the next step is to bisect the `with` block itself in the live testbench
rather than in a reduced case.

### Earlier suspicion, now ruled out

With `--debug-mem`, `build_tb.py` prints every memory response near the reset
vector: the address asked for, the data returned, and whether the memory model
had it. Over a whole run there is exactly one response, and it is for a wild
address:

```
[MEMDBG I] t=233900 addr=7674a22b rw=0 data=00000000 uninit=1
```

The instrumentation showed the agent does answer, at a random address, which is
why nothing appeared to answer `0x8000_0080`. Two other candidates were tested
and cleared with reduced cases: driving a wire vector from a clocking block
output works, and an unwritten clocking block output does not corrupt a wire the
DUT drives.

**One `randomize()` fails in the fetch-enable sequence.** Independent of the
above -- disabling that sequence does not change the mismatch.

```
%Warning-UNSATCONSTR: core_ibex_new_seq_lib.sv:19:
  Unsatisfied constraint: 'zero_delays dist {1 :/ zero_delay_pct,'
UVM_FATAL core_ibex_new_seq_lib.sv(97): Check failed
  (this.randomize(stimulus_delay_cycles)) Randomization failed!
```

The first iteration of the sequence succeeds and the second fails. Verilator
blames the `dist` constraint on `zero_delays`, which is a state variable in that
call holding 0 -- a value the distribution allows, so it should be satisfiable.
A reduced case with the same shape does not reproduce it, so this is not yet
root-caused. `+disable_fetch_enable_seq=1` gets past it and is what the runs
above use.

Worth knowing while reading those: Verilator is right, and stricter than the
commercial simulators, about `randomize(x)` keeping every other constraint
active over state variables. A reduced case confirmed that a state variable
outside its constraint's range makes the call fail. Some of what looks like a
solver bug in a UVM testbench is that rule being enforced.

## What is not done

- **Functional coverage.** `--fcov` exists but Verilator 5.050 dies with an
  internal error on the FSM transition bins. The three fcov sources leave the
  compile by default and `DV_FCOV_DISABLE` is defined.
- **Assertions.** `prim_assert.sv` gives Verilator the dummy macros, so the
  design's SVA is compiled out. Upstream's flows run with assertions on, and
  they are part of what those flows check. Turning them on means finding out
  how much of lowRISC's SVA Verilator 5 accepts.
- **Passing a test.** See "Where it stops" above. `python3 run_tests.py --list`
  prints upstream's 57 riscv-dv entries and the 27 UVM classes they map to;
  `run_tests.py` runs every class and tabulates where each one stops.
- **Per-test generation.** Upstream generates a different program per testlist
  entry, with that entry's generator options. `run_tests.py` runs every class
  against one program, which is enough to locate a shared blocker and not
  enough to be a regression. Until a program runs to its
  signature handshake, this is not yet the baseline `ports/riscv_dv` needs.
- **A runner.** Once tests complete, this wants the same both-harness reporting
  the other ports here have.

## Bugs worth reporting

In riscv-dv's pyflow, the three signature-handshake bugs in the table above.
They are one-line fixes and the path is plainly untested.

In Verilator, three internal errors, each with a small reproducer available from
the overlays: the covergroup transition bins over enum items, `int'()` on a wide
value used as a queue index, and `event e = null`. The `VlForceVec` virtual
interface restriction and the `ref`-argument reading of 13.2.2 are limitations
rather than crashes, but both are worth raising.

On the Ibex side, `core_ibex_tb_top.sv` drives `unused_assert_connected`
without the `` `ifdef INC_ASSERT `` that declares it, which breaks any tool
whose assertion macros are the dummy ones.
