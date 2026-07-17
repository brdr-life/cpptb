# Functional coverage

Functional coverage records which meaningful transaction cases a test
observed. It does not generate stimulus, wait for simulator time, or read the
DUT implicitly. A driver, monitor, reference model, or directed sequence calls
`sample()` with an ordinary C++ value at the point where that transaction is
considered observed.

## Define and sample a model

```cpp
enum class Opcode : uint8_t { Read, Write, Atomic, Reserved };

struct Packet {
    Opcode opcode;
    uint16_t length;
};

Covergroup<Packet> coverage{"packets"};

auto& opcode = coverage.coverpoint("opcode", &Packet::opcode);
opcode.bin("read", Opcode::Read)
    .bin("write", Opcode::Write)
    .bin("atomic", Opcode::Atomic)
    .illegal_bin("reserved", Opcode::Reserved)
    .transition_bin("read_to_write", Opcode::Read, Opcode::Write);

auto& length = coverage.coverpoint("length", &Packet::length);
length.ignore_bin("empty", uint16_t{0})
    .bin("short", uint16_t{1}, uint16_t{63})
    .bin("medium", uint16_t{64}, uint16_t{511})
    .bin("long", uint16_t{512}, uint16_t{1500})
    .illegal_bin("oversize", uint16_t{1501}, uint16_t{0xffff});

coverage.cross("opcode_x_length", opcode, length);

const CoverageSampleResult sampled = coverage.sample(packet);
test.expect("packet did not hit an illegal coverage bin", sampled.legal());
```

The covergroup accepts integral and enum coverpoint values. Extractors may be
member pointers, lambdas, or other callable objects. Model construction may
allocate; repeated `sample()` calls do not.

## Bin behavior

| Bin | Meaning |
|---|---|
| `bin(name, value)` | Ordinary exact-value bin included in coverage percentage |
| `bin(name, minimum, maximum)` | Ordinary inclusive range bin |
| `ignore_bin(...)` | Counted for diagnostics, excluded from ordinary bins and crosses |
| `illegal_bin(...)` | Counted and returned through `CoverageSampleResult`; not an implicit test failure |
| `transition_bin(name, from, to)` | Covered when consecutive samples move from `from` to `to` |
| `cross(name, left, right)` | Cartesian product of the two coverpoints' ordinary bins |

Illegal and ignore bins take precedence over ordinary and transition bins on
the same coverpoint. Multiple overlapping ordinary bins may all receive a hit,
and their cross combinations are all counted. The model freezes when sampling
starts; adding a point, bin, or cross later produces a clear exception.

Illegal coverage is data, not test lifecycle policy. A monitor may record it
without failing a test, while a protocol checker can turn the returned result
into `expect()` or `require()` as shown above.

## Sample from a monitor

Coverage fits naturally beside analysis publication:

```cpp
Task<void> packet_monitor(Dut dut, TestContext& test,
                          cpptb::vc::AnalysisPort<Packet>& observed,
                          Covergroup<Packet>& coverage) {
    while (true) {
        co_await RisingEdge{dut.clk};
        co_await ReadOnly{};
        if (!dut.valid.get() || !dut.ready.get()) continue;

        Packet packet{
            .opcode = static_cast<Opcode>(dut.opcode.get()),
            .length = dut.length.get(),
        };
        test.expect("monitor observed legal coverage",
                    coverage.sample(packet).legal());
        observed.write(packet);
    }
}
```

The timing operations remain visible in the monitor. `coverage.sample()` only
updates in-process counters.

## Compare the authored forms

<div class="cpptb-code-tabs" data-tabs="2" data-tab-group="coverage-comparison" data-tab-label="Functional coverage implementation"></div>

### cpptb

```cpp
Covergroup<Packet> coverage{"packets"};
auto& opcode = coverage.coverpoint("opcode", &Packet::opcode);
opcode.bin("read", Opcode::Read)
    .bin("write", Opcode::Write)
    .illegal_bin("reserved", Opcode::Reserved);

auto& length = coverage.coverpoint("length", &Packet::length);
length.bin("short", uint16_t{1}, uint16_t{63})
    .bin("long", uint16_t{64}, uint16_t{1500});
coverage.cross("opcode_x_length", opcode, length);

coverage.sample(packet);
```

### Pure SystemVerilog

```systemverilog
covergroup packet_coverage with function sample(packet_t packet);
  opcode: coverpoint packet.opcode {
    bins read = {Read};
    bins write = {Write};
    illegal_bins reserved = {Reserved};
  }
  length: coverpoint packet.length {
    bins short = {[1:63]};
    bins long = {[64:1500]};
  }
  opcode_x_length: cross opcode, length;
endgroup

packet_coverage coverage = new;
coverage.sample(packet);
```

The runnable performance twin uses explicit SystemVerilog bin counters because
the reference Verilator flow does not rely on simulator-specific covergroup
reporting. It performs the same ordinary, ignore, illegal, transition, and
cross accounting as the cpptb model.

## Snapshot, merge, and JSON

Take snapshots after sampling or at a reporting boundary:

```cpp
CoverageSnapshot run = coverage.snapshot();
std::printf("coverage %.2f%% (%llu/%llu bins)\n",
            run.coverage_percent(),
            static_cast<unsigned long long>(run.covered_bins()),
            static_cast<unsigned long long>(run.coverable_bins()));

CoverageSnapshot regression = first_seed;
regression.merge(second_seed);
write_coverage_json("coverage.json", regression);
```

`merge()` requires the same group, point, bin, cross names, order, and kinds;
it rejects a different model instead of silently combining unrelated data.
The schema-1 JSON contains model source identity, sample and illegal counts,
every bin hit count, and cross tuples. UCIS import/export remains future work.

## Performance qualification

The exact `coverage_sampling` pair samples one coverage transaction for every
DUT transaction and checks point, transition, and cross accounting before
timing is accepted:

```sh
make feature-test FEATURE=coverage_sampling
make feature-benchmark FEATURE=coverage_sampling
```

The valid July 17, 2026 run measured `0.705x` C++ DPI over pure SystemVerilog
at 100,000 transactions, with `0.702x` DPI-first, `0.716x` SV-first, `0.706x`
independent, and `0.08%` paired/independent disagreement. The repository still
enforces the ordinary `1.10x` hard guard on future changes.
