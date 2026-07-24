# APB transaction trace

This example performs 128 deterministic APB writes and 128 matching reads
against a 64-word RAM. One passive `ApbMonitor` reconstructs all 256 completed
operations. Its typed observation stream fans out to both an in-order
scoreboard and a transaction recorder.

The example is intentionally larger than a single smoke transaction but small
enough to read in one sitting. It exercises transaction order, payload fields,
begin/end simulation time, completion disposition, and per-stream sequence
numbers without mixing in a larger register model or protocol stack.

## Connect checking and recording

<div class="cpptb-code-tabs" data-tabs="2" data-tab-group="apb-trace-composition" data-tab-label="APB trace composition"></div>

<div class="cpptb-code-tab-label">cpptb-vc (C++ DPI)</div>

```cpp
Master master{bus, ApbConfig{.sample_delay = 1_ps}};
ApbMonitor monitor{test, bus, 1_ps};
AnalysisPort<Transaction> expected;
InOrderScoreboard<Transaction> scoreboard{test, "APB trace transaction"};

TransactionRecorder recorder;
InMemoryTransactionSink trace;
auto trace_sink = recorder.connect(trace);
auto& stream = recorder.stream<Transaction>("apb0.observed");

auto expected_connection = expected.connect(scoreboard.expected());
auto checking = monitor.observed().connect(scoreboard.actual());
auto recording = monitor.observed().connect(stream);

co_await Join{trace_sequence(master, test, expected),
              monitor.run(kTransactionCount)};
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
transaction_t expected_transactions[$];
transaction_t actual_transactions[$];
string recorded_streams[$];
string recorded_types[$];
longint unsigned recorded_sequences[$];
time recorded_begin_times[$];
time recorded_end_times[$];
int unsigned recorded_dispositions[$];
string recorded_json[$];

task automatic publish_observed(
    input transaction_t transaction,
    input time begin_time,
    input time end_time
);
  actual_transactions.push_back(transaction);
  recorded_streams.push_back("apb0.observed");
  recorded_types.push_back("memory_transaction");
  recorded_sequences.push_back(recorded_sequences.size());
  recorded_begin_times.push_back(begin_time);
  recorded_end_times.push_back(end_time);
  recorded_dispositions.push_back(0); // completed
  recorded_json.push_back(format_transaction(transaction));
  compare_available();
endtask

fork
  trace_sequence();
  monitor();
join
```

Both versions decode the APB setup and access phases once, compare the same
256 transaction values, retain the same seven record fields, and exercise 128
single-cycle wait states. They each report 649 checks and 898 simulated cycles.
The C++ monitor owns `observed()`, so adding or removing recording does not
change the driver, scoreboard, or protocol decoder.

## Inspect records

`InMemoryTransactionSink::records()` returns records in publication order:

```cpp
const auto& first = trace.records().front();

test.expect_eq("first sequence", first.sequence, uint64_t{0});
test.expect("completed after setup", first.end_time > first.begin_time);
test.expect("captured write",
            first.value_json.find("\"operation\":\"write\"") !=
                std::string::npos);
```

Use JSON Lines when the trace should survive the process:

```cpp
JsonLinesTransactionSink json{"transactions.jsonl"};
auto json_output = recorder.connect(json);

// Run the monitor and stimulus while json_output remains alive.
co_await Join{monitor.run(kTransactionCount), stimulus(dut, test)};
json.finalize();
```

Each output line contains the stream, sequence, static transaction type,
begin/end time in femtoseconds, disposition, and structured payload:

```json
{"stream":"apb0.observed","sequence":0,"type":"memory_transaction","begin_time_fs":35001000,"end_time_fs":45001000,"disposition":"completed","value":{"operation":"write","address":0,"data":1489221554,"byte_enable":18446744073709551615,"status":"okay","wait_cycles":0}}
```

The sink must outlive its connection. `finalize()` flushes and closes JSON
output and throws an actionable error if writing fails. The destructor only
performs best-effort cleanup.

## Run both versions

```sh
make cpp-dpi-apb-trace-run
make cpp-dpi-apb-trace-sv-run
make feature-test FEATURE=dpi_apb_trace
make feature-test FEATURE=transaction_recording
make feature-benchmark FEATURE=transaction_recording
```

The complete authored sources are under `examples/apb_trace/`. Generated DUT
bindings and simulator transport stay under `build/cpptb/apb_trace/`.

`dpi_apb_trace` is the readable 256-transfer equivalence example.
`transaction_recording` is its 100,000-iteration authoring-core counterpart:
it retains 200,000 records in both C++ and pure SV and applies the standard
`1.10x` hard performance guard. A timing result is published only after the
host passes the repository's low-load admission checks.

See [Transaction recording](../verification-components/transaction-recording.md)
for custom descriptors, stream naming, disabled behavior, and deferred
semantics.
