# Open-source core benchmarks

The open-core suite applies the four-mode comparison to unmodified,
production-style RTL rather than project-authored benchmark DUTs. Each core
is pinned with its upstream license, and each mode performs the same signal
updates, waits, checks, and checksum folding.

| Workload | Core | Verification shape |
|---|---|---|
| PicoRV32 firmware | [YosysHQ/picorv32](https://github.com/YosysHQ/picorv32) | CPU execution with infrequent testbench interaction |
| AES-128 ECB | [secworks/aes](https://github.com/secworks/aes) | Register programming and multi-cycle command completion |
| Ethernet FCS64 | [alexforencich/verilog-ethernet](https://github.com/alexforencich/verilog-ethernet) | Sustained 64-bit AXI-stream traffic and per-frame checking |

The upstream sources are vendored under
`benchmarks/framework_comparison/open_cores/third_party/`. cpptb-specific
wrappers do not modify the core RTL.

## PicoRV32 firmware

The testbench writes a small RV32I program into the benchmark memory while
reset is asserted, releases the CPU, and waits for a memory-mapped completion
store. The program runs a dependency-heavy xorshift loop whose iteration count
is supplied as data, not generated RTL.

```cpp
for (uint32_t index = 0; index < kFirmware.size(); ++index)
    co_await program_word(context, index * 4u, kFirmware[index]);
co_await program_word(context, 0x100u, context.iterations);

co_await FallingEdge{context.dut.clk};
context.dut.cpu_prog_we.set(0);
context.dut.rst_n.set(1);

co_await RisingEdge{context.dut.cpu_done};
co_await Delay{1_ps};

check(context, "firmware result", context.dut.cpu_result.get(), expected);
check(context, "CPU trap", context.dut.cpu_trap.get(), 0);
```

Most cycles execute wholly inside Verilator, making this workload useful for
measuring framework overhead when the testbench does not cross the DPI
boundary every clock.

## secworks AES-128

AES is programmed through the core's documented 32-bit register interface.
The bench expands one AES-128 key and checks the four ECB vectors from NIST SP
800-38A.

```cpp
for (uint32_t block = 0; block < context.iterations; ++block) {
    const uint32_t vector = block & 3u;
    for (uint32_t word = 0; word < 4; ++word)
        co_await aes_write(context, 0x20u + word,
                           kAesPlaintext[vector][word]);

    co_await aes_write(context, 0x08u, 2u);
    co_await aes_wait_status(context, 2u);

    for (uint32_t word = 0; word < 4; ++word) {
        const uint32_t actual = co_await aes_read(context, 0x30u + word);
        check(context, "AES ciphertext word", actual,
              kAesCiphertext[vector][word]);
        fold(context, actual);
    }
}
```

`aes_wait_status` requires the selected status bit to clear and then set. This
avoids accepting the wrapper's one-cycle-old `ready` or `valid` value as the
completion of a new command.

## 64-bit Ethernet FCS

The streaming bench generates legal 64-1518 byte Ethernet frame lengths,
packs bytes into little-endian AXI-stream lanes, handles a partial final beat,
and inserts a deterministic idle cycle every 17 beats.

```cpp
for (uint32_t offset = 0; offset < length; offset += 8u, ++beat) {
    const uint32_t bytes = std::min(8u, length - offset);
    uint64_t data = 0;
    for (uint32_t lane = 0; lane < bytes; ++lane) {
        const uint8_t value = frame_byte(packet, offset + lane);
        data |= static_cast<uint64_t>(value) << (lane * 8u);
        expected = crc32_byte(expected, value);
    }

    co_await FallingEdge{context.dut.clk};
    context.dut.fcs_tdata.set(data);
    context.dut.fcs_tkeep.set((1u << bytes) - 1u);
    context.dut.fcs_tlast.set(offset + bytes == length);
    context.dut.fcs_tvalid.set(1);
    co_await RisingEdge{context.dut.clk};
}

co_await Delay{1_ps};
check(context, "Ethernet FCS", context.dut.fcs_result.get(), ~expected);
```

## Reference results

This reference run used Verilator 5.050 and Cocotb 2.0.1. Each value is the
median whole-process wall time over four serialized, mode-rotated samples and
is normalized to the matching pure-SV testbench.

| Workload | Pure SV | C++ DPI | C++ VPI | Cocotb |
|---|---:|---:|---:|---:|
| PicoRV32, 20,000 loops | 490.5 ms / 1.00x | 603.4 ms / 1.23x | 1092.8 ms / 2.23x | 4062.0 ms / 8.28x |
| AES-128, 4,000 blocks | 202.5 ms / 1.00x | 290.1 ms / 1.43x | 512.2 ms / 2.53x | 7000.4 ms / 34.57x |
| Ethernet FCS, 2,000 frames | 407.9 ms / 1.00x | 366.9 ms / 0.90x | 1325.6 ms / 3.25x | 13111.4 ms / 32.14x |

The DPI/pure-SV advisory threshold is exceeded by PicoRV32 and AES and passes
for Ethernet FCS. Absolute timings are machine-specific; the
[Performance](../performance.md#open-source-core-comparison) page records the
simulation cycles, system-load caveat, and complete methodology.

## Run the suite

```sh
make framework-comparison-open-cores-test
make framework-comparison-open-cores-benchmark
```

Run one core independently when measuring on a busy workstation:

```sh
python3 benchmarks/framework_comparison/open_cores/run_benchmark.py \
  --skip-build --workload picorv32_firmware
```

The runner launches only one simulator process at a time and rotates mode
order over four measured rounds. Every sample must match the pure-SV
transactions, checks, simulation cycles, checksum, and failure count.

Raw per-round results are retained beside the suite under `results/`.
