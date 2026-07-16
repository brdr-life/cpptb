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

<div class="cpptb-code-tabs" data-tabs="2" data-tab-group="open-core-simulator" data-tab-label="PicoRV32 testbench implementation"></div>

<div class="cpptb-code-tab-label">cpptb (C++ DPI)</div>

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

<div class="cpptb-code-tab-label">Cocotb</div>

```python
async def run_picorv32(dut, state):
    await FallingEdge(dut.clk)
    dut.rst_n.value = 0
    for index, instruction in enumerate(FIRMWARE):
        await program_word(dut, index * 4, instruction)
    await program_word(dut, 0x100, state.iterations)

    await FallingEdge(dut.clk)
    dut.cpu_prog_we.value = 0
    dut.rst_n.value = 1
    await RisingEdge(dut.cpu_done)
    await Timer(1, unit="ps")

    expected = 0x12345678
    for _ in range(state.iterations):
        expected = xorshift32(expected)
    state.check("firmware result", int(dut.cpu_result.value), expected)
    state.check("CPU trap", int(dut.cpu_trap.value), 0)
```

Most cycles execute wholly inside Verilator, making this workload useful for
measuring framework overhead when the testbench does not cross the DPI
boundary every clock.

## secworks AES-128

AES is programmed through the core's documented 32-bit register interface.
The bench expands one AES-128 key and checks the four ECB vectors from NIST SP
800-38A.

<div class="cpptb-code-tabs" data-tabs="2" data-tab-group="open-core-simulator" data-tab-label="AES-128 testbench implementation"></div>

<div class="cpptb-code-tab-label">cpptb (C++ DPI)</div>

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

<div class="cpptb-code-tab-label">Cocotb</div>

```python
async def run_aes(dut, state):
    await reset_dut(dut)
    await aes_write(dut, 0x0A, 1)
    for index, value in enumerate(AES_KEY):
        await aes_write(dut, 0x10 + index, value)
    await aes_write(dut, 0x08, 1)
    await aes_wait_status(dut, 1)

    for block in range(state.iterations):
        vector = block & 3
        for word, value in enumerate(AES_PLAINTEXT[vector]):
            await aes_write(dut, 0x20 + word, value)
        await aes_write(dut, 0x08, 2)
        await aes_wait_status(dut, 2)

        for word, expected in enumerate(AES_CIPHERTEXT[vector]):
            actual = await aes_read(dut, 0x30 + word)
            state.check("AES ciphertext word", actual, expected)
            state.fold(actual)
        state.transactions += 1
```

`aes_wait_status` requires the selected status bit to clear and then set. This
avoids accepting the wrapper's one-cycle-old `ready` or `valid` value as the
completion of a new command.

## 64-bit Ethernet FCS

The streaming bench generates legal 64-1518 byte Ethernet frame lengths,
packs bytes into little-endian AXI-stream lanes, handles a partial final beat,
and inserts a deterministic idle cycle every 17 beats.

<div class="cpptb-code-tabs" data-tabs="2" data-tab-group="open-core-simulator" data-tab-label="Ethernet FCS testbench implementation"></div>

<div class="cpptb-code-tab-label">cpptb (C++ DPI)</div>

```cpp
for (uint32_t offset = 0; offset < length; offset += 8u, ++beat) {
    const uint32_t bytes = std::min(8u, length - offset);
    uint64_t data = 0;
    for (uint32_t lane = 0; lane < bytes; ++lane) {
        const uint8_t value = frame_byte(packet, offset + lane);
        data |= static_cast<uint64_t>(value) << (lane * 8u);
        expected = crc32_byte(expected, value);
    }

    if (((packet + beat) % 17u) == 0u) {
        co_await FallingEdge{context.dut.clk};
        context.dut.fcs_tvalid.set(0);
        co_await RisingEdge{context.dut.clk};
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

<div class="cpptb-code-tab-label">Cocotb</div>

```python
async def run_fcs(dut, state):
    await reset_dut(dut)
    for packet in range(state.iterations):
        length = frame_length(packet)
        expected = 0xFFFFFFFF
        for beat, offset in enumerate(range(0, length, 8)):
            chunk = min(8, length - offset)
            data = 0
            for lane in range(chunk):
                value = frame_byte(packet, offset + lane)
                data |= value << (lane * 8)
                expected = crc32_byte(expected, value)

            if (packet + beat) % 17 == 0:
                await FallingEdge(dut.clk)
                dut.fcs_tvalid.value = 0
                await RisingEdge(dut.clk)
            await FallingEdge(dut.clk)
            dut.fcs_tdata.value = data
            dut.fcs_tkeep.value = (1 << chunk) - 1
            dut.fcs_tlast.value = int(offset + chunk == length)
            dut.fcs_tvalid.value = 1
            await RisingEdge(dut.clk)

        await Timer(1, unit="ps")
        actual = int(dut.fcs_result.value)
        state.check("Ethernet FCS", actual, ~expected)
        state.fold(actual)
        state.transactions += 1
```

## Reference results

This July 16, 2026 reference run used Verilator 5.050 and Cocotb 2.0.1. Each
value is the median whole-process wall time over four serialized, mode-rotated
samples and is normalized to the matching pure-SV testbench.

| Workload | Pure SV | C++ DPI | C++ VPI | Cocotb |
|---|---:|---:|---:|---:|
| PicoRV32, 20,000 loops | 274.8 ms / 1.00x | 357.7 ms / 1.30x | 625.2 ms / 2.27x | 2434.2 ms / 8.86x |
| AES-128, 4,000 blocks | 122.2 ms / 1.00x | 172.6 ms / 1.41x | 315.1 ms / 2.58x | 3986.8 ms / 32.62x |
| Ethernet FCS, 2,000 frames | 261.3 ms / 1.00x | 251.6 ms / 0.96x | 841.6 ms / 3.22x | 7110.3 ms / 27.21x |

The DPI/pure-SV advisory threshold is exceeded by PicoRV32 and AES and passes
for Ethernet FCS. Absolute timings are machine-specific; the
[Performance](../performance.md#open-source-core-comparison) page records the
simulation cycles, system-load caveat, and complete methodology.

## Run the suite

```sh
make framework-comparison-open-cores-test
make framework-comparison-open-cores-benchmark
```

Run a single Cocotb example directly:

```sh
make framework-comparison-open-cores-cocotb-run \
  OPEN_CORES_COCOTB_WORKLOAD=secworks_aes128 \
  OPEN_CORES_COCOTB_ITERS=100
```

The complete testbench is
`benchmarks/framework_comparison/open_cores/testbenches/cocotb/test_open_cores.py`.
The adjacent `run_cocotb.py` is simulator launch glue rather than authored
stimulus.

Run one core independently when measuring on a busy workstation:

```sh
python3 benchmarks/framework_comparison/open_cores/run_benchmark.py \
  --skip-build --workload picorv32_firmware
```

The runner launches only one simulator process at a time and rotates mode
order over four measured rounds. Every sample must match the pure-SV
transactions, checks, simulation cycles, checksum, and failure count.

Raw per-round results are retained beside the suite under `results/`.
