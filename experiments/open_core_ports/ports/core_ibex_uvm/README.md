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

**Tests pass.** Each testlist entry now runs the program its own `gen_opts` ask
for, and a program that reaches its end tells the testbench so. Of the first
eight entries tried, six end on the riscv-dv signature handshake with no cosim
mismatch; none times out. See "Per-test programs".

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
cd build && ./obj/core_ibex_tb \
  +UVM_TESTNAME=core_ibex_base_test \
  +bin=elf/gen_0.bin \
  +signature_addr=8ffffffc \
  +timeout_in_cycles=4000000 \
  +disable_fetch_enable_seq=1
```

or, for the testlist rather than one program:

```sh
python3 build_programs.py --all-tests
python3 run_tests.py +disable_fetch_enable_seq=1
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
| `pmp_*`, `mseccfg`, `enable_write_pmp_csr`, `suppress_pmp_setup` | no pyflow target sets `support_pmp` |
| `gen_debug_section`, `num_debug_sub_program`, `set_dcsr_ebreak`, `enable_debug_single_step` | `gen_debug_rom` is `# TODO / pass`, and no pyflow target sets `support_debug_mode` |
| `toggle_dit`, `toggle_dummy_instr`, `gen_all_csrs_by_default`, `add_csr_write` | Ibex's own extension, which is SystemVerilog |
| `uvm_set_type_override=...` | a UVM factory override; there is no factory here |
| `enable_zba_extension` and the other subset flags | pyflow has only `enable_b_extension` and `enable_bitmanip_groups` |
| `directed_instr_N=ibex_*` and three of riscv-dv's own streams | not in `riscv_utils.factory`, which is a literal dict of eleven names |
| `num_of_sub_program=N` | forced to 0; `gen_callstack` fails on current pyvsc |
| `illegal_instr_ratio`, `hint_instr_ratio` | `riscv_illegal_instr` randomization fails on current pyvsc after a handful of instructions |
| `no_csr_instr=0`, `no_fence=0` | pyflow generates neither CSR nor fence instructions in either case |

The last row is worth reading twice, because it is not a missing feature.
`build_basic_instruction_list` guards the CSR instructions with
`cfg.init_privileged_mode == "MACHINE_MODE"`, an enum compared against a
string, which is never true; and `create_instr_list` skips FENCE, FENCE_I and
SFENCE_VMA before the categories are filled, so the SYNCH category is empty.
Correcting the CSR comparison was tried: the CSR instructions then reach the
solver and every program fails to generate, so it is left alone and recorded
instead.

`gen_test` is honoured where pyflow has the test. Forty entries name
`riscv_rand_instr_test`, which pyflow does have -- with four of its seven
directed streams commented out. Passing `--gen_test` matters for a second
reason: `riscv_instr_base_test.py` runs its test at import, guarded by
`cfg.argv.gen_test`, and `riscv_rand_instr_test.py` imports it, so without the
flag both run, nested inside each other's multiprocessing pool, and the second
one hangs.

### Seven more pyflow bugs

Generating at the sizes the testlist asks for -- 6,000 to 20,000 instructions
rather than a few hundred -- turned up seven more, all patched the same way.
Three are the same mistake: a name pushed into an instruction pool as a string
where the pool holds `riscv_instr_name_t` members.

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

### Where that leaves it

Eight entries built and run so far, at the instruction counts their entries ask
for, with `+disable_fetch_enable_seq=1`:

| Entry | Outcome |
| --- | --- |
| `riscv_arithmetic_basic_test` | passed |
| `riscv_ebreak_test` | passed |
| `riscv_illegal_instr_test` | passed |
| `riscv_rv32im_instr_test` | passed |
| `riscv_unaligned_load_store_test` | passed |
| `riscv_user_mode_rand_test` | passed |
| `riscv_debug_wfi_test` | cosim mismatch: trap expected at ISS PC 80000c2a, DUT at 80000c26 reported none |
| `riscv_multiple_interrupt_test` | failed: no write to CSR 0x300 within the test class's 10,000-cycle timeout |

Neither of the two is investigated. Both are entries whose UVM class drives
stimulus of its own -- a debug sequence and an interrupt sequence -- and whose
program is missing the debug ROM pyflow cannot generate, so the interaction
between the class and the program is the place to start rather than the program
alone.

Seven of the eight ran a program that differs from its entry in at least one
recorded way, so a pass here means the class runs a riscv-dv program of roughly
the intended shape to its handshake, not that the entry's intent was covered.

Generation is not fast: an entry of a few hundred instructions takes seconds, a
10,000-instruction one with directed streams several minutes, so `--all-tests`
is an hour or two at four entries at a time. `--jobs` is capped at half the
cores because each entry is a separate interpreter with the solver loaded.

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

## What is not done

- **Functional coverage.** `--fcov` exists but Verilator 5.050 dies with an
  internal error on the FSM transition bins. The three fcov sources leave the
  compile by default and `DV_FCOV_DISABLE` is defined.
- **Assertions.** `prim_assert.sv` gives Verilator the dummy macros, so the
  design's SVA is compiled out. Upstream's flows run with assertions on, and
  they are part of what those flows check. Turning them on means finding out
  how much of lowRISC's SVA Verilator 5 accepts.
- **The whole testlist.** Eight of the 57 entries have been generated and run.
  `python3 build_programs.py --all-tests` builds the rest, which takes about an
  hour; `python3 run_tests.py --list` shows which entries have a program.
- **The entries pyflow cannot serve.** The PMP, debug and bitmanip entries can
  be generated, but with the options that define them dropped, so what they run
  is a plain random program under a differently named test class. They also
  want RTL parameters this build does not set (`PMPEnable`, `SecureIbex`,
  `RV32B`); `rtl_params` is parsed out of the testlist and not yet acted on.
  `riscv_csr_test` needs riscv-dv's `gen_csr_test.py`, which is a separate
  generator not wired up here.
- **A runner.** `run_tests.py` reports outcomes; it does not yet do the
  both-harness reporting the other ports here have.

## Bugs worth reporting

In riscv-dv's pyflow, ten bugs: the three on the signature-handshake path and
the seven in "Seven more pyflow bugs". All are small, several are one word, and
between them they are the difference between pyflow generating a few hundred
instructions and generating what the testlist asks for. Each has its evidence
in `PYGEN_PATCHES`.

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
