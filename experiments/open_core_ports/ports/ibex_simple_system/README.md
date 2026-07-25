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

## Building firmware

Resolved, and needed only when firmware changes. The VMEM a port runs is
committed here, so running the port needs no toolchain at all.

```sh
python3 ../../fetch.py riscv_toolchain            # pinned, sha256 verified
export PATH="$PWD/../../deps/riscv_toolchain/bin:$PATH"
make -C ../../deps/ibex/examples/sw/simple_system/hello_test \
    CC=riscv32-unknown-elf-clang CROSS_COMPILE=riscv32-unknown-elf-
python3 ../../bin2vmem.py <prog>.bin <prog>.vmem
```

Three things differ from the upstream instructions, each found by running them:

**The toolchain is clang, not gcc.** lowRISC's prebuilt release now ships
`riscv32-unknown-elf-clang`; there is no `-gcc`. Ibex's `common.mk` still
defaults to gcc, so `CC` has to be set.

**`CROSS_COMPILE` has to be set too.** `common.mk` derives it with
`$(patsubst %-gcc,%-,$(CC))`, which silently produces
`riscv32-unknown-elf-clangobjcopy` for a clang-named compiler.

**srecord is not required.** Upstream converts the binary with `srec_cat`,
a system package needing root. `bin2vmem.py` does the same job: these memories
are 32 bits wide and read with `$readmemh`, so a VMEM is one hex word per
location, which is what `srec_cat -byte-swap 4` arranges for. Verified against
`objdump`: the words at the image base match `4ac0006f 4a80006f 4a40006f
4a00006f` exactly.

## Loading firmware

`prim_util_memload.svh` offers two routes, and the port should prefer the
first:

1. The `MemInitFile` parameter, reaching the memory as `SRAMInitFile` on
   `ibex_simple_system`, loads a VMEM with `$readmemh` at elaboration. cpptb
   passes design parameters already, so this needs no C++ at all and avoids
   libelf.
2. The exported DPI tasks `simutil_memload` and `simutil_set_mem`, which is how
   upstream's `--meminit` loads an ELF, using libelf from C++.

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
