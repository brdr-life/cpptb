# Embedding and results

<!-- api-headers: include/cpptb/test_api.hpp include/cpptb/test_result.hpp include/cpptb/test_reporting.hpp -->

The contract for running registered tests from your own harness instead of
the `cpptb` launcher, and the result structures every run produces. All in
`namespace cpptb`. [Architecture](../architecture.md) explains where this
boundary sits; nothing here is needed when the `cpptb` command runs your
tests.

## Catalog and launch

### registered_tests

```cpp
template <typename Dut>
std::span<const detail::TestDescriptor<Dut>> registered_tests();
```

The compiled catalog. Each descriptor carries the registered `name`, the
invoker, a `TestMetadata` (base name, case name, tags, expected-failure
and skip reasons, per-test simulation timeout), and the declaration
location — enough to filter by tag or case without knowing parameter
types.

### run_registered_test

```cpp
template <typename Dut>
bool run_registered_test(coro::Testbench& scheduler, Dut dut,
                         TestResult& result, RunRequest request,
                         coro::ClockRegistrar clocks = {},
                         ResultSink* sink = nullptr);

// Environment-driven form: builds the RunRequest from CPPTB_* variables.
template <typename Dut>
bool run_registered_test(coro::Testbench& scheduler, Dut dut,
                         TestResult& result,
                         coro::ClockRegistrar clocks = {});
```

Selects one test and spawns it as the scheduler's root process. **The
return value is selection success, not test success**: after `true`, the
harness drives the scheduler to completion and then reads
`result.status`. Selection failures (unknown name, ambiguous empty name,
bad seed) return with a recorded `Selection` failure. A descriptor whose
options carry a `skip_reason` returns `true` without spawning anything.

### RunRequest

```cpp
struct RunRequest {
    std::string_view test_name;      // empty auto-selects a single-test catalog
    bool list_only = false;          // print the catalog, run nothing
    std::optional<coro::SimTime> simulation_timeout;  // overrides the descriptor's
    std::optional<uint64_t> random_seed;              // default 1
    std::string_view configuration_error;  // surface an env-parse error as Selection
    LoggingOptions logging;
};
```

## Observing a run

### ResultSink

```cpp
class ResultSink {
   public:
    virtual void test_started(const TestResult&) {}
    virtual void failure_recorded(const FailureRecord&) {}
    virtual void warning_recorded(const WarningRecord&) {}
    virtual void test_finished(const TestResult&) {}
};
```

All callbacks default to no-ops — override what you need. Two contract
points: references are valid only for the duration of the call, and a
selection error fires `failure_recorded` + `test_finished` **without** a
preceding `test_started`, so do not assume pairing.

## Result structures

### TestResult

```cpp
struct TestResult {
    TestStatus status;               // NotRun … TimedOut
    uint64_t checks;  uint32_t failures;  uint32_t warnings;
    std::string test_name, case_name, status_reason;
    std::vector<std::string> tags;
    std::vector<FailureRecord> failure_records;
    std::vector<WarningRecord> warning_records;
    std::optional<WaitGraphSnapshot> wait_graph;   // captured on timeout
    std::optional<uint64_t> random_seed;
    std::string random_algorithm;
    std::string constraint_backend, constraint_backend_version;
    uint64_t random_sampling_solves, random_solver_solves;
    uint64_t simulation_time_fs;
    uint64_t wall_time_ns;           // the HARNESS fills this in
    bool finished;
};
```

`wall_time_ns` is deliberately left to the embedding harness — the
framework cannot know what wall interval the harness considers the run.

### TestStatus

```cpp
enum class TestStatus : uint8_t { NotRun, Running, Passed, Failed, Error,
                                  Skipped, ExpectedFailure, UnexpectedPass,
                                  TimedOut };

constexpr bool test_status_successful(TestStatus status);
// true for Passed, Skipped, and ExpectedFailure — the exit-code predicate
```

### FailureRecord

```cpp
struct FailureRecord {
    FailureKind kind;      // Expectation, Requirement, Exception,
                           // Selection, Timeout, UnexpectedPass
    std::string label, actual, expected;
    std::string source_file;  uint32_t source_line;
    uint64_t simulation_time_fs;
    std::string process;  uint64_t process_id;
    std::string process_source_file;  uint32_t process_source_line;
    bool has_comparison;   // actual/expected are meaningful only when true
};
```

`WarningRecord` has the same shape without `kind` and the comparison
fields.

## The JSON contract

### write_test_result_json

```cpp
bool write_test_result_json(const char* path, const TestResult& result);
```

Writes the versioned structured result — the file a CI system consumes,
`schema_version` 5, with earlier versions still readable by the reference
runner. A null or empty `path` returns `true` and writes nothing: an
unset result path is not an error. The top-level keys mirror `TestResult`
field for field; each failure record carries its kind, label, values,
both source locations, and simulation time; a timed-out run's
`wait_graph` serializes every parked process with what it waits on —
`WaitGraphSnapshot::deadlocked()` and `format_wait_graph(...)` interpret
the same data in process.

## The minimal harness

```cpp
coro::Testbench scheduler;
TestResult result;
RunRequest request{.test_name = "counter_sequence"};

if (run_registered_test(scheduler, dut, result, request)) {
    // drive the scheduler to completion (simulator loop)
    result.wall_time_ns = measured_wall_ns;
    write_test_result_json(result_path, result);
}
return test_status_successful(result.status) ? 0 : 1;
```

## See also

- [Running tests](../running-tests.md#lower-level-runner-protocol) — the
  `cpptb-run` executable protocol built on this API.
- [Architecture](../architecture.md) — the ownership and threading rules a
  harness must respect.
