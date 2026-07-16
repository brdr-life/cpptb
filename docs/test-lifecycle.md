# Framework test lifecycle

The cpptb lifecycle API is part of the reusable C++ framework. It does not
require the `cpptb` command, `cpptb-run`, a particular build system, or a
particular regression harness. An embedding application can discover a
compiled catalog, select one test, run it, and consume its result directly.

The framework owns:

- compiled test registration, metadata, and exact selection;
- one `TestContext` and one `TestResult` for each invocation;
- nonfatal checks, fatal requirements, and structured warnings;
- ownership, provenance, and cleanup of processes started by a test;
- terminal test states and simulation-time timeout policy; and
- optional in-process result callbacks through `ResultSink`.

A harness may build simulator executables, start fresh processes, filter
catalogs, enforce wall-time limits, and translate results into CI formats.
Those policies are deliberately outside the lifecycle API. See
[Running tests](running-tests.md) for the optional reference harness.

## Register and invoke tests

Register each root coroutine in the testbench translation unit:

```cpp
Task<void> reset_defaults(Dut dut, TestContext& test) {
    dut.clk.set(0);
    test.start_clock(dut.clk, 10_ns);

    dut.rst_n.set(0);
    co_await RisingEdge{dut.clk};
    co_await Delay{1_ps};
    test.expect_eq("reset count", dut.count.get(), 0u);
}

CPPTB_REGISTER_TEST(reset_defaults);
```

The framework exposes `registered_tests<Dut>()` for catalog discovery and
`run_registered_test(...)` for exact selection. Each invocation receives
fresh lifecycle state; checks, failures, warnings, and owned processes are
never shared between results.

## Checks and diagnostics

Checks always carry a concise user label:

```cpp
test.expect("response is in range", response >= low && response <= high);
test.expect_eq("response payload", response, expected);

test.require("configuration loaded", configured);
test.require_eq("protocol version", version, kSupportedVersion);
```

`expect()` and `expect_eq()` are nonfatal. Independent failures accumulate in
the current result. `require()` and `require_eq()` record a failure and stop
the owning test cleanly, allowing structured reporting to complete.

Equality checks retain formatted actual and expected values. Formatting is
only invoked after `actual == expected` fails, so passing checks do not build
diagnostic strings.

| Value type | Failure representation |
|---|---|
| Boolean | `true` or `false` |
| Signed and unsigned integers | Decimal value |
| Floating point | Round-trip decimal value |
| C++ enums | Symbolic name when supplied, otherwise the underlying value |
| Strings and string-like values | Text value |
| `Bits<W>` | Width-qualified hexadecimal, such as `12'habc` |
| `LogicBits<W>` | Width-qualified binary preserving `X` and `Z` |
| Arrays and input ranges | Recursive list, such as `[1, 2, 3]` |
| Generated packed views | Width-qualified raw packed value |

Generated HDL enums provide their symbolic names automatically. A user type
can specialize `DiagnosticFormatter<T>` without modifying the check API:

```cpp
template <>
struct cpptb::DiagnosticFormatter<Transaction> {
    static std::string format(const Transaction& value) {
        return "Transaction{address=" + std::to_string(value.address) +
               ", data=" + std::to_string(value.data) + "}";
    }
};
```

## Warnings and process ownership

`warn()` records a structured warning without failing the test:

```cpp
test.warn("response used the protocol fallback");
```

Start lifecycle-owned concurrency through the context:

```cpp
Task<void> protocol_watcher(Dut dut, TestContext test) {
    while (true) {
        co_await RisingEdge{dut.response_valid};
        co_await ReadOnly{};
        test.expect_eq("response reserved bits",
                       dut.response_data.get() & 0xff00'0000u, 0u);
    }
}

Task<void> traffic_test(Dut dut, TestContext& test) {
    test.spawn_detached(protocol_watcher(dut, test));

    auto driver = test.spawn(input_driver(dut));
    co_await driver;
    test.require_eq("driver completed", driver.done(), true);
}

CPPTB_REGISTER_TEST(traffic_test);
```

Both forms attach the child to the current test. A detached process is
detached only from a user-visible `Process` handle; it remains owned by the
test. Normal completion, a fatal requirement, a timeout, or an uncaught child
exception cancels unfinished owned processes.

The watcher takes `TestContext` by value because it may remain alive until the
root test completes. Its checks and uncaught exceptions still carry the
watcher's process ID and spawn location. The finite driver retains a handle so
the foreground sequence can await it and inspect its terminal state.

Use `spawn()` when work must run concurrently, needs an independent process
identity, or may be cancelled through a handle. A sequential helper can be
awaited directly with `co_await helper(...)`; this avoids creating an
independent process while retaining ordinary coroutine composition.

Every root and spawned process receives a stable invocation-local numeric ID.
Failures, warnings, and exceptions retain that ID, the process description,
and the source location of the `spawn()` call. An uncaught child exception is
therefore attributed to its owning test and process instead of being lost or
surfacing later as a generic timeout.

## Lifecycle outcomes

The terminal states have explicit meanings:

| Status | Meaning | Successful result? |
|---|---|---:|
| `passed` | Body completed with no failures | Yes |
| `failed` | An expectation or requirement failed | No |
| `error` | Selection failed or an exception escaped | No |
| `skipped` | Metadata or the running test requested a skip | Yes |
| `expected_failure` | A test marked expected-to-fail failed normally | Yes |
| `unexpected_pass` | A test marked expected-to-fail passed | No |
| `timed_out` | The simulation-time limit expired | No |

`running` and `not_run` are nonterminal states used during selection and
execution. Exceptions and timeouts are never converted into expected
failures.

A test can skip dynamically:

```cpp
if (!dut.has_optional_feature.get()) {
    test.skip("optional feature is not present");
}
```

Static policy belongs in registration metadata:

```cpp
CPPTB_REGISTER_TEST_WITH_OPTIONS(
    known_issue,
    (::cpptb::TestOptions{
        .tags = {"nightly", "known-issue"},
        .expected_failure = true,
        .expected_failure_reason = "tracked as RTL-241",
        .simulation_timeout = 20_us,
    }));
```

The timeout is measured in simulator time and cancels the test body cleanly.
A harness-level wall-time timeout is a separate safeguard for a stalled
simulator process.

## Parameterized cases

Parameterized cases retain one typed C++ value per descriptor. Stable catalog
names use `test_name[case_name]`, and an embedding harness can inspect the
case name and tags without knowing how the value is represented:

```cpp
struct CounterCase {
    uint32_t pulses;
    uint32_t expected;
};

Task<void> counter_case(Dut dut, TestContext& test,
                        const CounterCase& parameter) {
    for (uint32_t i = 0; i < parameter.pulses; ++i) {
        co_await RisingEdge{dut.clk};
    }
    test.expect_eq("count", dut.count.get(), parameter.expected);
}

CPPTB_REGISTER_TEST_CASE(counter_case, empty, (CounterCase{0, 0}));
CPPTB_REGISTER_TEST_CASE_WITH_OPTIONS(
    counter_case, wraps, (CounterCase{256, 0}),
    (::cpptb::TestOptions{.tags = {"nightly", "wrap"}}));
```

The catalog contains `counter_case[empty]` and `counter_case[wraps]` as
independently selectable tests.

## Structured results

`TestResult` is the harness-neutral result model. It records:

- test and case names, tags, terminal status, and status reason;
- check, failure, and warning counts;
- final simulation and wall time;
- structured failure and warning records; and
- process identity, spawn location, check location, and comparison values.

`ResultSink` provides optional `test_started`, `failure_recorded`,
`warning_recorded`, and `test_finished` callbacks. The versioned JSON
serializer is another adapter over the same result model. Neither requires
the public command-line harness.

CLI tag filtering, process launch policy, JUnit XML, waveform-on-failure
reruns, wall-time enforcement, reproduction command rendering, and build
diagnostic presentation remain harness concerns rather than framework
lifecycle behavior.
