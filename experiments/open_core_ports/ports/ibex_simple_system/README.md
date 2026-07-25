# Ibex Simple System

**Status: not started.** This directory records the intent and the constraints
found while scoping it; no port exists yet.

## What upstream provides

Ibex Simple System is a Verilator simulation of the Ibex core with memory and a
small set of peripherals, which runs a RISC-V binary to completion. Upstream
drives it from `examples/simple_system/ibex_simple_system.cc` and
`ibex_simple_system_main.cc`.

That detail is why this was chosen first: the upstream harness is already
hand-written C++ against Verilator. Replacing it with a cpptb testbench changes
the framework and nothing else, so a comparison is not confounded by also
changing language or methodology, as it would be against a UVM or cocotb peer.

Fetch it with `python3 ../../fetch.py ibex`; the pinned commit is in
`sources.toml`.

## What the port has to do

Drive the same design, run the same binary, and reach the same end state, then
report the same evidence upstream reports so the two can be compared rather
than merely both run.

## Known constraints

**A RISC-V cross toolchain.** Upstream builds its software with
`riscv32-unknown-elf-gcc`. Nothing in this repository needs a cross compiler
today: the PicoRV32 benchmark uses a single `.v` file with a small embedded
program. Pin a prebuilt binary in this directory so the port runs without a
toolchain, and treat rebuilding from source as optional.

**Upstream stays unmodified.** Read from `deps/`, write only here, in `work/`,
or in `results/`. A port that needs upstream changed should say so explicitly
rather than patching a fetched tree, since `deps/` is disposable.

**Peer choice.** The honest performance peer is the upstream C++ harness
itself, since both then run on Verilator against the same RTL. Cocotb and plain
SystemVerilog remain available as additional peers.

## Evidence to record

Semantic agreement between both harnesses, wall time under the validity the
environment guard would demand, testbench lines written versus generated, what
a failure reports before a debugger is opened, and edit-to-result time. See the
[parent README](../../README.md).
