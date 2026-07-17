# APB register-file example

This example composes the optional `cpptb_vc` APB components around a small
register file. The testbench imports a generic master, passive monitor,
protocol checker, in-order scoreboard, and functional coverage instead of
defining protocol machinery locally.

```cpp
#include "cpptb_vc/cpptb_vc.hpp"

const auto write = co_await master.write(address, value);
test.expect_eq("APB write status", write.status, MemoryStatus::Okay);

const auto read = co_await master.read(address);
test.expect_eq("APB register readback", read.data, value);
```

The sequence accepts any `MemoryMappedMaster`, so its register programming is
not coupled to APB. The concrete `ApbBus` is assembled directly from generated
DUT signals. Components do not start a clock, reset the DUT, or spawn
themselves.

Run the C++ DPI testbench and its pure-SystemVerilog peer:

```sh
make cpp-dpi-apb-regfile-run
make cpp-dpi-apb-regfile-sv-run
make feature-test FEATURE=dpi_apb_regfile
```

Run the exact 100,000-iteration component performance pair separately:

```sh
make feature-test FEATURE=apb_component
make feature-benchmark FEATURE=apb_component
```

See `docs/verification-components.md` for the package and API contracts.
