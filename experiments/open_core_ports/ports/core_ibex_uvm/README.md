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

**The whole riscv-dv testlist is built and run.** 53 of the 57 entries have a
program; **30 of them pass on `--config opentitan` and 19 on `--config small`**,
with no plusarg workaround -- the fetch-enable stimulus runs. Separately, all
five integrity classes pass on `--config opentitan` now that the signals they
probe are visible to VPI.

**A pass is not the same as coverage of the entry, and the runner says which is
which.** Every entry carries a verdict -- 13 faithful, 12 partial, 28 hollow, 1
that no generator here produces -- and "hollow" means an option that defines the
entry was dropped, so the outcome is not evidence about the entry's name.
Seventeen of the thirty passes on `opentitan` are hollow, ten of them PMP
entries running a program with no PMP configuration in it. See "Where that
leaves it".

Getting from eight entries to 53 took six more pyflow bugs, one of which -- the
missing MSTATUS/MIE signature handshake -- was on its own the reason twenty
directed test classes died 10,000 cycles into every run without executing any of
their program.

**The other testlist runs: 912 of 944 directed tests pass.**
`directed_tests/directed_testlist.yaml` is 944 hand-written C and assembly
entries with no generator anywhere in the flow, and none of them had been
built here. All 944 build and run on `--config opentitan` now, and the 32 that
do not pass are named below. Getting a number that meant anything took five
fixes first, three of them upstream's -- the arch tests were compiling their
own bodies away, the ePMP linker script is stale by a megabyte, and no ePMP
test can report a failure to the harness at all. See "The directed tests".

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

### More than one configuration at a time

Each configuration builds into its own `build/obj_<config>`, and every runner
takes `--config` to pick which binary it uses. `small` stays the default.

```sh
python3 build_tb.py --config small
python3 build_tb.py --config opentitan
python3 run_tests.py    --config opentitan ...
python3 run_directed.py --config opentitan ...
```

This is not a convenience. `ibex_configs.yaml` gives `small` `PMPEnable: 0`,
`SecureIbex: 0` and `RV32BNone`, and a large part of what the two testlists
test is exactly those three. Until each configuration had its own directory
the only way to run a PMP test was to build over whatever was there, which
invalidated everything that had been measured before it -- which is how the
integrity tests came to be verified once and never re-checked.

A test whose `rtl_params` the built configuration does not satisfy is now
reported as **inapplicable**, which is neither a pass nor a failure. That is
the comparison upstream's `ibex_cmd.filter_tests_by_config` makes, against the
same field. What each testlist needs:

| | entries | `small` | `opentitan` |
| --- | ---: | ---: | ---: |
| `riscv_dv_extension/testlist.yaml` | 57 | 18 inapplicable | 1 inapplicable |
| `directed_tests/directed_testlist.yaml` | 944 | **944 inapplicable** | 0 inapplicable |

The 18 are five wanting `SecureIbex`, ten wanting `PMPEnable` and three naming
an `RV32B` value. The one on `opentitan` is `riscv_bitmanip_full_test`, which
wants `RV32BFull` where `opentitan` has `RV32BOTEarlGrey`. The three `RV32B`
entries never get as far as being ruled out: no program links for them at all,
for a reason that has nothing to do with the configuration. So a sweep reports
15 inapplicable on `small` and 0 on `opentitan`.

The second row is the important one: **all three configs in the directed
testlist carry `rtl_params: {PMPEnable: 1}`**, so not one of the 944 directed
tests means anything on the default build. The empty directed test on `small`
dies on `csrw pmpaddr0` at the fourth instruction of `INIT_PMP`.

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

That backend is only half of what UVM's HDL access needs. Verilator exposes a
signal to VPI only when it is marked public, and accepts a write only when it is
`public_flat_rw`; nothing is public by default. The integrity and glitch tests
reach into the DUT by name -- 22 `uvm_hdl_read`, `uvm_hdl_force` and
`uvm_hdl_release` calls in `core_ibex_test_lib.sv`, several with the path
assembled at run time -- and every one of them got a null handle back.
`shims/uvm_hdl_public.vlt` names the signals, grouped by the test class that
uses them, and `build_tb.py` passes it to Verilator.

`forceable` is a second, separate permission: `vpi_put_value` with
`vpiForceFlag` or `vpiReleaseFlag` is refused on a signal that does not have it.
Only the signals the tests actually force carry it, because `forceable` is what
makes a signal a `VlForceVec` and the row above records what that costs inside
an interface. None of these are in an interface.

Two things the control file cannot contain: a backtick, which starts a directive
even inside a `//` comment, and a comment whose first word is `verilator`, which
is read as a metacomment. Both were in the first draft of the header and both
stopped the build.

Five test classes use this: `core_ibex_pc_intg_test`, `core_ibex_rf_intg_test`,
`core_ibex_rf_addr_intg_test`, `core_ibex_ram_intg_test` and
`core_ibex_icache_intg_test`. Most of what they reach for only exists when
`SecureIbex`, `ICache` and `ICacheScramble` are set, so `--config small` resolves
the paths that are in that configuration and `--config opentitan` resolves all of
them. On `opentitan` all five run with no HDL lookup failure, and the force
reaches the design: `core_ibex_ram_intg_test` glitches a RAM control signal and
then checks the major alert unconditionally, and that check passes.

### A force is not substituted inside an array index

`core_ibex_rf_addr_intg_test` forces `raddr_a_i` or `raddr_b_i` on the register
file. Verilator accepts the force and reads the forced value back, but the
register file does not see it, because `ibex_register_file_ff` reads
`rf_reg[raddr_a_i]` and Verilator leaves the index reading the unforced
variable. Reduced case in `shims/verilator_force_array_index.sv`, one module,
both uses of the same forced signal:

```
[SV] base rdata=13 plus=04
[VPI] FORCE top.u_rf.raddr_i <- 0x7
[SV] forced raddr_i=7: rdata=13 (expect 17) plus=08 (expect 08)
```

`plus_o = raddr_i + 1` follows the force; `rdata_o = mem[raddr_i]` does not. The
generated C++ says the same thing: the arithmetic reads `raddr_i__VforceRd` and
the array select reads `raddr_i`. Native SystemVerilog `force` behaves
identically, so this is `V3Force` rather than the VPI path.

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
cd build && ./obj_small/core_ibex_tb \
  +UVM_TESTNAME=core_ibex_base_test \
  +bin=elf/gen_0.bin \
  +signature_addr=8ffffffc \
  +timeout_in_cycles=4000000 \
  +disable_fetch_enable_seq=1
```

or, for the testlist rather than one program:

```sh
python3 build_programs.py --all-tests --jobs 4
python3 run_tests.py --config opentitan --jobs 4
```

`build_programs.py --list-tests` prints each entry with the verdict its program
would carry -- faithful, partial or hollow -- before generating anything.

or, for the directed testlist, which needs no generator at all:

```sh
python3 build_tb.py --config opentitan
python3 run_directed.py --config opentitan --group riscv-tests
python3 run_directed.py --config opentitan            # all 944
```

`+bin` is a **flat binary**, not an ELF: `core_ibex_base_test` loads it byte by
byte with `$fread` from `` `BOOT_ADDR `` and hands the same file to the cosim
agent. Passing an ELF leaves memory at zero and the run dies a long way
downstream, with the core executing `0x00000000` and the double-fault detector
hitting its threshold.

`build_programs.py` generates with the same pyflow the `ports/riscv_dv` port
uses and links with riscv-dv's own `scripts/link.ld`, which already targets
0x8000_0000. It patches the generator to do it, applying the patches to a copy
under `build/pygen` by the same exact-text discipline as the SystemVerilog
overlays. They fall into three groups, all listed with their evidence in
`PYGEN_PATCHES`.

Three are bugs on pyflow's signature-handshake path, which is what core_ibex
uses to talk to the program and which evidently nobody has run:

| Bug | Effect |
| --- | --- |
| `test_result_t.TEST_RESULT` -- that member is on `signature_type_t` | `AttributeError`, generation aborts |
| `instr.extend(("a string"))` is not a tuple | the program comes out one letter per line |
| `instr_stream.append(instr)` where every other producer extends | a line reading `['li x22, 0x8ffffffc', ...]` |

Two are not bugs but the pieces of Ibex's own riscv-dv customisation a program
cannot run, or finish, without. `ibex_asm_program_gen.sv` overrides
`gen_program_header` to align `_start` to 0x80, because Ibex takes its reset
vector at `boot_addr + 0x80`; without it `_start` lands at 0x8000_0000 and the
core fetches zeros. And it overrides `gen_test_done` to write
`(TEST_PASS << 8) | TEST_RESULT` to `signature_addr - 0x4`, which is the single
thing `core_ibex_base_test::wait_for_test_done` waits for. pyflow generates
riscv-dv's stock ending instead -- `li gp, 1; ecall`, and an ecall handler that
falls into `write_tohost` -- which is the HTIF handshake Spike and Simple
System use and core_ibex has nothing that reads. See "Per-test programs".

The rest are generator bugs that only show up once programs get longer than a
few hundred instructions; they are in the same section.

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

## Per-test programs

Upstream generates a different program for each of the 57 testlist entries,
with that entry's own `gen_opts`, and runs it with that entry's `sim_opts`.
`build_programs.py --all-tests` does the same, and `run_tests.py` runs the
result.

### The reason every test timed out

A sweep of all 27 UVM classes against one generic program returned 20
`TEST TIMEOUT!!`, which looked like the programs being too long for the
simulator. It was not. The program ran to completion every time and then spun
here forever:

```
800003fc  auipc  x30,0x2
80000400  sw     x3,-1020(x30)   PA:0x80002000
80000404  c.j    800003fc
```

That is `write_tohost`, the HTIF ending riscv-dv generates by default. The
testbench is not watching for it: `wait_for_test_done` waits for a write of
`TEST_RESULT` to `signature_addr - 0x4`, which Ibex's `ibex_asm_program_gen.sv`
emits at `test_done:` and pyflow does not. One missing five-instruction
sequence, and no test in this environment can ever finish. With it, the same
program that had been timing out ends in under a second:

```
Test done due to RISCV-DV handshake (payload=TEST_PASS)
Co-simulation matched        190 instructions
--- RISC-V UVM TEST PASSED ---
```

### What pyflow cannot honour

pyflow is a separate implementation of riscv-dv's generator from the
SystemVerilog one upstream runs, and it reads none of Ibex's SystemVerilog
extension, so some `gen_opts` have no equivalent. None of them is dropped
silently: `build_programs.py` records each one against the test it came from in
`build/manifest.json`, and `run_tests.py` marks the tests whose program does
not match their entry and prints the reasons under the results.

| gen_opt | Why not |
| --- | --- |
| `pmp_*`, `mseccfg`, `enable_write_pmp_csr` | `riscv_pmp_cfg` has no Python implementation at all, `setup_pmp` and `gen_pmp_csr_write` are stubs, and no pyflow target sets `support_pmp` |
| `gen_debug_section`, `num_debug_sub_program`, `set_dcsr_ebreak`, `enable_debug_single_step`, `enable_ebreak_in_debug_rom` | `gen_debug_rom` is `# TODO / pass`, and no pyflow target sets `support_debug_mode` |
| `toggle_dit`, `toggle_dummy_instr`, `gen_all_csrs_by_default`, `add_csr_write` | Ibex's own extension, which is SystemVerilog |
| `uvm_set_type_override=...` | a UVM factory override; there is no factory here |
| `enable_zba_extension` and the other subset flags | pyflow's only B module is the draft v0.93 encoding; see below |
| `directed_instr_N=ibex_*` and two of riscv-dv's own streams | not in `riscv_utils.factory`, which is a literal dict of eleven names |
| `num_of_sub_program=N` | forced to 0; `insert_jump_instr` is `pass  # TODO`, so a sub-program would never be called |
| `no_csr_instr=0` | `csr_c` is `# TODO / pass` and there is no `riscv_csr_instr` class |
| `enable_access_invalid_csr_level`, `enable_dummy_csr_write`, `enable_misaligned_instr` | parsed, and read on a path pyflow never takes, or not read at all |
| `no_ecall=0` | pyflow has no `--no_ecall` and never puts ECALL in the pool |

Four rows are worth reading twice.

**`num_of_sub_program`** was recorded as a solver failure, and that was the
symptom rather than the cause. `gen_callstack` does die -- it calls
`self.callstack_gen.init(...)` on a local it has just named `callstack_gen` --
but fixing that would not help, because `riscv_instr_sequence.insert_jump_instr`
is `pass`, above a `# TODO riscv_jump_instr class implementation` with the whole
body commented out. That is the function that puts the jump into the caller, and
`insert_sub_program` appends the sub-program bodies *after* the jump to
`test_done`. A fixed `gen_callstack` would produce unreachable code, not a call
stack. Nine entries ask for sub-programs.

**`no_csr_instr=0`** needs more than the enum-against-string comparison in
`build_basic_instruction_list` corrected. Behind it, `riscv_instr.csr_c` -- the
constraint that keeps the address inside the implemented set -- is
`# TODO / pass`; pyflow has no `riscv_csr_instr` class, so `csr_addr_c`,
`write_csr_c` and the read-only-write rules do not exist; `create_csr_filter`
fills `include_reg` and `exclude_reg` with strings nothing reads; and
`convert2asm` formats the result as `0x{}` around the *decimal* value, so a CSR
drawn as 0x320 is written `0x800`. Honouring it means porting
`riscv_csr_instr.sv`, not correcting a comparison. Eight entries ask for it.

**The bitmanip entries do not link.** `enable_bitmanip_groups` *is* honoured --
`riscv_b_instr.is_supported` reads it -- and `--target rv32imcb` is the one
pyflow target whose `supported_isa` contains RV32B, so `build_programs.py`
selects it for those three entries rather than generating a B-free program that
would link cleanly and test nothing. But pyflow's only B module is
`isa/rv32b_instr.py`, which is bitmanip **draft v0.93**. What comes out is
`bfp`, `grevi`, `unshfli`, `cmix`, `crc32.h`, `sbclr`, `packu`, `fsri`, and
binutils 15.2 assembles none of them, with or without
`-march=...zba_zbb_zbc_zbs`. Upstream's own comment above those entries says the
same thing: "Both an updated compiler and ISS are required to verify the
bitmanip v.1.00 and draft v.0.93 extensions."

**`suppress_pmp_setup` and `disable_pmp_exception_handler` cost nothing.** Both
ask for the *absence* of something. `riscv_asm_program_gen::setup_pmp` emits an
allow-everything PMP configuration when the first is set, and Ibex's extension
skips its PMP exception handler when the second is; pyflow emits neither section
in the first place, and a program with no PMP entries configured is unrestricted
in M mode, which is where those two entries run. They are recorded and not
counted against the entry.

`gen_test` is honoured where pyflow has the test. Forty entries name
`riscv_rand_instr_test`, which pyflow does have -- with four of its seven
directed streams commented out. Passing `--gen_test` matters for a second
reason: `riscv_instr_base_test.py` runs its test at import, guarded by
`cfg.argv.gen_test`, and `riscv_rand_instr_test.py` imports it, so without the
flag both run, nested inside each other's multiprocessing pool, and the second
one hangs.

### Seven more pyflow bugs, from generating at the sizes the testlist asks for

6,000 to 20,000 instructions rather than a few hundred turned up seven more, all
patched the same way. Three are the same mistake: a name pushed into an
instruction pool as a string where the pool holds `riscv_instr_name_t` members.

| Bug | Effect |
| --- | --- |
| `basic_instr.append("WFI")`, `"EBREAK"`, `"DRET"` | `KeyError: 'WFI'` for `+no_wfi=0`, `+no_ebreak=0`, `+no_dret=0` |
| `basic_instr.append(instr_category["SYNCH"])` appends the list as one element | same shape, for `+no_fence=0` |
| `exclude_instr.append(C_ADDI16SP.name)` | the exclusion never matches |
| `get_rand_instr` builds `disallowed_instr`, then picks from the unfiltered pool | the exclusion is never applied at all |
| `randomize_avail_regs` is `pass  # TODO` | `avail_regs` can be all reserved registers, and `randomize_gpr` is unsatisfiable |
| `gen_load_store_instr` extends one `allowed_instr` list across every address | `Error: illegal operands 'c.lwsp a5,25(sp)'` |
| `get_load_store_instr` shallow-copies a pyvsc `randobj` template | every instance of one instruction name shares its `rs1` |

The last one is the reason four of the eight entries came back with a cosim
mismatch before it was found. The instructions are pyvsc objects, so a shallow
copy shares the field objects with the template, and the base register of a
load/store stream changes part-way through the stream:

```
la    s5, region_1+3568   #start riscv_load_store_rand_instr_stream_0
...
lhu   s3, -132 (a1)       #end riscv_load_store_rand_instr_stream_0
```

`a1` still holds the constant the init section put in it, so the program reads
a wild address, the memory agent answers, Spike takes a load access fault, and
the scoreboard reports a trap the DUT did not report. `get_rand_instr` two
functions up already uses `deepcopy`, with a comment explaining why.

The fourth and fifth are why generation used to fail about half the time above
a couple of thousand instructions. pyvsc's own diagnostics named it in the end:

```
Problem Set: 2 constraints
  if ((instr_name == 242)) { (rd == 2); }
  (rd != reserved_rd.reserved_rd[0]);
```

242 is `C_ADDI16SP`, whose `rd` is architecturally SP, and `reserved_rd[0]` is
SP because the load/store stream around it took SP as its base register.
`randomize_instr` has the guard for exactly this case and excludes the four
SP-forcing compressed instructions -- and the exclusion reaches a function that
does not use it.

### Python's `and` and `or` are not pyvsc operators

This is one bug with five sites in one file, and it is the reason
`+illegal_instr_ratio` and `+hint_instr_ratio` were recorded above as
unsupported. The recorded reason -- "randomization fails on current pyvsc after
a handful of instructions" -- was wrong in an interesting way: it never
succeeded at all, in any configuration, from the first call.

pyvsc builds a constraint by side effect. Every expression created inside a
`@vsc.constraint` body is appended to the enclosing scope, and an operator such
as `&` or `|` consumes its operands and pushes the combination. Python's `and`
and `or` are not operators: they test the left operand for truthiness, which an
expression object always has, and return one of the two. So `(a == 1) and
(b == 2)` leaves **both** comparisons behind as separate conjuncts, an `or`
chain of eight alternatives becomes a conjunction of eight, and `not (a == 1)`
evaluates to a Python `False` that goes nowhere while leaving `a == 1` asserted
with the wrong polarity.

`riscv_illegal_instr.py` writes all three. `legal_rv32_c_slli` is the clearest:

```python
with vsc.if_then((self.c_msb == 0) and (self.c_op == 2) and (self.xlen == 32)):
```

means `if (c_msb == 0 && c_op == 2 && XLEN == 32)` and gives the solver
`c_msb == 0; c_op == 2; if (XLEN == 32)`, which pins every compressed encoding
to C.SLLI on its own. `has_func7_c` means `(opcode == 19 && func3 inside {1,5})
|| opcode inside {51,59}` and gives it `opcode == 19 && func3 == 1 &&
func3 == 5 && opcode == 51 && opcode == 59`.

A minimal unsatisfiable core over the sixteen constraint blocks is ten of them,
found by delta-debugging with `set_constraint_enabled`, which is what a
conjunction of mutually exclusive alternatives looks like from the solver's
side. It is also why no single-constraint reduction found anything: each block
is satisfiable alone.

With the five sites fixed, all seven `illegal_instr_type_e` values appear in a
6,000-instruction program and 60 of 60 randomizations succeed:

```
Counter({'kIllegalOpcode': 26, 'kIllegalSystemInstr': 20, 'kHintInstr': 14,
         'kIllegalFunc7': 9, 'kReservedCompressedInstr': 6,
         'kIllegalCompressedOpcode': 6, 'kIllegalFunc3': 5})
```

An AST scan of `pygen_src` finds Python boolean operators inside
`@vsc.constraint` bodies in that file and no other, so this is contained.

One more was needed before the result assembled. `get_bin_str` returns
`hex(instr_bin)` for the 32-bit case, which works by accident, and the bare
integer for the 16-bit case, so every HINT and every compressed illegal
instruction came out as `.2byte 24705` where `.2byte 0x6081` was meant. The
SystemVerilog formats `%8h` and `%4h` and the caller writes `.4byte 0x%s`.

### And `+no_fence=0`

`create_instr_list` drops FENCE, FENCE_I and SFENCE_VMA from the pool
unconditionally. The SystemVerilog it reimplements guards the same line with the
option that exists to control it:

```systemverilog
if (cfg.no_fence && (instr_name inside {FENCE, FENCE_I, SFENCE_VMA})) continue;
```

so `instr_category["SYNCH"]` is empty and `+no_fence=0` has nothing to add. The
line above it is the same mistake with the polarity reversed -- the
SystemVerilog reads `!cfg.enable_sfence && instr_name == SFENCE_VMA`, pyflow
reads `cfg.enable_sfence and ...` -- so restoring the fence guard on its own
starts emitting `sfence.vma` into an rv32imc program. pyflow also has none of
the SystemVerilog's `sfence_c`, which forces `enable_sfence == 0` when the
target does not set `support_sfence`; that condition is folded into the same
patch. A 2,000-instruction program then contains 25 `fence` and 14 `fence.i`
and no `sfence.vma`.

### `riscv_rand_instr_test` generates 200 instructions, not 10,000

`riscv_rand_instr_test.randomize_cfg` hardcodes `cfg.instr_cnt = 10000` over the
top of the command line, and this port used to patch that to
`cfg.argv.instr_cnt` so that each entry's `+instr_cnt` was honoured. That patch
was wrong: the SystemVerilog `riscv_rand_instr_test` does exactly the same
assignment, after `riscv_instr_gen_config::new` has read `+instr_cnt=`, so
**upstream generates 10,000 instructions for all forty of those entries whatever
their `gen_opts` say**. `riscv_ebreak_test`'s `+instr_cnt=6000` is ignored
upstream too.

Removing the patch turned up a worse problem underneath it. In pyvsc the
assignment does not work at all. `cfg.instr_cnt` is a plain Python int and the
constraint that reads it --

```python
self.main_program_instr_cnt in vsc.rangelist(vsc.rng(10, self.instr_cnt))
```

-- is elaborated into the model when the config object is built, which happens
at import, before any test's `randomize_cfg` runs. So the range the solver draws
from is still `[10, argv.instr_cnt]`, and argparse's default for that is 200. In
SystemVerilog the constraint is evaluated at `randomize()` and the same two
lines mean what they say.

Measured on `riscv_ebreak_test`, same seed, everything else equal: with
`--instr_cnt 400` on the command line and 10000 assigned in `randomize_cfg`,
`test_done:` lands at line 354. With `--instr_cnt 10000` it lands at 7,791. The
config dump reports 10000 either way, which is why it is easy to miss. Run as
shipped, pyflow's `riscv_rand_instr_test` generates a main program of between 10
and 200 instructions.

`build_programs.py` puts the 10,000 on the command line, where pyvsc can see it,
and records the entry's own `+instr_cnt` as overridden.

### The debug ROM stub is a `dret`

Thirteen entries send the core into the debug ROM, and only one of them says so
in its testlist entry -- the other twelve start a debug sequence from their own
UVM class, which `build_programs.py` reads out of `core_ibex_test_lib.sv` rather
than keeping a list. pyflow's `gen_debug_rom` is a stub, and what this port used
to put at `debug_rom:` was a self-loop, so every one of those thirteen would
have been a cycle timeout caused by the port rather than by anything upstream.

`riscv_debug_rom_gen` says what belongs there:

```systemverilog
if (!cfg.gen_debug_section) begin
  // If the debug section should not be generated, we just populate it
  // with a dret instruction.
  debug_main = {dret};
```

and `gen_debug_exception_handler` is `str = {"dret"}` in every case, with its
own TODO saying so. So `debug_exception` is now exactly upstream's, and
`debug_rom` is exactly upstream's for an entry that does not ask for a debug
section. `riscv_debug_stress_test` is one -- its description is "debug_rom is
empty, with only a dret instruction". The twelve that do ask get debug entry and
an immediate return where upstream runs a generated ROM, which is the floor
rather than the article, and is recorded as such.

### Where that leaves it: all 57 built and run

**53 of the 57 entries have a program, and all 53 run.** No plusarg workaround:
`+disable_fetch_enable_seq=1` is not passed and the fetch-enable stimulus runs.

| | `small` | `opentitan` |
| --- | ---: | ---: |
| entries | 57 | 57 |
| no program | 4 | 4 |
| inapplicable | 15 | 0 |
| ran | 38 | 53 |
| **passed** | **19** | **30** |

Split by how much of the entry the program actually carries, which is the number
that matters:

| verdict | `small` | `opentitan` | what a pass means |
| --- | ---: | ---: | --- |
| faithful | 5 of 11 | 6 of 13 | every `gen_opt` honoured; the outcome is about the entry |
| partial | 6 of 9 | 7 of 12 | the program differs in ways that leave the entry's stimulus intact |
| hollow | 8 of 18 | 17 of 28 | an option that defines the entry was dropped; the outcome says nothing about the entry's name |

**Seventeen of the thirty passes on `opentitan` are hollow.** Ten of those
seventeen are the PMP and ePMP entries, which run a plain random program with no
PMP configuration in it. A pass there means Ibex executed a riscv-dv program
correctly against Spike with `PMPEnable=1` and nothing configured. That is not
nothing, and it is not what `riscv_epmp_mml_read_only_test` is for.

The two configurations agree on every entry they both run, so nothing here is
about `PMPEnable`, `SecureIbex`, `ICache` or the writeback stage. The 15
inapplicable on `small` are five wanting `SecureIbex` and ten wanting
`PMPEnable`; the three bitmanip entries would make 18, and they have no program
to be inapplicable with.

### What the failures are

Grouped by what the run said, on `--config opentitan`:

| what | entries | why |
| --- | ---: | --- |
| `Did not receive core_status IN_DEBUG_MODE within 10000 cycles` | 3 | the debug ROM signals `IN_DEBUG_MODE` and pyflow's is a bare `dret` |
| `Core did not jump to vectored interrupt handler` | 5 | the interrupt handler's `HANDLING_IRQ` handshake, same shape as the above |
| `Did not receive write to csr 0x342` | 1 | `riscv_dret_test`, waiting on an MCAUSE handshake from the illegal-instruction handler |
| `Did not receive write to csr 0x300` | 1 | `riscv_invalid_csr_test`; it boots U-mode, which pyflow does not do, so the whole privileged block is missing |
| cosim mismatch | 6 | five are "synchronous trap expected at ISS PC, DUT reported none" |
| `Randomization failed` | 3 | Verilator, not the program: an `interval dist` in `core_ibex_seq_lib.sv` |
| killed | 2 | the simulator crashes: `SIGSEGV` with no output, and `*** stack smashing detected ***` |
| cycle timeout | 1 | `riscv_reset_test`, whose class resets the core mid-program |
| `ECC alert did not fire` | 1 | `riscv_rf_addr_intg_test`; see the forced-array-index defect above |

The first four rows are all the same thing: a signature handshake the program
does not emit, and the class waits for. `wait_for_core_setup` was the biggest of
those and is fixed above; what is left needs the debug ROM and the interrupt
vector table, which is `riscv_debug_rom_gen.sv` and the `HANDLING_IRQ` half of
`gen_interrupt_vector_table`.

The three `Randomization failed` are worth separating from the rest: they are
the same Verilator `dist` defect the fetch-enable sequence hit, one file over.
`core_ibex_seq_lib.sv:26` has `interval dist {[0 : max_interval/10] :/ 1, ...}`
and Verilator reports it unsatisfiable partway through a run that has already
solved it several times. That is a simulator problem, not an Ibex one.

Two entries are not stable across runs: `riscv_mem_intg_error_test` passed on
one sweep and aborted on the next, and `riscv_mem_error_test` moved between a
randomize failure and a cosim mismatch. The solver is a subprocess and its
answers are not identical run to run.

Generation is not fast: `--all-tests` is about an hour at four entries at a
time, and longer on a loaded machine. Each entry is a separate interpreter with
the solver loaded, so `--jobs` is capped at half the cores. One seed per entry,
where upstream runs each entry 10 to 15 times.

### Still open: throughput

A clean build reaches cycle 46,808 in 240 seconds -- about **195 cycles per
second**. Instrumented builds are far slower still; either way a full riscv-dv
program is out of reach. The likely cause is
that Verilator shells out to `z3` as a subprocess for every `randomize()`, and
the memory response sequence randomizes once per bus access. That is a cost per
transaction, not per cycle, so it dominates.

Measured properly, by timing fixed work rather than counting cycles in fixed
time. 20,000 cycles, three runs each:

| build | runs (s) | mean |
| --- | --- | ---: |
| normal | 102.52, 101.70, 102.23 | 102.1 |
| `--pin-delays`, no constrained solve at all | 102.15, 98.03, 104.39 | 101.5 |

**The solver is not the bottleneck.** Removing every constrained solve from the
response path -- not reducing it, removing it -- buys about half a percent,
inside the noise.

That withdraws two earlier claims from this file. The "+30% from `rand_mode`"
was a measurement artefact: it came from counting cycles reached in a fixed
wall clock, with solver logging enabled, across different phases of the
program. Timed as fixed work the difference disappears. And the 34-SMT-queries
-per-randomize figure, while real, does not matter: `z3 --in` is one persistent
process and the queries are cheap next to everything else. The `rand_mode`
change is kept because it is equivalent and removes work that was pointless,
not because it made the run faster.

Measured properly at last: instrumentation committed before building, the flag
verified in `--help`, the overlay verified in `build/overlay`, the solver log
counted, and the box idle.

| memory-response path | `check-sat` / 3k cycles | wall, 20k cycles | CPU |
| --- | ---: | ---: | ---: |
| as upstream writes it | 20,513 | ~102 s | -- |
| grant delay drawn directly | 14,340 | 61.6 / 58.9 / 62.4 | ~10 s |
| both delays drawn directly | **35** | **2.61 / 2.57 / 2.58** | ~2.5 s |

**About 39x faster, and wall now equals CPU**: the simulator is finally
compute-bound rather than blocked on a pipe. 195 cycles/s became roughly 7,700.
Zero cosim mismatches throughout, and the delay distributions are unchanged --
same buckets, same weights, drawn with `$urandom_range` instead of the
constraint solver.

The lesson generalises beyond this testbench. Verilator solves constraints by
shelling out to `z3`, and every `randomize()` is a round trip over a pipe.
That is fine for setup-time randomisation and ruinous per transaction. A UVM
agent that randomizes a delay on every bus access pays it thousands of times a
run. Nothing here needed the solver: five fields were pinned by equality and
the delays are three- and four-bucket weighted picks.

`--pin-delays` stays as a measurement build, but is now redundant for timing:
the shipped path makes 35 solver calls a run, which is already the floor.

What is left is the simulation itself: `--timing` coroutine scheduling across a
UVM environment on a 270 MB model, at about 195 cycles per second. The next
measurement is `--prof-exec` to attribute model time, and the first cheap thing
to try is the optimisation level -- `--binary` compiles the generated C++ at
`-Os`.

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

## The directed tests

`directed_tests/directed_testlist.yaml` is the other testlist, and the larger
one: **944 entries** against the riscv-dv list's 57. Nothing generates them.
Each entry names a hand-written C or assembly source and upstream's
`scripts/compile_test.py` turns it into a binary with two commands, which is
what `run_directed.py` runs:

```
$RISCV_GCC <gcc_opts> -I<includes> -T<ld_script> -o test.o <test_srcs>
$RISCV_OBJCOPY -O binary test.o test.bin
```

The working directory matters and is not stated anywhere: the `-I` paths inside
`gcc_opts` are relative and are written against `dv/uvm/core_ibex`, while
`ld_script`, `includes` and `test_srcs` are resolved by the schema relative to
the testlist file. Both are honoured here.

```sh
python3 run_directed.py --config opentitan --list
python3 run_directed.py --config opentitan --group riscv-tests
python3 run_directed.py --config opentitan --pattern 'pmp_*' --jobs 4
python3 run_directed.py --config opentitan          # all 944
```

They are not evenly spread. 744 of the 944 are ePMP entries, and those 744 are
generated from three `.cc_skel` templates -- `test_pmp_csr_1` (528),
`test_pmp_ok_1` (192) and `test_pmp_ok_share_1` (24) -- as a combinatorial
sweep over PMP configuration bits. The other 200 are 107 riscv-arch-tests and
93 riscv-tests.

### Five things had to be fixed before any of the numbers meant anything

Two are this toolchain, and would not arise on lowRISC's.

- The riscv-tests and riscv-arch-tests configs name no `-march` and take the
  toolchain default, which for lowRISC's `riscv32-unknown-elf` is `rv32imc`
  and for `riscv-none-elf` 15.2.0 here is `rv32imac_zmmul_zaamo_zalrsc_zca` --
  the A extension Ibex has not got. One derived from the built Ibex
  configuration is supplied instead.
- gcc 15 assembles no CSR or `fence.i` instruction unless Zicsr and Zifencei
  are named, so the epmp config's `-march=rv32imc` becomes
  `-march=rv32imc_zicsr_zifencei`. Same instruction set, newer assembler.
- `syscalls.c` declares three `__thread` buffers; this toolchain emulates TLS,
  libgcc's emulation calls `malloc`, and the config passes `-nostdlib`, so the
  744 ePMP entries did not link. `shims/emutls_malloc.c` is a bump allocator
  for exactly that.

Three are upstream, and each one silently changes what a test means.

- **The ePMP linker script is stale by a megabyte.** Every generated ePMP
  source says `#define TEST_MEM_START 0x80200000` and programs its PMP entries
  from it; `mseccfg_test.ld` places `TEST_MEM` at `0x0020_0000` and `M_MEM` at
  `0x0010_0000`. The flat binary starts at its lowest section, so loading at
  `BOOT_ADDR` puts `_start` at `0x8000_0080` -- right -- and `TEST_MEM` at
  `0x8010_0000`, a megabyte below where the program's own PMP entries say it
  is. lowRISC regenerated the C for Ibex's memory map and left the script on
  Spike's. In a 13-entry sample the four failures were the four entries that
  execute from or load out of `TEST_MEM`, and all four pass with the script
  rebased. `--stock-ld` runs it as vendored.
- **The arch tests compile their bodies away.** Every riscv-arch-test wraps its
  body in `#ifdef TEST_CASE_1`, and the framework's own runner defines that
  after matching the ISA string in `RVTEST_CASE`. The testlist's `gcc_opts`
  does not. `add-01` built as the testlist asks has a 292-byte `.text.init`,
  retires **73 instructions, none of them an add**, and passes. With
  `-DTEST_CASE_1=True` it is 12,868 bytes and retires 3,239. All 107 entries
  were affected. `--stock-defines` leaves them as upstream has them.
- **No ePMP test can fail upstream.** `syscalls.c`'s `tohost_exit(code)` writes
  `(code << 8) | CORE_STATUS` and then a constant `(TEST_PASS << 8) |
  TEST_RESULT`. `checkTestResult` computes a verdict, passes it to `exit()`,
  and the harness is told TEST_PASS regardless -- as is `handle_trap`, which
  exits 1337. The code does reach memory and `TRACE_EXECUTION` records the
  store, so `run_directed.py` reads it back out of the tracer log and reports a
  non-zero one as a failure. Upstream has no way to fail one of these 744.

### What each group's pass actually means

| group | entries | what a pass is |
| --- | ---: | --- |
| riscv-tests | 93 | the program's own verdict: `RVTEST_PASS` or `RVTEST_FAIL` |
| epmp-tests | 744 | the exit code recovered from the trace, plus the cosim |
| riscv-arch-tests | 107 | the RVFI cosim against Spike, and nothing else |

The arch tests signal TEST_PASS unconditionally from `RVMODEL_HALT` and leave
the checking to a signature comparison against a reference that neither this
flow nor upstream's performs. What does check them here is the cosim
scoreboard, which compares every retired instruction against Spike -- a real
check, and for `add-01` it is 3,239 instructions of one.

One more thing worth knowing about the riscv-tests group: `riscv_test.h`'s
`trap_vector` sends an ecall, and any exception it cannot handle, to
`write_tohost`, which stores to the `tohost` symbol and spins. The testbench
watches the signature address, not `tohost`, so a riscv-test that takes an
unexpected trap does not fail -- it hangs until the budget runs out. `scall`,
which ends on a deliberate ecall, is a wall-clock timeout for that reason and
cannot be anything else in this environment.

### All 944 run

`--config opentitan`, four at a time, everything above applied:

| group | entries | passed | other |
| --- | ---: | ---: | --- |
| riscv-tests | 93 | 89 | 3 build failed, 1 wall-clock timeout |
| riscv-arch-tests | 107 | 105 | 1 build failed, 1 double faults |
| epmp-tests | 744 | 718 | 26 self-check failed |
| **total** | **944** | **912** | **32** |

The riscv-tests and epmp rows are one run of all 944; the arch row is a second
run of that group alone, after `-DTEST_CASE_1=True` was added. Nothing in the
other two groups changed between them. The 944-entry run took about two hours
on a box that was busy with something else; the arch group on its own is five
and a half minutes.

The four that do not build are the vendored sources against a newer binutils,
not anything about Ibex: `illegal`, `ma_addr` and `ma_fetch` use `mbadaddr` and
`sptbr`, the pre-1.10 names for `mtval` and `satp`, and `jalr-01` writes
`la x0, 5b`, which the assembler now rejects.

`scall` is the wall-clock timeout, and cannot be anything else: it ends on a
deliberate ecall, which `trap_vector` sends to `write_tohost`.

`cebreak-01` reaches the double-fault threshold. `RVMODEL_BOOT` is empty, so
`mtvec` is still the reset value -- `BOOT_ADDR | 0x1`, vectored at
`0x8000_0000` -- and the arch tests place their first code at `0x8000_0080`.
The breakpoint exception vectors into the 128 bytes of zeros ahead of the
entry point and executes `c.unimp` until the detector fires. That is the
environment, not the core.

### The 26 that fail their own check: both are test bugs

All 26 are ePMP and none is a cosim mismatch. They are two defects, both
introduced by the same lowRISC commit, and neither is a defect in Ibex.

**Not the linker script.** Re-running all 26 with `--stock-ld` gives the same
26 failures with the same exit codes, byte for byte in the results file. The
rebase is not what makes them fail; it is also not what hides anything, because
each family fails for a reason that does not depend on where TEST_MEM lands.

The vendored tests came from
[lowrisc/riscv-isa-sim](https://github.com/lowRISC/riscv-isa-sim) branch
`mseccfg_tests`, and commit `a7c5d5d` ("Copy over changes made by Saad525")
moved them from Spike's memory map to Ibex's and gave them a way to signal a
result. Both changes are in the diff of the two skeletons, and each one broke
an assumption the generator still makes.

**Twenty-one `test_pmp_ok_1_*`: entry 0 swallows the region under test.**
`set_cfg()` has two arms. The `M_MODE_RWX` arm programs one NAPOT entry for all
of M memory; the other arm programs three TOR entries for code, data and
TEST_MEM separately. The NAPOT entry used to be

```c
asm volatile ("csrw pmpaddr0, %0 \n" :: "r"((TEST_MEM_START >> 3) - 1) : "memory");
```

which is the NAPOT encoding of `[0, TEST_MEM_START)`: it stops exactly where
the region under test begins, and by construction cannot shadow it. `a7c5d5d`
rewrote it as

```c
asm volatile ("csrw pmpaddr0, %0 \n" :: "r"((0x80000000 >> 2) | 0xfffff) : "memory");
```

`0xfffff` is a size as well as a base: against the old base it gave 512 KiB,
against `0x8000_0000 >> 2` it gives twenty trailing ones, so 8 MiB,
`[0x8000_0000, 0x8080_0000)`. TEST_MEM at `0x8020_0000` and U_MEM at
`0x8024_0000` are both inside it. PMP matches lowest index first, so entry 0
answers for every access the test makes, entry 2 is never consulted, and
because entry 0 is unlocked and grants RWX, no access ever faults, in M mode or
in U mode. The trace shows it plainly: `PA:0x80200010 store:0x0909090a` and a
fetch at `0x80200000`, no trap either side.

The generator picks this arm by `set_m_mode_rwx(cur_files_count % 3 == 0)`,
which is why it affects a scattered third of the `mml0` entries. 32 of the 192
have `M_MODE_RWX 1`; **the rule "both actual counters stay 0" predicts the exit
code of all 32**, the 11 that pass along with the 21 that fail. The 160 with
`M_MODE_RWX 0` all pass. The other arm of the same function bounds M memory at
`0x8020_0000`, which is what the NAPOT entry was meant to say.

**Five `test_pmp_csr_1_*_mml1_*`: the signature entry is a locked entry.**
`a7c5d5d` added

```c
asm volatile ("csrw pmpaddr7, %0 \n" :: "r"(0x8ffffff8 >> 2) : "memory");  // for ibex signature addr
```

so the test can reach the testbench's signature address, and, since M mode
needs `L` set to reach anything once MML is on, added `PMP_L` for entry 7 to
both arms. The generator was not told. Its `pmpaddr` test picks
`addr_idx = 7 + count % 9` under the comment "for invalid cfgs, start from 7",
on the assumption that entries 7 and above are untouched, and its model sets
`pmpaddr_fail` only in the `addr_idx` 2/3 branch. So for the entries where
`addr_idx` comes out 7, the test writes a locked `pmpaddr7` and expects the
write to land.

It does not, and it should not. `pmp7cfg.L` is set and `mseccfg.RLB` is clear,
so `ibex_cs_registers.sv` gates the write off:

```systemverilog
assign pmp_cfg_locked[i] = pmp_cfg[i].lock & ~pmp_mseccfg_q.rlb;
assign pmp_addr_we[i]    = csr_we_int & ~pmp_cfg_locked[i] & ...
```

That is Smepmp's rule, and it is plain PMP locking rather than anything
MML-specific: MML only enters into it by being the reason the test locks entry
7 in the first place. Spike agrees exactly. From `+cosim_log_file=`:

```
core   0: 0x80001370 (0x3b779073) csrw    pmpaddr7, a5
core   0: 3 0x80001370 (0x3b779073)
core   0: 0x80001374 (0x3b702673) csrr    a2, pmpaddr7
core   0: 3 0x80001374 (0x3b702673) x12 0x23fffffe
```

The write logs no state change and the read-back is the old value. 18 entries
reach `addr_idx` 7 with `mml1`; the 5 with `rlb0` fail and the 13 with `rlb1`
pass, which is the same rule seen from the other side.

Note that this is not
[lowRISC/ibex#2242](https://github.com/lowRISC/ibex/issues/2242), which is MML
suppression of `pmpcfg` writes missing from the DV CSR model. Different
component, different mechanism; the RTL has both.

| bucket | count |
| --- | ---: |
| harness or address artefact | 0 |
| test bug | 26 |
| Ibex bug | 0 |
| Spike bug | 0 |
| specification ambiguity | 0 |

### Where the cycle budget had to go

Upstream never overrides `timeout_in_cycles`, so its budget is
`core_ibex_base_test`'s default of 100,000,000 cycles, which is hours here. At
200,000 ten of the `pmp_mseccfg` entries reported a timeout; all ten pass at
5,000,000, the first of them finishing at cycle 236,835. The default is now
5,000,000 with the entry's own `timeout_s` -- 300 seconds for all three configs
-- as the real bound.

## What is not done

- **Functional coverage.** `--fcov` exists but Verilator 5.050 dies with an
  internal error on the FSM transition bins. The three fcov sources leave the
  compile by default and `DV_FCOV_DISABLE` is defined.
- **Assertions.** `prim_assert.sv` gives Verilator the dummy macros, so the
  design's SVA is compiled out. Upstream's flows run with assertions on, and
  they are part of what those flows check. Turning them on means finding out
  how much of lowRISC's SVA Verilator 5 accepts.
- **The 28 hollow entries.** They are generated and run, and marked, and what
  they run is a plain random program under a name that promises something else.
  Making them mean anything is five separate pieces of work in pyflow, none of
  them small: `riscv_pmp_cfg.sv` (1,072 lines) for the ten PMP entries,
  `riscv_debug_rom_gen.sv` (252) for the twelve debug ones, `riscv_csr_instr.sv`
  plus a `csr_c` for the CSR ones, `USER_MODE` in a target's
  `supported_privileged_mode` for the three privilege-mode ones, and a ratified
  Zba/Zbb/Zbc/Zbs instruction library for the three bitmanip ones.
  `riscv_csr_test` is a sixth: it needs riscv-dv's `gen_csr_test.py`, a separate
  generator not wired up here.
- **The interrupt handler's `HANDLING_IRQ` handshake.** Five entries fail on
  "Core did not jump to vectored interrupt handler", which is the same shape of
  gap as the MSTATUS one that was fixed: `gen_interrupt_vector_table` in pyflow
  does emit it, so this one needs finding rather than writing.
- **`num_of_sub_program`.** Nine entries ask for sub-programs and get none.
  `riscv_jump_instr` is not implemented in pyflow at all -- `insert_jump_instr`
  is `pass` -- so this is a class to write, not a line to fix.
- **The 26 ePMP self-check failures.** Diagnosed above: two test defects, both
  from the same lowRISC commit, neither of them a defect in the core. Nothing
  is fixed here, because the point of the group is to run what upstream ships.
- **A runner.** `run_tests.py` reports outcomes; it does not yet do the
  both-harness reporting the other ports here have.
- **A configuration between the two.** Everything the directed testlist needs
  is `PMPEnable`, and `opentitan` brings `SecureIbex`, `ICache`,
  `ICacheScramble`, a writeback stage and bitmanip with it. `maxperf-pmp` in
  `ibex_configs.yaml` is PMP with none of that and would separate what the PMP
  tests find from what the rest of the configuration does. It has not been
  built here.

## Bugs worth reporting

In riscv-dv's pyflow, eighteen bugs: three on the signature-handshake path,
seven found by generating at the sizes the testlist asks for, the five-site
`and`/`or`/`not` bug in `riscv_illegal_instr.py` together with the decimal
`get_bin_str` beside it, the missing `cfg.no_fence` guard in
`create_instr_list` together with the inverted `enable_sfence` polarity above
it, `riscv_rand_instr_test.randomize_cfg` setting an instruction count that the
constraint reading it was elaborated before, the missing MSTATUS and MIE
signature handshake in `gen_privileged_mode_switch_routine`, and
`riscv_instr_base_test.run_phase` catching `Exception` where pyflow's own error
path raises `SystemExit`, which turns any generation error into a permanent
hang. Most are small, several are one word. Between them they are the
difference
between pyflow generating a few hundred instructions and generating what the
testlist asks for, and they take `+illegal_instr_ratio`, `+hint_instr_ratio`
and `+no_fence=0` from "cannot" to "does". Each has its evidence in
`PYGEN_PATCHES`.

The `and`/`or` one is worth raising on its own, because it is a hazard of
pyvsc's API rather than a typo: a constraint written with Python's boolean
operators compiles, runs, and asserts something other than what it reads as. An
AST scan for `ast.BoolOp` and `ast.Not` inside `@vsc.constraint` bodies finds
every instance in a few lines and would make a reasonable lint.

The `SystemExit` one is the most expensive to hit and the cheapest to fix. It
cost 37 minutes of a blocked build before anything said what was wrong, and the
fix is one word: `except Exception` becomes `except BaseException`. Any pyflow
user who trips one of the two dozen `logging.critical(...); sys.exit(1)` paths
sees a process at zero CPU and no message at all.

The unimplemented parts of pyflow are worth reporting as a list, because none of
them announces itself at run time: `riscv_pmp_cfg`, `riscv_debug_rom_gen`,
`riscv_csr_instr`, `riscv_jump_instr`, `riscv_loop_instr` (in the factory, three
quarters commented out) and `randomize_avail_regs` are absent or stubs, no
target lists `USER_MODE` in `supported_privileged_mode`, and the options that
drive all of them are accepted by the argparse and silently do nothing.

In Verilator, three internal errors, each with a small reproducer available from
the overlays: the covergroup transition bins over enum items, `int'()` on a wide
value used as a queue index, and `event e = null`. The `VlForceVec` virtual
interface restriction and the `ref`-argument reading of 13.2.2 are limitations
rather than crashes, but both are worth raising. So is the fourth: a forced
signal used as an unpacked-array index is read unforced, which silently gives
the wrong answer rather than failing, and has a two-module reproducer.

On the Ibex side, `core_ibex_tb_top.sv` drives `unused_assert_connected`
without the `` `ifdef INC_ASSERT `` that declares it, which breaks any tool
whose assertion macros are the dummy ones.

And five about the directed tests, none of which any simulator would report.
Three make a test pass while testing nothing:

- `directed_testlist.yaml`'s riscv-arch-tests config never defines
  `TEST_CASE_1`, so all 107 of those entries compile to a register-init
  prologue and an unconditional `RVMODEL_HALT`. `add-01` retires 73
  instructions, none of them an add.
- `vendor/riscv-isa-sim/tests/mseccfg/mseccfg_test.ld` places `TEST_MEM` at
  `0x0020_0000` where the generated sources beside it program their PMP
  entries at `0x8020_0000`. Every one of the 744 ePMP entries runs against
  the wrong megabyte.
- `syscalls.c`'s `tohost_exit(code)` always signals `TEST_PASS`, so no ePMP
  test can fail. 26 of the 744 do fail once the code is read back out of the
  trace.

Two are why those 26 fail, both from `a7c5d5d` on `lowrisc/riscv-isa-sim`'s
`mseccfg_tests` branch, both diagnosed in full above:

- the `M_MODE_RWX` arm of `set_cfg()` writes `(0x80000000 >> 2) | 0xfffff` to
  `pmpaddr0`, an 8 MiB NAPOT region that covers TEST_MEM and U_MEM and, being
  entry 0 and unlocked, grants every access the test expects to fault. The
  expression it replaced, `(TEST_MEM_START >> 3) - 1`, stopped where TEST_MEM
  began. 32 of the 192 `test_pmp_ok_1_*` take that arm and 21 of them fail;
  the other 11 expected no fault anyway. `((TEST_MEM_START >> 3) - 1) |
  (0x80000000 >> 2)` would restore the intent.
- the same commit gave `pmpaddr7` to the testbench signature address and set
  `pmp7cfg.L` whenever MML is set, but `gen_pmp_test.cc` still treats entries 7
  and up as free and never predicts a `pmpaddr` failure for them. Five
  `test_pmp_csr_1_*` land on `addr_idx` 7 with `mml1` and `rlb0`, where the
  write is correctly ignored.
