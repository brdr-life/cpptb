# Simple examples

These examples introduce one cpptb idea at a time. Start with the counter, then
add timing, checking, multiple clocks, and cancellation as you need them. Each
one is a complete, runnable project with a pure-SystemVerilog twin.

| Example | Start here for | Source shown |
|---|---|---|
| [Counter](counter.md) | A small, single-clock DUT | Complete testbench |
| [Clockless timers](timer-only.md) | Combinational or clockless models | Complete testbench |
| [FIFO scoreboard](fifo-scoreboard.md) | Streaming drivers, monitors, and scoreboards | Key processes and composition |
| [Multiple clocks](multiclock.md) | Independent clock domains | Producer, consumer, and trigger probe |
| [Watchdogs and timeouts](watchdog-timeout.md) | Operations that may stall or need cancellation | Transaction, timeout, and process lifecycle |

The [counter](counter.md) is the one walked through step by step in
[Getting started](../getting-started.md). When these stop being enough, move on
to the [advanced examples](advanced.md).
