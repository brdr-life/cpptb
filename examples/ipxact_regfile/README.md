# IP-XACT register model

This example treats `component.xml` as the authored register contract. The
build imports it through PeakRDL, generates a typed C++ model under `build/`,
and compiles that model into an APB testbench. No generated source is checked
in.

The contract contains ordinary fields, an enum field, an array of registers,
and a native IP-XACT memory address block. `testbench.cpp` exercises all four
through the generated model. `systemverilog/ipxact_regfile_sv_tb.sv` runs the
same APB sequence directly in SystemVerilog.

From this directory:

```sh
make test
```

The individual targets are `make model-test`, `make run`, and `make sv-run`.
The model test uses a fake memory-mapped master and does not start a simulator.
