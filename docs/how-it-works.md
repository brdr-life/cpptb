# How a build works

This page follows one small DUT from source to a running test, naming every
artifact the build creates along the way. It is the walkthrough version of
[Architecture](architecture.md): that page describes the layers and their
ownership rules; this one shows the pipeline producing them, using the
`examples/counter` project you can build yourself.

Nothing here is needed to write a testbench. Read it when you want to know
what `cpptb build` actually did, or when something in `build/` needs
explaining.

## The three files you write

A minimal project is the design, the testbench, and — optionally — two lines
of configuration:

```
examples/counter/
├── counter.sv        the DUT: clk, rst_n, enable, count[7:0]
├── testbench.cpp     coroutine tests + CPPTB_REGISTER_TEST(...)
└── cpptb.toml        [build] timing_backend + deferred_writes
```

`counter.sv` is fourteen lines: an `always_ff` counter with an async
active-low reset. The testbench drives it in the cocotb shape — write after
the awaited edge, sample at `ReadOnly` — and registers two tests.
`cpptb.toml` names the write model explicitly:

```toml
[build]
timing_backend = "verilator-direct"
deferred_writes = true
```

Both values are the defaults, so a project with no `cpptb.toml` at all builds
identically; the example carries the file to make the choice visible.

## The pipeline

`cpptb build --project examples/counter` runs four steps. Everything lands
under `build/cpptb/counter/`:

```
 counter.sv                testbench.cpp
     │                          │
     ▼                          ▼
 [1] elaborate            [2] discovery compile
     the design               -DCPPTB_HIERARCHY_DISCOVERY
     (ports, widths,          objects are scanned, never
     parameters)              linked, never executed
     │                          │
     │                          ▼
     │                     access.json  ── which internal paths
     │                          │          the testbench touches
     ▼                          ▼
 [3] cpptb-codegen  ──────────────────────────────
     │
     ├── generated/dpi_counter.sv       the SV wrapper: DUT instance,
     │                                  clocks, timers, DPI trunk
     ├── generated/dpi_counter.cpp      the C++ DPI adapter
     ├── generated/counter_dut.hpp      the typed Dut struct
     ├── generated/counter_binding.hpp  signal metadata + binding call
     └── generated/dut.hpp             `using Dut = ...` alias
     │
     ▼
 [4] one Verilator build
     wrapper + framework host loop + your testbench.cpp
     │
     ▼
 obj/Vdpi_counter          one self-contained simulator process
```

A fingerprint over every input — sources, flags, tool versions, the
`cpptb.toml` keys — decides whether any of this reruns. A cache hit returns
the existing binary; any miss starts the simulator build clean, because an
object compiled under different `-D` flags would otherwise be reused
silently.

## Step 1 — elaborating the design

The codegen frontend elaborates the RTL with Slang, resolving the top
module's ports, widths, and parameters into a design IR. For `counter`
that is four ports; for `core_ibex_cpptb` it is 136 sources and a parameter
map — the same step either way. This is where a `cpptb.toml` parameter block
becomes elaboration parameters, so a model generated for the wrong
configuration fails to compile rather than simulating the wrong core.

## Step 2 — discovering what the testbench touches

Ports are always accessible. But a testbench may also probe *internal*
hierarchy — a RAM behind two module levels, a status flop — and the wrapper
only carries transport for what is actually used.

Each testbench translation unit is compiled once with
`-DCPPTB_HIERARCHY_DISCOVERY`. Under that define, every hierarchy access in
the source plants a marker record in a dedicated object-file section. The
build scans those sections and writes the union to `access.json`. The
objects are never linked and no test code ever executes at build time — the
access set comes from the compiler, not from a trial run. A path the scan
missed would be absent from the generated catalog, and the testbench then
fails to compile with a `static_assert` naming the missing path.

## Step 3 — the generated sources

`cpptb-codegen` now has everything it needs and emits five files into
`generated/`. The build actually invokes the generator twice: once before
the discovery compile, so the testbench has a typed `Dut` to compile
against, and again afterward to finalize the hierarchy transport from the
recovered access set — which is why a probe of internal hierarchy appears
in the generated struct only when the testbench actually uses it.

**`counter_dut.hpp`** is the C++ face of the design — a plain struct with
one typed member per port (plus one per discovered internal path):

```cpp
namespace cpptb::generated::counter {

enum SignalId : uint32_t {
    kSignalClk, kSignalRstN, kSignalEnable, kSignalCount,
    kCpptbSignalCount,
};

struct Dut {
    cpptb::dpi::StaticPackedSignal<1, true, true,  kSignalClk,    0> clk;
    cpptb::dpi::StaticPackedSignal<1, true, true,  kSignalRstN,   1> rst_n;
    cpptb::dpi::StaticPackedSignal<1, true, true,  kSignalEnable, 2> enable;
    cpptb::dpi::StaticPackedSignal<8, false, false, kSignalCount, 0> count;
};

}  // namespace cpptb::generated::counter
```

Width, direction, signal ID, and word offset are template parameters, so
`dut.count.get()` compiles down to reading a word at a fixed offset in the
transport buffer — no name lookup, no map, nothing resolved at run time.
`dut.hpp` aliases this struct to `cpptb::Dut`, which is why every testbench
starts with `#include "dut.hpp"` and receives a `Dut` by value: it is a
handle over shared transport state, cheap to copy.

**`dpi_counter.sv`** is the real top module Verilator elaborates. It
instantiates your `counter`, and around it owns everything that has to live
on the SystemVerilog side:

- the clock drivers for clocks registered by `test.start_clock(...)`,
  configured by querying the runtime at time zero;
- the edge watchers and the timer owner — processes that observe awaited
  edges and hold the earliest framework timer deadline, waking the C++
  scheduler when either fires;
- the DPI trunk: an `import` for scheduler steps
  (`cpptb_counter_dpi_step`), an idempotent output pull, timer-deadline and
  edge-interest queries, and an `export` the C++ side calls to dispatch a
  phase — the `ReadWrite` / `ReadOnly` / `NextTimeStep` points are driven by
  the framework host loop through that export, not by a wrapper process.

Signal values cross the boundary as packed word arrays, batched per step —
for `counter`, one input word carries `count` toward C++, and three output
words carry `clk`, `rst_n`, `enable` back.

**`dpi_counter.cpp`** is the C++ end of the trunk: the generated adapter
translation unit that receives scheduler steps and moves the packed words
between the wrapper and the runtime.

**`counter_binding.hpp`** carries the metadata that ties the two together —
signal tables, the binding call, the registration entry point — consumed by
the design's `DpiAdapter` (see [Architecture](architecture.md#reusable-dpi-runtime)).

## Step 4 — one Verilator build

With a timing backend configured (and there is no supported build without
one), Verilator runs in `--cc --exe` mode and links three things into one
executable:

1. the Verilated model of `dpi_counter.sv` (your DUT inside the wrapper),
2. the framework host loop, `src/verilator_timing_main.cpp`, compiled with
   the defines the `cpptb.toml` keys resolve to — `CPPTB_VERILATOR_DIRECT_TIMING`
   for the direct backend, `CPPTB_DEFERRED_WRITES` for the write model,
3. your `testbench.cpp`, whose `CPPTB_REGISTER_TEST` initializers install
   the test catalog at load time.

The result, `obj/Vdpi_counter`, is one self-contained process. There is no
separate simulator to install and no shared library to preload; running a
different test is running the same binary with a different selection
(`CPPTB_TEST=counter_sequence ./Vdpi_counter`).

## What happens when it runs

At time zero the wrapper's `initial` block calls `cpptb_counter_dpi_init`,
then makes the first scheduler step with `PHASE_INIT`. On the C++ side that
step constructs the scheduler, looks up the selected test in the compiled
catalog, and spawns it as the root process. The test's first actions —
`dut.clk.set_now(0)`, `test.start_clock(dut.clk, 10_ns)` — reach back
through the trunk: the wrapper queries the clock configuration and its
generated driver starts toggling.

From then on the loop is symmetrical:

- an edge or timer fires on the SV side → one `dpi_step` call carries the
  packed input words across → the scheduler resumes every coroutine waiting
  on that event, runs them until they all suspend again, and returns flags
  saying what it now needs (edges, a timer deadline, a phase, changed
  outputs);
- writes made by those coroutines queue in the runtime and flush at the
  timestep's `ReadWrite` point — after the edge's own updates, which is
  what makes the cocotb shape work — and the wrapper pulls the changed
  output words only when the step says they changed.

When the root test coroutine completes (or a fatal check fires), the result
is reported through the same trunk, the wrapper reaches `$finish`, and the
process exit code is the test verdict.

## Where to look next

- [Architecture](architecture.md) — the layer and ownership rules this
  pipeline's output obeys.
- [Code generation](code-generation.md) — driving the generator directly,
  manifests, and register-model generation.
- [Scheduling](scheduling.md) — the authoritative reference for phases,
  edges, and the write model the host loop and wrapper implement together.
