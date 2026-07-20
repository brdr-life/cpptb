# Structured logging

cpptb logging is a framework facility. It attaches simulation time, source
location, and lifecycle process provenance to messages without requiring the
reference command-line runner or storing every message in `TestResult`.

Create a logger from the current test context and give it a stable component
or activity scope:

```cpp
Task<void> response_monitor(Dut dut, TestContext test) {
    auto log = test.logger("response_monitor");

    while (true) {
        co_await RisingEdge{dut.response_valid};
        co_await ReadOnly{};
        log.info("response observed");
    }
}

Task<void> protocol_test(Dut dut, TestContext& test) {
    auto log = test.logger("protocol_test");
    log.info("starting traffic");
    test.spawn_detached(response_monitor(dut, test));
    co_await drive_traffic(dut);
}
```

Every emitted `LogRecord` contains:

- severity, message, and user-selected scope;
- origin (`Cpp` or `SystemVerilog`) and, for HDL records, instance hierarchy;
- test name, simulation time in femtoseconds, and a per-test sequence number;
- logging call source file and line; and
- owning process ID, description, spawn source file, and spawn line.

Root and spawned processes use the same stable identities as failures,
warnings, and uncaught exceptions. Logs issued outside a lifecycle-owned
process still retain their scope, source location, and simulation time; their
process ID is zero.

The sequence starts at one for each test. Simulation time orders records across
time steps; sequence order is the stable tie-breaker when several processes log
at the same simulation time. The default console sink prints both values.

```text
cpptb: packet_test.cpp:31: 0 fs #1: info [driver] [process 1: root process]: starting traffic
cpptb: packet_test.cpp:18: 20000000 fs #2: info [monitor] [process 2: spawned process]: response observed
cpptb: packet_test.cpp:44: 20000000 fs #3: info [scoreboard] [process 3: spawned process]: response matched
```

## Levels and filtering

The available levels are `Trace`, `Debug`, `Info`, `Warning`, and `Error`.
`Off` disables every level. A message is enabled when its level is at least
the configured minimum:

```cpp
auto log = test.logger("apb.driver");

log.trace("entered transfer");
log.debug("PSEL asserted");
log.info("write completed");
log.warning("slave inserted ten wait states");
log.error("slave returned an error response");
```

Log levels are observational. `warning()` and `error()` do not increment the
test's warning or failure counts and do not stop the test. Use the lifecycle
APIs when a condition affects the result:

| Intent | API |
|---|---|
| Diagnostic message only | `log.warning(...)` or `log.error(...)` |
| Structured nonfatal test warning | `test.warn(...)` |
| Nonfatal correctness check | `test.expect(...)` or `test.expect_eq(...)` |
| Fatal prerequisite | `test.require(...)` or `test.require_eq(...)` |

The default minimum is `Info`. The reference executable reads
`CPPTB_LOG_LEVEL` through its normal environment request:

```sh
CPPTB_LOG_LEVEL=debug build/cpptb/my_design/obj/Vdpi_my_design
CPPTB_LOG_LEVEL=off build/cpptb/my_design/obj/Vdpi_my_design
```

Accepted values are `trace`, `debug`, `info`, `warning` (or `warn`), `error`,
and `off`. Embedding applications configure the same policy directly:

```cpp
RunRequest request{
    .test_name = "packet_test",
    .logging = {.minimum_level = LogLevel::Debug},
};
run_registered_test(scheduler, dut, result, request);
```

## SystemVerilog messages

RTL and SystemVerilog verification code can publish into the same structured
stream as C++. `cpptb build` automatically compiles the logging package and
DPI bridge; the project file needs no logging sources, include paths, or extra
link settings.

Include the shipped macro header where messages are authored:

```systemverilog
`ifdef CPPTB_ENABLE_SV_LOGGING
`include "cpptb/sv/cpptb_log.svh"
`endif

module packet_buffer (...);
  always_ff @(posedge clk) begin
    if (input_valid && input_ready) begin
`ifdef CPPTB_ENABLE_SV_LOGGING
      `cpptb_debug($sformatf("accepted tag=%0d", input_tag), "input")
`endif
    end
  end
endmodule
```

The available macros are `cpptb_trace`, `cpptb_debug`, `cpptb_info`,
`cpptb_warning` (or `cpptb_warn`), and `cpptb_error`. The optional second
argument is the user scope. The macro captures `` `__FILE__ ``, `` `__LINE__ ``,
`%m`, and `$realtime` at the call site. The message expression is evaluated
only when its level is enabled, so a disabled `$sformatf` stays on the filtered
path.

SystemVerilog and C++ records receive sequence numbers from the same test-owned
counter. They therefore remain deterministic at equal simulation times and
appear in one `LogHistory` in emission order. An HDL record has
`origin == LogOrigin::SystemVerilog`, its `%m` path in `hierarchy`, and
`process_id == 0`; SystemVerilog processes do not have CPPTB lifecycle process
IDs. C++ records retain `std::source_location` and lifecycle process
attribution as before.

```text
cpptb: testbench.cpp:14: 20000000 fs #1: info [cpp.driver] [process 1: root process]: drove request
cpptb: packet_buffer.sv:27: 25000000 fs #2: info [input] [sv TOP.dut.i_buffer]: accepted request
cpptb: testbench.cpp:19: 26000000 fs #3: info [cpp.monitor] [process 1: root process]: observed response
```

The logging levels remain observational on both sides: an SV `cpptb_error`
does not fail the test. Use an assertion, a C++ check, or another explicit
result-changing mechanism for correctness. Plain `$display`, `$warning`, and
simulator diagnostics are not intercepted and continue to use the simulator's
own output stream. Guarding the include and calls with
`CPPTB_ENABLE_SV_LOGGING` keeps the RTL usable in non-CPPTB and synthesis
flows.

The runnable [mixed-language logging example](examples/mixed-logging.md)
shows C++ stimulus and RTL messages interleaved in one trace.

### Portable timestamp conversion

SystemVerilog timestamps are converted with `$realtime / 1s * 1.0e15`. The
seconds literal remains exactly representable when the calling scope has a
timeprecision coarser than 1 fs, while dividing by `1fs` directly could round
that divisor to zero on an IEEE-compliant simulator. The mixed-language
integration test asserts the exact femtosecond values received through DPI.

This fixes a portability bug found during the Verilator-reference review.
Running the same assertion on a second standards-compliant simulator remains
part of the portability milestone.

## Lazy messages

Pass a zero-argument callable when constructing the message requires work:

```cpp
log.debug([&] {
    return "write address=" + std::to_string(address) +
           " data=" + std::to_string(data);
});
```

cpptb checks the level before invoking the callable. A disabled message does
not format values, allocate a message string, or call the sink. `enabled()` is
also available when preparing several values is itself expensive:

```cpp
if (log.enabled(LogLevel::Trace)) {
    const auto snapshot = collect_protocol_snapshot(dut);
    log.trace([&] { return format_snapshot(snapshot); });
}
```

Immediate string messages use the same filtering path. C++ logging after the
test has reached a terminal state is ignored. SystemVerilog records emitted
after test completion, or after test selection fails, are unowned diagnostics:
they have sequence zero and no test name and use the runtime fallback sink.
The default runtime fallback sink is stderr, not the test's configured sink or
history, whose lifetime is allowed to end with the test.

The value returned by a lazy message factory must remain valid until the
factory call returns and CPPTB emits the record. Returning `std::string` by
value is safe and recommended. A returned `std::string_view` is safe only when
it refers to storage that outlives the logging call; never return a view into a
factory-local string.

## Ordered history

Use `LogHistory` when a harness, report, or test artifact needs to retain the
trace. History is opt-in so ordinary logging does not allocate storage or grow
memory throughout a long simulation:

```cpp
LogHistory history;
history.reserve(1024);  // Optional when the expected volume is known.

RunRequest request{
    .test_name = "packet_test",
    .logging = {
        .minimum_level = LogLevel::Debug,
        .history = &history,
    },
};
run_registered_test(scheduler, dut, result, request);

for (const auto& entry : history) {
    consume_trace_entry(entry.simulation_time_fs, entry.sequence,
                        entry.scope, entry.process_id, entry.message);
}
```

The normal console or custom sink still receives each record. `LogHistory`
stores owned `StoredLogRecord` values in exact emission order, including owned
copies of every text field. Therefore temporary lazy-message strings remain
valid in the history after `emit()` returns. For any adjacent records, the
simulation time is nondecreasing during a normal simulator run; records at the
same time have increasing sequence numbers that preserve scheduler execution
order.

Use `history.records()`, `history[index]`, or range iteration to inspect the
trace. `size()`, `empty()`, `reserve()`, and `clear()` support harness-managed
storage. Call `clear()` between tests when reusing one history; `test_name`
still identifies entries if a harness intentionally accumulates several tests.
The configured history must outlive the running test invocation.

## Custom sinks

Without an explicit sink, enabled records are written to `stderr`. An
embedding harness or verification package can provide a `LogSink` for JSON,
console coloring, transaction databases, or another logging library:

```cpp
class JsonLogSink final : public LogSink {
  public:
    void emit(const LogRecord& record) override {
        write_json(record.level, record.scope, record.message,
                   record.simulation_time_fs, record.process_id);
    }
};

JsonLogSink sink;
RunRequest request{
    .logging = {
        .minimum_level = LogLevel::Debug,
        .sink = &sink,
    },
};
run_registered_test(scheduler, dut, result, request);
```

`LogRecord` uses non-owning string views so an enabled message does not require
framework-owned copies. Those views are valid only during `emit()`. A sink
that queues or retains records must copy the fields it needs. Sink callbacks
are synchronous and run on the simulator thread; expensive file or network
work should be buffered by the sink rather than performed for each message.
The configured sink must outlive the running test invocation. Sinks must not
throw; a thrown sink exception is handled as a test error.
This exception containment also applies when an SV-origin record invokes the
sink through DPI; the resulting failure retains the originating `.sv` file and
line instead of allowing a C++ exception to cross the DPI boundary.
When both a history and output sink are configured, history stores the record
first so the message remains available when the output sink throws.

Verification components can accept a `Logger` by value. The logger retains
the test lifecycle state and owns its scope string, while the harness remains
responsible for sink policy. A retained logger must not outlive the scheduler
and `TestResult` used by the running test.

## Matched performance workload

The `structured_logging` authoring benchmark models sparse logging in a hot
scoreboard loop. Debug formatting is disabled on every iteration, and one
constant info checkpoint is emitted through a counting sink every 1,024
iterations. `structured_log_history` repeats that workload while both C++ and
pure SV retain the complete enabled-record metadata and validate chronological
sequence order. `mixed_logging` adds one sparse C++ checkpoint and one sparse
RTL checkpoint to the normal request/response workload, then compares that
against a pure-SV implementation retaining equivalent metadata.

<div class="cpptb-code-tabs" data-tabs="2" data-tab-group="structured-logging" data-tab-label="Structured logging benchmark"></div>

<div class="cpptb-code-tab-label">cpptb (C++ DPI)</div>

```cpp
auto log = test.logger("scoreboard");
for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
    log.debug([&] {
        ++disabled_factories;
        return "transaction " + std::to_string(iteration);
    });
    if ((iteration & 1023u) == 0)
        log.info("transaction checkpoint");
}
```

<div class="cpptb-code-tab-label">Pure SystemVerilog</div>

```systemverilog
int minimum_log_level = 2;
string disabled_message;
void'($value$plusargs("AUTHORING_CORE_LOG_LEVEL=%d", minimum_log_level));

for (int unsigned i = 0; i < iterations; i++) begin
  if (1 >= minimum_log_level) begin
    disabled_factories++;
    disabled_message = $sformatf("transaction %0d", i);
  end
  if ((i & 1023) == 0 && 2 >= minimum_log_level) begin
    records++;
    attributed_records++;
    complete_records++;
  end
end
```

Run semantic parity and the individually gated benchmark with:

```sh
make feature-test FEATURE=structured_logging
make feature-benchmark FEATURE=structured_logging
make feature-test FEATURE=structured_log_history
make feature-benchmark FEATURE=structured_log_history
make feature-test FEATURE=mixed_logging
make feature-benchmark FEATURE=mixed_logging
```

The exact semantic pairs pass. Formal timing publication remains pending a
host-load window admitted by the standard `1.10x` performance policy.

## Related APIs

- [Framework test lifecycle](test-lifecycle.md) defines result-changing
  warnings, checks, requirements, and process ownership.
- [Scheduling](scheduling.md) explains the simulation time attached to each
  record.
- [Performance](performance.md) documents the matched benchmark methodology
  and host-load admission rules.
