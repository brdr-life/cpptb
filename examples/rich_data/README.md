# Rich data

This clockless example demonstrates a 137-bit packed value and slice,
Q2.14 fixed-point arithmetic, unpacked arrays, multidimensional arrays with
non-zero and descending bounds, and typed packed struct/enum views.

Run the C++ DPI and matching pure-SystemVerilog benches with:

```sh
make cpp-dpi-rich-data-run
make cpp-dpi-rich-data-sv-run
```

The testbench batches independent writes before one explicit settling delay.
That keeps the scheduling intent visible and avoids unnecessary simulator
round trips.
