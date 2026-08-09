# Code generation

`cpptb build` is the user-facing entry point. Internally, `cpptb-codegen`
elaborates the configured SystemVerilog sources with Slang and renders the
design-specific files needed by the simulator:

- A typed C++ DUT hierarchy and stable `dut.hpp` include.
- C++ binding metadata and access callbacks.
- A SystemVerilog DPI wrapper containing the event hooks.
- A C++ DPI adapter connecting the generated transport to the public test API.

```sh
cpptb build
```

The build backend invokes `cpptb-codegen` twice: once to create the typed
interface, and again to finalize the hierarchy transport from the access
set. The access set is recovered by compiling the testbench translation
units alone -- no link, and no user test code ever executes at build time
-- and scanning the objects for the section records the framework headers
plant. Clocks are not a build input: `start_clock()` registers them at run
time and the generated wrapper drives whichever writable one-bit signals
were registered. This sequence is cached and does not belong in a user
Makefile. The low-level source-first command remains available for custom build
integrations and writes to `build/cpptb/<target>/generated` by default:

```sh
uv run --frozen cpptb-codegen rtl/design.sv
```

Top-level interface instances, selected modports, interface parameters,
constructor connections, fixed interface arrays, and `inout` drive intent are
also derived from the elaborated source. See
[SystemVerilog interfaces and bidirectional signals](interfaces.md).

The generated hierarchy records whether each HDL object is two-state or
four-state, but transport capability is a separate simulator property. The
experimental Verilator request runs a semantic probe before generation can
enable four-state transport; normal builds do not run the probe. See
[four-state values](four-state.md) for the API and fail-closed activation
contract.

Pass multiple RTL files together and use `--top` only when source elaboration
cannot choose one root unambiguously. Code generation discovers port shape,
but does not assign clock roles or timing. The C++ testbench does that:

```cpp
dut.write_clk.set_now(0);
dut.read_clk.set_now(0);
test.start_clock(dut.write_clk, 4_ns);
test.start_clock(dut.read_clk, 6_ns, 1_ns);
```

The generated wrapper provides interest-gated edge observation for scalar
one-bit outputs. Input clocks registered by the C++ test use a simulator-side
periodic process. DUT-produced clocks and handshake outputs can be awaited
directly without an edge-observer option.

Source-driven generation writes to
`build/cpptb/<target>/generated` by default. The directory is disposable and
should remain ignored by version control. Generated files begin with a
`Do not edit by hand` notice. Internal conformance fixtures may still commit
generated snapshots when a regression specifically needs to compare them.

The public project CLI is the normal user workflow. `cpptb-codegen` and
version-1 manifests remain supported as lower-level compatibility surfaces;
they are not required for ordinary targets. Hierarchy and internal objects are
always inferred from the elaborated RTL. See
[hierarchical DUT access](hierarchy.md) for the generated API and usage-pruned
transport contract. The complete legacy schema and frontend notes remain in
`tools/codegen/README.md`.
