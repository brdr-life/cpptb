# Code generation

`cpptb-codegen` elaborates the configured SystemVerilog sources with Slang and
renders three design-specific files:

- A typed C++ DUT hierarchy.
- C++ binding metadata and access callbacks.
- A SystemVerilog DPI wrapper containing the transport and event hooks.

```sh
uv run --frozen cpptb-codegen path/to/design.dpi.json
uv run --frozen cpptb-codegen path/to/design.dpi.json --check
```

The checked-in examples and conformance suite commit generated output so users
can inspect the boundary and CI can detect stale files. Generated files begin
with a `Do not edit by hand` notice.

The manifest format, clock sources, hierarchy mapping, wide values, arrays,
and internal probes are documented in the
[generator reference](../tools/codegen/README.md).
