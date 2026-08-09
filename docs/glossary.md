# Glossary

Short definitions for the terms the rest of this documentation uses without
stopping to explain. Each entry links to the page that treats it fully.

**Access set**
: The set of internal hierarchy paths a testbench actually touches,
  recovered at build time by compiling the testbench translation units with
  the discovery define and scanning the objects — no test code executes.
  The generated wrapper carries transport only for this set. See
  [How a build works](how-it-works.md).

**Backdoor**
: Reading or depositing a value directly into design state (a memory word,
  a register field) without a bus transaction, through generated hierarchy
  probes. The opposite is a **frontdoor** access, which goes through the
  design's protocol interface and takes simulation time. See
  [Memory and register models](memory-register-models.md).

**Drive point / drive anchor**
: The instant within a clock cycle at which a driver writes its pins. In
  the write model, the anchor is the rising edge plus the `ReadWrite`
  settle: a write queued there lands after the edge's own updates and is
  captured by the next edge. Legacy immediate-write benches anchored half a
  cycle earlier, at the falling edge, achieving the same DUT-visible timing
  by geometry. See [Scheduling](scheduling.md#the-write-model).

**Full advance vs. settle**
: The two moves a driver makes between anchors. A *full advance* consumes a
  clock edge and lands at the next cycle's anchor. A *settle* descends to
  the anchor of the cycle it is already in — after a bare `RisingEdge`
  wait, for instance — without consuming another edge. Classifying a drive
  site as one or the other is done by its predecessor *await* in control
  flow, never by the preceding line of text. See
  [Coming from cocotb](coming-from-cocotb.md#the-three-traps).

**Phase contract**
: The guarantee attached to each simulator phase wait: what has already
  happened when it resumes. `ReadWrite` resumes after the timestep's
  evaluation has settled and queued writes flush there; `ReadOnly` resumes
  at the end of the timestep, when writing is illegal; `NextTimeStep`
  resumes at the start of the next one. The scheduler conformance suite
  pins this contract on every supported backend. See
  [Scheduling](scheduling.md).

**Settle point**
: The `ReadWrite` instant of a timestep — where evaluation has settled and
  the deferred write queue flushes. Writing from inside it re-arms it, so
  the queue drains within the timestep (cocotb's writes-until-stable loop).

**Timing backend**
: The mechanism that gives the C++ scheduler its simulator-phase waits.
  `"verilator-direct"` drives Verilator's host loop directly;
  `"vpi"` rides standard VPI callbacks and is the portable path. Every
  project selects one; there is no supported build without a backend. See
  [Scheduling](scheduling.md).

**Transport**
: The generated DPI path that moves signal values between the simulator and
  the C++ side — packed word arrays with generated signal IDs, batched per
  scheduler step. No hierarchical name lookup happens at run time. See
  [How a build works](how-it-works.md).

**Twin bench / peer bench**
: A pure-SystemVerilog testbench that performs the exact same workload as a
  C++ testbench — same stimulus, same checks, same expected counters — so
  the two can be compared on results and on wall time. The examples and the
  authoring-core benchmark kernels each have one; the equivalence and
  performance guards run against them. See
  [Performance](performance.md).

**Write model**
: cpptb's documented write semantics, which are cocotb's: `set()` queues,
  the queue flushes at the settle point, and a write made after an awaited
  edge lands for the *next* edge. `set_now()` is the marked immediate
  escape hatch. `deferred_writes = false` restores legacy immediate writes
  and is on a deprecation path. See
  [Scheduling](scheduling.md#the-write-model).
