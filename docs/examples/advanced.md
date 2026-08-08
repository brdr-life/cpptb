# Advanced examples

These examples combine reusable components, protocol and register models,
generated artifacts, richer data types, and production-scale RTL. Each one is a
complete, runnable project with a pure-SystemVerilog twin.

| Example | Start here for | Source shown |
|---|---|---|
| [Component FIFO](component-fifo.md) | Reusable transaction endpoints and analysis fan-out | Complete component composition |
| [APB register file](apb-regfile.md) | Optional protocol component package | Generic sequence, master, monitor, checker, scoreboard, and coverage |
| [APB transaction trace](apb-trace.md) | Timed typed transaction recording | 256-operation monitor, scoreboard, in-memory trace, and JSON Lines output |
| [IP-XACT register model](ipxact-regfile.md) | A standard IP-XACT contract with registers and native memory | Generated hierarchy, fake-master check, APB sequence, and pure-SV peer |
| [Fault injection](fault-injection.md) | Internal access and controlled fault injection | Deposit, force, release, and explicit settling |
| [Rich data](rich-data.md) | Wide, fixed-point, array, struct, and enum ports | Typed construction and checking |
| [Interfaces and inouts](interfaces.md) | Parameterized interfaces, modports, interface arrays, and bidirectional pins | Named member access, independent clocks, drive, and release |
| [Mixed-language logging](mixed-logging.md) | One ordered C++ and RTL diagnostic stream | C++ loggers, SV macros, source provenance, and hierarchy |
| [Heavy benchmarks](heavy-benchmarks.md) | Computationally substantial four-mode comparisons | FIR, packet CRC32, and matrix accelerator sequences |
| [Open-source cores](open-source-cores.md) | Real CPU, crypto, and network RTL comparisons | Firmware, register programming, and AXI-stream sequences |
| [secworks AES register-model oracle](secworks-aes-regmodel.md) | Ground-truth validation of generated register access | Upstream oracle, generated RegModel, and matched pure-SV sequence |

[Fault injection](fault-injection.md) is the smallest complete bench centred on
internal hierarchy access — deposit, force, release, and settling against
inferred RTL objects, with no probe list. Read it alongside
[Hierarchical DUT access](../hierarchy.md).
