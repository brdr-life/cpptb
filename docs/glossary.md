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
  probes. Backdoor operations are immediate. The opposite is a
  **frontdoor** access. See
  [Memory and register models](memory-register-models.md).

**Conformance suite**
: The scheduler and timing regression that pins the documented semantics —
  trigger ordering, the phase contract, the write model — on every
  supported backend, run by every `make test`. A backend is supported only
  while it passes the whole suite. See
  [Scheduling](scheduling.md#timing-backend-support).

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

**Frontdoor**
: An access that goes through the design's protocol interface — a bus
  transaction driven on pins — and therefore takes simulation time. Its
  pin drives queue like any port `set()` under the write model. The
  opposite is a **backdoor** access. See
  [Memory and register models](memory-register-models.md).

**Harness and framework**
: The load-bearing packaging distinction. The *framework* is the reusable
  C++ API — scheduler, DUT access, lifecycle, results — that survives
  embedding in someone else's build or CI system. The *harness* is the
  optional reference tooling around it: the `cpptb` command, `cpptb-run`,
  and their process policy. Framework milestones never block on harness
  features. See [Running tests](running-tests.md).

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
  See [Scheduling](scheduling.md#the-write-model).

**Timing backend**
: The mechanism that gives the C++ scheduler its simulator-phase waits.
  `"verilator-direct"` drives Verilator's host loop directly;
  `"vpi"` rides standard VPI callbacks and is the portable path. Every
  project selects one; there is no supported build without a backend. See
  [Scheduling](scheduling.md).

**Transport**
: The generated DPI path that moves signal values between the simulator and
  the C++ side — packed word arrays with generated signal IDs, batched per
  scheduler step. No hierarchical name lookup happens at run time. The
  build walkthrough calls its wrapper-side interface the *DPI trunk*. See
  [How a build works](how-it-works.md).

**Twin bench / peer bench**
: A pure-SystemVerilog testbench that performs the exact same workload as a
  C++ testbench — same stimulus, same checks, same expected counters — so
  the two can be compared on results and on wall time. The examples and the
  authoring-core benchmark kernels each have one; the equivalence and
  performance guards run against them. See
  [Performance](performance.md).

**Wait graph**
: The scheduler's structured snapshot of every parked process — spawn site,
  process ID, and the exact edge, phase, event, queue, lock, or semaphore
  it waits on — with a conservative deadlock classification. Captured
  automatically before a timeout cancels the test, printed, and stored in
  the result JSON. See
  [Scheduling](scheduling.md#wait-graphs-and-deadlock-diagnostics).

**Write model**
: cpptb's documented write semantics, which are cocotb's: `set()` queues,
  the queue flushes at the settle point, and a write made after an awaited
  edge lands for the *next* edge. `set_now()` is the marked immediate
  escape hatch. `deferred_writes = false` restores legacy immediate writes
  and is on a deprecation path. See
  [Scheduling](scheduling.md#the-write-model).
