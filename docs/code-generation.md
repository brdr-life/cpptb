# Code generation

You never write the glue between your testbench and the simulator — cpptb
generates it from the RTL. This page is the high-level tour: what a build
produces, one complete small example, and the lower-level surfaces for custom
build integrations. [How a build works](how-it-works.md) walks the same
pipeline step by step, with every artifact and design decision explained.

## What a build produces

`cpptb build` elaborates the configured SystemVerilog sources with Slang,
discovers which signals the testbench touches, and renders five
design-specific files into `build/cpptb/<target>/generated/`. For a top
module named `counter`:

| File | What it is |
|---|---|
| `counter_dut.hpp` | The typed `Dut` struct — one member per port and per discovered internal path |
| `dut.hpp` | `using Dut = ...` — the stable include every testbench uses, regardless of the top name |
| `dpi_counter.sv` | The SV wrapper: DUT instance, clock drivers, edge watchers, and the DPI trunk |
| `dpi_counter.cpp` | The C++ DPI adapter connecting the generated transport to the public test API |
| `counter_binding.hpp` | Signal tables and the binding call that tie the two sides together |

One Verilator `--cc --exe` build then links the wrapper, the framework host
loop, and your testbench into a single self-contained simulator process:

```
 counter.sv    testbench.cpp     ← you write these two
      │             │
      ▼             ▼
        cpptb build              elaborate → discover → generate → compile
              │
              ▼
  build/cpptb/counter/
  ├── generated/                 the five files above
  └── obj/Vdpi_counter           one self-contained simulator
```

No user test code executes during the build: the access set is recovered by
compiling the testbench translation units alone and scanning the objects, and
clocks are not a build input at all — `start_clock()` registers them at run
time. The whole sequence is cached on a fingerprint over sources, flags, and
tool versions; the [`timing_backend` and `deferred_writes`](cpptb-toml.md)
keys are part of that fingerprint, because flipping either changes the `-D`
flags every object is compiled under, so a change rebuilds clean rather than
silently reusing mismatched objects.

## A complete small example

`examples/counter` is the whole story in three files. The DUT:

```systemverilog
module counter (
    input  logic       clk,
    input  logic       rst_n,
    input  logic       enable,
    output logic [7:0] count
);
  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) count <= '0;
    else if (enable) count <= count + 1'b1;
  end
endmodule
```

From those ports, generation derives the typed struct — an excerpt of
`generated/counter_dut.hpp`:

```cpp
struct Dut {
    cpptb::dpi::StaticPackedSignal<1, true, true,  kSignalClk,    0> clk;
    cpptb::dpi::StaticPackedSignal<1, true, true,  kSignalRstN,   1> rst_n;
    cpptb::dpi::StaticPackedSignal<1, true, true,  kSignalEnable, 2> enable;
    cpptb::dpi::StaticPackedSignal<8, false, false, kSignalCount, 0> count;
};
```

Width, direction, and word offset are template parameters, so
`dut.count.get()` compiles down to reading a word at a fixed offset in the
transport buffer — no name lookup and nothing resolved at run time. The
testbench includes the stable alias and drives the struct directly:

```cpp
#include <cpptb/cpptb.hpp>
#include "dut.hpp"

using cpptb::Dut;

Task<void> counter_sequence(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
    test.start_clock(dut.clk, 10_ns);   // the testbench owns clock timing

    dut.rst_n.set(0);
    dut.enable.set(0);
    co_await clock_cycles(dut.clk, 2);

    dut.rst_n.set(1);
    dut.enable.set(1);

    co_await RisingEdge{dut.clk};
    co_await ReadOnly{};                // let the design settle, then check
    test.expect_eq("enabled count", dut.count.get(), 1u);
}

CPPTB_REGISTER_TEST(counter_sequence);
```

```sh
cpptb test --project examples/counter
```

Misspell a signal and the compiler says so. Probe an internal path the
discovery scan did not see and the testbench fails to compile with a
`static_assert` naming the missing path — never a silent absence at run
time.

## What elaboration derives

Top-level interface instances, selected modports, interface parameters,
constructor connections, fixed interface arrays, and `inout` drive intent are
all derived from the elaborated source; see
[Interfaces and inouts](interfaces.md). The generated hierarchy records
whether each HDL object is two-state or four-state, but transport capability
is a separate simulator property — the experimental four-state request runs a
semantic probe before generation can enable that transport, and normal builds
never run the probe. See [Four-state values](four-state.md).

Pass multiple RTL files together and use `--top` only when elaboration cannot
choose one root unambiguously. Code generation discovers port shape, but it
does not assign clock roles or timing — the C++ testbench does that:

```cpp
dut.write_clk.set_now(0);
dut.read_clk.set_now(0);
test.start_clock(dut.write_clk, 4_ns);
test.start_clock(dut.read_clk, 6_ns, 1_ns);
```

## Generated output and its lifetime

Generation writes to `build/cpptb/<target>/generated` by default. The
directory is disposable and should remain ignored by version control;
generated files begin with a `Do not edit by hand` notice. Internal
conformance fixtures may still commit generated snapshots when a regression
specifically needs to compare them.

## Lower-level surfaces

The public [`cpptb` command](cli.md) is the normal workflow. `cpptb-codegen`
remains available when another build system owns the compile — it writes the
same five files and takes RTL sources directly:

```sh
uv run --frozen cpptb-codegen rtl/design.sv
```

Its options fall into three groups:

| Group | Options | Purpose |
|---|---|---|
| Selection and naming | `--top`, `--output-dir`, `--target`, `--namespace`, `--root-type` | Pick the root module and control where outputs land and what they are called |
| Explicit declaration | `--clock` (repeatable), `--primary-clock`, `--edge-observer` (repeatable) | The escape hatch when runtime discovery is not wanted: declare clocks and observed DUT outputs up front instead of registering them at run time |
| Inspection | `--inspect-hierarchy`, `--hierarchy-json`, `--check-hierarchy` | Print or export the elaborated hierarchy, or verify it against a previous export |

Ordinary projects need none of the declaration flags — `start_clock()`
registers clocks at run time, and DUT-produced clocks and handshake outputs
are awaited directly through interest-gated edge observation in the generated
wrapper. Version-1 manifests remain supported as a compatibility surface for
manifest-driven flows; the complete legacy schema and frontend notes live in
`tools/codegen/README.md`.

See [hierarchical DUT access](hierarchy.md) for the generated API and the
usage-pruned transport contract, and [How a build works](how-it-works.md) for
the pipeline that produces all of this.
