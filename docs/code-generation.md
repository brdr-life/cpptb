# Code generation

`cpptb-codegen` elaborates the configured SystemVerilog sources with Slang and
renders four design-specific files:

- A typed C++ DUT hierarchy.
- C++ binding metadata and access callbacks.
- A SystemVerilog DPI wrapper containing the event hooks.
- A C++ DPI adapter connecting the generated transport to the public test API.

```sh
uv run --frozen cpptb-codegen rtl/design.sv
uv run --frozen cpptb-codegen rtl/design.sv --check
```

Pass multiple RTL files together and use `--top` only when source elaboration
cannot choose one root unambiguously. Code generation discovers port shape,
but does not assign clock roles or timing. The C++ testbench does that:

```cpp
dut.write_clk.set(0);
dut.read_clk.set(0);
test.start_clock(dut.write_clk, 4_ns);
test.start_clock(dut.read_clk, 6_ns, 1_ns);
```

The generated wrapper provides interest-gated edge observation for scalar
one-bit outputs. Input clocks registered by the C++ test use a simulator-side
periodic process. DUT-produced clocks and handshake outputs can be awaited
directly without an edge-observer option.

The checked-in examples and conformance suite commit generated output so users
can inspect the boundary and CI can detect stale files. Generated files begin
with a `Do not edit by hand` notice.

The source-first CLI is the normal user workflow. Version-1 manifests remain
supported for compatibility and build configuration such as include paths,
defines, source lists, and parameter overrides; they are not required for
ordinary targets. Hierarchy and internal objects are always inferred from the
elaborated RTL. See [hierarchical DUT access](hierarchy.md) for the generated
API and usage-pruned transport contract. The complete legacy schema and
frontend notes remain in `tools/codegen/README.md`.
