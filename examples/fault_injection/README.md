# Fault injection

This example demonstrates direct hierarchical `get()`, `deposit()`,
`force()`, and `release()` operations on internal nets, variables, and memory.
Those operations never advance simulation time implicitly; the testbench uses
an explicit `Delay` or clock edge when it needs the DUT to react.

Run the C++ DPI and matching pure-SystemVerilog benches with:

```sh
make cpp-dpi-fault-injection-run
make cpp-dpi-fault-injection-sv-run
```

Slang infers the ports and complete elaborated hierarchy directly from the
RTL. The user does not write a manifest or probe list for the internal objects;
the discovery compile retains only the operations used by `testbench.cpp`.

See the rendered [fault-injection guide](../../docs/examples/fault-injection.md)
for the framework shape and annotated hierarchy operations.
