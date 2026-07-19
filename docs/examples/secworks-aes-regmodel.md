# secworks AES register-model oracle

This ground-truth example connects cpptb's generated register model to a
proven open-source core. It uses the pinned secworks AES RTL and the project's
unchanged upstream top-level testbench at commit
[`80dc4718`](https://github.com/secworks/aes/blob/80dc4718e1dcbbdb4b0dd1bdb393d8f7b98981dc/src/tb/tb_aes.v).

The upstream bench is the oracle. A passive `bind` observer records every
register transaction without changing DUT or testbench behavior. The cpptb
and matched pure-SystemVerilog benches must reproduce the same ordered 720
events, 20 NIST AES-128/AES-256 encrypt/decrypt cases, 80 result words, and
`46264475` checksum.

```sh
make secworks-aes-regmodel-equivalence
```

Successful output is concise:

```text
AES_EQUIVALENCE_RESULT oracle_events=720 cpptb_modes=3 cpptb_events=720 sv_events=720 cases=20 checksum=46264475 status=pass
```

## What is compared

The other upstream AES benches exercise the key memory, encipher block,
decipher block, or core submodule. They are useful unit tests but are not
longer top-level software-visible workloads. The scalable benchmark therefore
repeats the complete, oracle-proven 20-case suite instead of inventing a
different transaction stream.

The observer is enabled only by `+AES_BUS_TRACE`. Normal benchmark runs do not
format or print each event.

<div class="cpptb-code-tabs" data-tabs="3" data-tab-group="aes-regmodel-oracle" data-tab-label="AES register programming"></div>

<div class="cpptb-code-tab-label">cpptb generated RegModel</div>

```cpp
AesMaster master{dut};
secworks_aes_regs::RegModel<AesMaster> regs{test, master};

for (std::size_t word = 0; word < key.size(); ++word)
    co_await key_registers(regs)[word]->write(key[word]);

co_await regs.config.write(aes256 ? 2u : 0u);
co_await regs.control.write(1u);
co_await Delay{200_ns};

const auto result0 = co_await regs.result0.read();
test.expect_eq("AES result word", result0.data, expected[0]);
```

<div class="cpptb-code-tab-label">Pinned upstream oracle</div>

```systemverilog
write_word(ADDR_KEY0, key[255:224]);
write_word(ADDR_KEY1, key[223:192]);
// ...the remaining key words...
write_word(ADDR_CONFIG, key_length ? 8'h02 : 8'h00);
write_word(ADDR_CTRL, 8'h01);
#(100 * CLK_PERIOD);

read_word(ADDR_RESULT0);
result_data[127:96] = read_data;
```

<div class="cpptb-code-tab-label">Matched pure SystemVerilog</div>

```systemverilog
for (integer word = 0; word < 8; ++word)
  write_word(ADDR_KEY0 + word[7:0], key[255 - word * 32 -: 32]);

write_word(ADDR_CONFIG, aes256 ? 32'h2 : 32'h0);
write_word(ADDR_CTRL, 32'h1);
#(100 * CLK_PERIOD);

for (integer word = 0; word < 4; ++word)
  read_word(ADDR_RESULT0 + word[7:0],
            result_data[127 - word * 32 -: 32]);
```

The authored register contract is
`benchmarks/regmodel_ground_truth/secworks_aes/registers/aes.rdl`. PeakRDL
generates the typed C++ model under `build/`; no generated header is checked
into the source tree.

## Longer performance run

```sh
make secworks-aes-regmodel-benchmark
```

The default benchmark executes 180 complete suites, or 3,600 AES cases, in
each implementation. Processes are serialized and their order alternates.
The runner validates work count, checks, failures, and checksum before using
whole-process wall time.

On July 18, 2026 with Verilator 5.050 and symmetric `OPT_FAST=-O3`, 15
diagnostic runs produced these local medians. The one-minute load average moved
from 3.74 to 4.08 on eight logical CPUs, above the current normalized-load
admission limit:

| Workload | Pure SV | cpptb generated RegModel | Ratio | Guard |
|---|---:|---:|---:|---:|
| 180 suites / 3,600 AES cases | 200.7 ms | 318.5 ms | 1.587x | Over `1.10x` |

This is a flagged optimization target, not accepted performance evidence.
Exact semantic equivalence passes. Current runs require at least four balanced,
paired samples and reject normalized one-minute load above `0.30`; the command
still writes its machine-readable result under `build/` when a guard fails.

An instrumented 200-suite run recorded 88,001 delay callbacks and zero clock
callbacks. The generated clock remains in SystemVerilog because no C++ process
waits on it. Same-binary decomposition measured the generated register model
at about 2.6% over the direct bus master, while removing per-access child tasks
changed another 0.5%. The remaining gap is dominated by simulator/DPI/C++
scheduler re-entry at each authored timing boundary, not register lookup.

The complete sources are in
`benchmarks/regmodel_ground_truth/secworks_aes/`.
