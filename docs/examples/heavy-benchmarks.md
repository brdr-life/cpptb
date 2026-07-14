# Heavy benchmark testbenches

The four-mode heavy suite is designed to include meaningful DUT activity and
host-side verification work. Pure SystemVerilog, C++ DPI, raw C++ VPI, and
Cocotb use the same RTL, stimulus functions, edge sequence, scoreboards, and
checksums.

The C++ DPI testbench remains ordinary explicit signal access and scheduling.
Generated bindings and DPI transport are outside the user-facing file.

Run the semantic smoke matrix or reproduce the scaled measurements with:

```sh
make framework-comparison-heavy-test
make framework-comparison-heavy-benchmark
```

The tabbed blocks below are excerpts from the complete runnable testbench
files. The commands build each full testbench, its simulator wrapper, and the
shared RTL before executing it.

## Latest measurements

The July 14, 2026 local reference run used Verilator 5.050 and Cocotb 2.0.1.
The table reports median whole-process wall time over four serialized,
mode-rotated samples after one warm-up per mode. Ratios are normalized to the
exact matching pure SystemVerilog testbench in each row.

| Workload | Work units | Pure SV | cpptb (C++ DPI) | C++ VPI | Cocotb |
|---|---:|---:|---:|---:|---:|
| 32-tap streaming FIR | 100,000 samples | 47.8 ms / 1.00x | 84.1 ms / **1.76x** | 120.2 ms / **2.51x** | 2684.5 ms / **56.16x** |
| Variable-length packet CRC32 | 2,000 packets | 41.1 ms / 1.00x | 70.1 ms / **1.70x** | 117.4 ms / **2.86x** | 2281.2 ms / **55.51x** |
| 4x4 signed matrix accelerator | 2,000 blocks | 36.5 ms / 1.00x | 66.4 ms / **1.82x** | 103.3 ms / **2.83x** | 1989.1 ms / **54.50x** |

Every measured sample reported identical transactions, checks, simulation
cycles, checksums, and zero failures across all four modes. The observed
one-minute system load average was 3.10-3.46. These are machine-specific
reference values; see [Performance](../performance.md#heavy-four-mode-comparison)
for the methodology and interpretation.

## Streaming FIR

The testbench keeps an independent 32-sample history and computes the expected
signed result before driving each sample. These are the equivalent process
bodies from all four benchmark modes:

<div class="cpptb-code-tabs" data-tabs="4" data-tab-group="simulator" data-tab-label="FIR testbench implementation"></div>

<div class="cpptb-code-tab-label">cpptb (C++ DPI)</div>

```cpp
Task<void> run_fir(Context& context) {
    std::array<int16_t, 32> history{};
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const int16_t sample = fir_sample(iteration);
        int64_t expected = static_cast<int64_t>(sample) * fir_coefficient(0);
        for (uint32_t tap = 1; tap < history.size(); ++tap) {
            expected += static_cast<int64_t>(history[tap - 1]) *
                        fir_coefficient(tap);
        }
        for (uint32_t tap = history.size() - 1; tap > 0; --tap) {
            history[tap] = history[tap - 1];
        }
        history[0] = sample;

        co_await FallingEdge{context.dut.clk};
        context.dut.fir_in_sample.set(static_cast<uint16_t>(sample));
        context.dut.fir_in_valid.set(1);
        co_await RisingEdge{context.dut.clk};
        co_await Delay{1_ps};

        const uint32_t result = context.dut.fir_out_result.get();
        check(context, "FIR result", result, static_cast<uint32_t>(expected));
        fold(context, result);
        ++context.result.transactions;
    }
    context.dut.fir_in_valid.set(0);
    check(context, "FIR accepted sample count",
          context.dut.fir_sample_count.get(), context.iterations);
}
```

<div class="cpptb-code-tab-label">C++ VPI</div>

```cpp
Task<void> run_fir(Context& context) {
    std::array<int16_t, 32> history{};
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const int16_t sample = static_cast<int16_t>(stimulus(iteration));
        int64_t expected = static_cast<int64_t>(sample) * fir_coefficient(0);
        for (uint32_t tap = 1; tap < history.size(); ++tap)
            expected += static_cast<int64_t>(history[tap - 1]) *
                        fir_coefficient(tap);
        for (uint32_t tap = history.size() - 1; tap > 0; --tap)
            history[tap] = history[tap - 1];
        history[0] = sample;

        co_await FallingEdge{context.dut.clk};
        context.dut.fir_in_sample.set(static_cast<uint16_t>(sample));
        context.dut.fir_in_valid.set(1);
        co_await RisingEdge{context.dut.clk};
        co_await Delay{1_ps};

        const uint32_t result = context.dut.fir_out_result.get();
        check(context, "FIR result", result, static_cast<uint32_t>(expected));
        fold(context, result);
        ++context.result.transactions;
    }
    context.dut.fir_in_valid.set(0);
    check(context, "FIR accepted sample count",
          context.dut.fir_sample_count.get(), context.iterations);
}
```

<div class="cpptb-code-tab-label">Cocotb</div>

```python
async def run_fir(dut, state):
    history = [0] * 32
    for iteration in range(state.iterations):
        sample = signed16(stimulus(iteration))
        expected = sample * fir_coefficient(0)
        expected += sum(
            history[tap - 1] * fir_coefficient(tap)
            for tap in range(1, 32)
        )
        history[1:] = history[:-1]
        history[0] = sample

        await FallingEdge(dut.clk)
        dut.fir_in_sample.value = sample & 0xFFFF
        dut.fir_in_valid.value = 1
        await RisingEdge(dut.clk)
        await Timer(1, unit="ps")

        result = int(dut.fir_out_result.value)
        state.check("FIR result", result, expected)
        state.fold(result)
        state.transactions += 1
    dut.fir_in_valid.value = 0
    state.check(
        "FIR accepted sample count",
        int(dut.fir_sample_count.value),
        state.iterations,
    )
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
task automatic run_fir();
  logic signed [15:0] history [0:31];
  logic signed [15:0] sample;
  longint signed expected;
  history = '{default: '0};
  for (int unsigned iteration = 0; iteration < iterations; iteration++) begin
    sample = fir_sample(iteration);
    expected = sample * fir_coefficient(0);
    for (int tap = 1; tap < 32; tap++)
      expected += history[tap - 1] * fir_coefficient(tap);
    for (int tap = 31; tap > 0; tap--) history[tap] = history[tap - 1];
    history[0] = sample;

    @(negedge clk);
    fir_in_sample = sample;
    fir_in_valid = 1'b1;
    @(posedge clk);
    #1ps;

    check32("FIR result", fir_out_result, expected[31:0]);
    fold(fir_out_result);
    transactions++;
  end
  fir_in_valid = 1'b0;
  check32("FIR accepted sample count", fir_sample_count, iterations);
endtask
```

The default run processes 100,000 samples, so both the DUT and reference model
perform 3.2 million multiply-accumulate terms.

## Packet CRC32

Each iteration is one 32-95 byte packet. The reference CRC is updated for
every byte while `crc_in_last` defines the frame boundary:

```cpp
for (uint32_t packet = 0; packet < context.iterations; ++packet) {
    uint32_t expected = 0xffff'ffffu;
    const uint32_t length = packet_length(packet);

    for (uint32_t offset = 0; offset < length; ++offset) {
        const uint8_t data = packet_byte(packet, offset);
        expected = crc32_byte(expected, data);

        co_await FallingEdge{context.dut.clk};
        context.dut.crc_in_data.set(data);
        context.dut.crc_in_last.set(offset + 1u == length);
        context.dut.crc_in_valid.set(1);
        co_await RisingEdge{context.dut.clk};
    }

    co_await Delay{1_ps};
    const uint32_t result = context.dut.crc_out_result.get();
    check(context, "packet CRC32", result, ~expected);
    fold(context, result);
}
```

The variable frame length prevents the run from reducing to one fixed-period
transaction shape.

## Matrix accelerator

Each block loads two signed 4x4 matrices, pulses `mat_start`, then validates
the ordered output stream:

```cpp
for (uint32_t index = 0; index < 16; ++index) {
    matrix_a[index] = matrix_value(block, 0, index);
    co_await load_matrix_value(context, 0, index, matrix_a[index]);
}
for (uint32_t index = 0; index < 16; ++index) {
    matrix_b[index] = matrix_value(block, 1, index);
    co_await load_matrix_value(context, 1, index, matrix_b[index]);
}

co_await FallingEdge{context.dut.clk};
context.dut.mat_load_valid.set(0);
context.dut.mat_start.set(1);
co_await RisingEdge{context.dut.clk};
co_await FallingEdge{context.dut.clk};
context.dut.mat_start.set(0);

for (uint32_t output = 0; output < 16; ++output) {
    co_await RisingEdge{context.dut.clk};
    co_await Delay{1_ps};

    const uint32_t row = output / 4u;
    const uint32_t column = output % 4u;
    int64_t expected = 0;
    for (uint32_t element = 0; element < 4; ++element) {
        expected += static_cast<int64_t>(matrix_a[row * 4u + element]) *
                    matrix_b[element * 4u + column];
    }

    check(context, "matrix output index", context.dut.mat_out_index.get(),
          output);
    check(context, "matrix output data", context.dut.mat_out_data.get(),
          static_cast<uint32_t>(expected));
}
```

See [Performance](../performance.md#heavy-four-mode-comparison) for the latest
reference matrix and measurement cautions.
