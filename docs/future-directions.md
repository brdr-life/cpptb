# Future directions

Candidate work that is **not scheduled**. The [roadmap](roadmap.md) admits a
milestone only once a concrete project supplies the use case, semantics,
runnable example, test coverage, and performance peer. Nothing here has cleared
that bar; this page records the analysis so the reasoning survives, in the same
spirit as
`benchmarks/authoring_core/OPTIMIZATION_NOTES.md`.

Two areas are covered: capabilities the framework lacks, and the verification
corpus needed to judge whether it is actually better than the alternatives.

## Framework capabilities

### Temporal assertions

The largest gap that no milestone currently covers. Every check today is
procedural: `expect_eq` compares a value at one instant. There is no way to say
"every request is followed by an acknowledgement within five cycles" except an
ad-hoc coroutine with hand-written bookkeeping, and nothing distinguishes a
property that passed from one that was never exercised.

This is the reason teams tolerate SystemVerilog. It is also the gap the
coroutine model is best placed to close: implication, bounded windows,
stability, and `throughout` all express naturally as awaitable expressions, and
their pass, fail, and vacuity counts belong in `TestContext` alongside existing
checks, feeding assertion coverage into `cpptb/coverage.hpp`.

It stays clear of the [deliberate non-goals](roadmap.md#deliberate-non-goals):
no factory, no phases, no objections, no sequencer. It is a missing checking
primitive, not imported architecture.

### A second simulator

Already milestone 6, and worth restating as the gate on everything else.
Verilator-only excludes encrypted vendor IP, gate-level netlists, power-aware
runs, and accurate X propagation, which is most of what an industrial user
needs. It is also the claim in the README that remains untested: transport is
described as standards-based DPI, and one backend cannot demonstrate that.

### Waveforms and transaction annotation

Milestone 7 already lists waveforms. Nothing in `include/` or the code
generator emits VCD or FST today. Debugging is where verification time is
spent, and a failure currently yields logs and nothing else.

The valuable half is not the dump itself but the annotation: transaction
recording already exists, so a scoreboard mismatch could place the offending
transaction on the waveform at the right cycle. Combined with deterministic
seeds, the deferred waveform-on-failure rerun becomes cheap and is worth
promoting out of the backlog.

### Coverage closure as a workflow

`cpptb/coverage.hpp` can merge, but merging is a primitive, not a methodology.
Closure needs merging across seeds and runs, a report ranking holes by how
reachable they are, and an answer to "what should I randomize next".

This is deliberately not
[UCIS interchange](roadmap.md#no-priority-backlog), which remains correctly
deferred: the value is the local closure loop, not a file format. Coverage as
structured data that both C++ and Python tooling can consume plays to a
strength SystemVerilog handles poorly.

### Regression orchestration

Seeds already exist in the runner. The public CLI is `build`, `list`, and
`test`. What is missing is the loop that multiplies everything else: many tests
across many seeds, run in parallel, with failure clustering, a one-line
reproduction command, and a trend over time. The backlog defers JUnit
conversion, tag filtering, and reproduction-command presentation; individually
small, together they are the difference between running tests and running a
regression.

### Not recommended

Further scheduler performance work. Measured ratios sit between `0.76x` and
`1.08x` against pure SystemVerilog, the optimization notes put the remaining
architectural cost near `0.7%`, and profiles attribute the rest to Verilator
itself. Speculative protocol components should also stay deferred on the
existing bar, since a component library built without a consumer is a
maintenance liability.

## A real verification corpus

The current benchmarks are framework-authored. Deciding whether cpptb genuinely
improves on the alternatives needs testbenches somebody else wrote, for designs
somebody else maintains, running workloads long enough to be meaningful.

### What UVM on Verilator can and cannot do

Verilator 5.050 supports considerably more of the verification subset than it
once did. Verified directly against the pinned version:

- classes with `rand` members and `constraint` blocks solve and randomize;
- constraint solving is delegated to an external `z3` binary, so
  `make z3-toolchain` incidentally satisfies it;
- covergroups with `coverpoint` and `bins` compile and sample.

Verilator's own contributor documentation nevertheless describes its current
focus as "completing Universal Verification Methodology (UVM, IEEE 1800.2-2017)
support", so UVM is in progress rather than available.

The consequence for comparisons is specific. A UVM environment cannot be used
as a **performance** peer on Verilator today, because it cannot be relied on to
run there at all. It remains a legitimate **ergonomics** peer: port its
structure and compare the code. For performance the runnable peers on Verilator
are plain SystemVerilog and cocotb, which the four-mode harness already
supports.

A useful side effect of Verilator's constraint support: pure-SystemVerilog
twins can now use constrained-random classes rather than procedural stimulus,
making them a more representative baseline than they were.

### Candidates, verified as live and maintained

| Project | Verification surface | Why it fits |
| --- | --- | --- |
| lowRISC/ibex | `examples/simple_system` with `ibex_simple_system.cc`, plus `dv/uvm/core_ibex` and `dv/verilator` | Its Verilator harness is already hand-written C++, so a cpptb port is a like-for-like ergonomic comparison. Runs real RISC-V software. |
| alexforencich/verilog-ethernet | around 230 cocotb testbench directories | cocotb is coroutine-based, the closest methodological peer, and already a mode in the harness. Two of its modules are vendored already. |
| openhwgroup/cva6 | `verif/` including `core-v-verif`, `env`, `regress`, `sim`, `tb`, `tests` | Application-class core with a full verification environment; the UVM half serves as an ergonomics peer. |
| pulp-platform/axi | `test/` | AXI is the protocol the component library lacks; APB is the only bus covered today. |
| lowRISC/opentitan | `hw/dv/verilator` | Chip-level with boot ROM and software. The most credible workload and the heaviest to adopt. |
| chipsalliance/caliptra-rtl | active, security root of trust | Realistic security-focused traffic; verification flow needs assessment before committing. |

### Suggested order

1. **Ibex simple system.** Smallest step with the clearest comparison, and it
   extends the existing PicoRV32 firmware workload to a maintained core whose
   Verilator flow is C++ already.
2. **One verilog-ethernet cocotb testbench.** Ported one-to-one, it measures
   cpptb against the peer methodology that most resembles it, on a design
   partly vendored already.
3. **CVA6 or pulp AXI**, depending on whether the goal is a larger workload or
   a wider protocol surface.
4. **OpenTitan**, once the earlier ports have established the porting pattern.

Vendoring should follow the precedent in
`benchmarks/framework_comparison/open_cores/`: exact pinned files, upstream
licence notices preserved in `THIRD_PARTY_NOTICES.md`, and only the RTL each
workload elaborates.

### What such a comparison must report

Performance alone would waste the exercise. Each port should record the
semantic evidence both sides produce, the wall time and its validity under the
existing environment guard, and the ergonomic measures that motivated the
framework: lines of testbench code, how much is generated rather than written,
what a failure reports before a debugger is opened, and how long a single test
takes from edit to result.
