# TestContext and checks

<!-- api-headers: include/cpptb/test_api.hpp include/cpptb/random.hpp -->

The API a test body receives, and the macros that register one. All in
`namespace cpptb`. A test is a coroutine of shape
`Task<void>(Dut dut, TestContext& test)`;
[Test lifecycle](../test-lifecycle.md) covers the model, this page the
surface.

`TestContext` is a copyable, reference-counted handle over the run's
shared state. Pass it **by value** into spawned processes that may outlive
the spawning scope, by reference into ordinary sequential helpers.

## Registration

### CPPTB_REGISTER_TEST
### CPPTB_REGISTER_TEST_WITH_OPTIONS
### CPPTB_REGISTER_TEST_CASE
### CPPTB_REGISTER_TEST_CASE_WITH_OPTIONS

```cpp
CPPTB_REGISTER_TEST(counter_sequence);

CPPTB_REGISTER_TEST_WITH_OPTIONS(
    known_issue,
    (::cpptb::TestOptions{
        .tags = {"nightly"},
        .expected_failure = true,
        .expected_failure_reason = "tracked as RTL-241",
        .simulation_timeout = 20_us,
    }));

CPPTB_REGISTER_TEST_CASE(counter_case, empty, (CounterCase{0, 0}));
```

Place at file scope in the testbench translation unit. The registered name
is the function identifier; a case registers as `function[case]` and its
body takes the case value as a third parameter. Wrap braced arguments in
parentheses so their commas survive the macro. Plain functions only — the
registration slot is a function pointer, so a capturing lambda cannot be
registered directly.

### TestOptions

```cpp
struct TestOptions {
    std::vector<std::string_view> tags;
    bool expected_failure = false;
    std::string_view expected_failure_reason;
    std::string_view skip_reason;
    std::optional<coro::SimTime> simulation_timeout;
};
```

`skip_reason` skips the test at selection without spawning it.
`expected_failure` inverts the verdict: a failing run reports
`ExpectedFailure` (which counts as successful) and a clean run reports
`UnexpectedPass`. `simulation_timeout` is the in-simulation deadline for
this test; there is no macro or environment variable for it.

## Checks

### expect
### expect_eq
### require
### require_eq

```cpp
void expect(std::string_view label, bool condition) const;
void expect_eq(std::string_view label, const Actual& actual,
               const Expected& expected) const;
void require(std::string_view label, bool condition) const;
void require_eq(std::string_view label, const Actual& actual,
                const Expected& expected) const;
```

The complete check family — four calls, label first and mandatory.
`expect*` records a failure and continues, so one run reports every
mismatch. `require*` records and ends the test at once by unwinding to
the framework — which is why a `catch (...)` must never wrap a check.
Every check, passing or failing, counts toward the result's `checks`.

Each failure records the label, the call site, both formatted values (for
`_eq`), the simulation time, and the spawning process's identity — the
same fields the structured JSON carries. Value formatting runs only on
failure, and custom types render by specializing
`cpptb::DiagnosticFormatter<T>`.

### warn

```cpp
void warn(std::string_view message) const;
```

Records a warning with the same location detail as a failure; never
changes the verdict.

### skip

```cpp
[[noreturn]] void skip(std::string_view reason) const;
```

Ends the test immediately with status `Skipped`, which counts as
successful. For skipping at registration instead, use
`TestOptions::skip_reason`.

## Processes and clocks

### spawn
### spawn_detached

```cpp
coro::Process spawn(coro::Task<void> task) const;
void spawn_detached(coro::Task<void> task) const;
```

Start a coroutine as a concurrent, test-owned process. Ownership means
cleanup is automatic: on any terminal outcome — return, fatal check,
timeout, escaping exception — remaining owned processes are cancelled, and
a failure inside one is attributed to its spawn site. `spawn_detached` is
the same ownership without a handle. Spawning after the test has already
finished is an error that cannot leak a process.

### start_clock

```cpp
void start_clock(Signal signal, coro::SimTime period,
                 coro::SimTime phase = {}) const;
```

Register a periodic clock on a writable one-bit port; returns without
suspending. Call before the test's first `co_await`, once per clock; the
first registered clock supplies the result's cycle count. The period must
be even and, like the phase, a whole multiple of the simulator precision.
Initialize the pin with `set_now(0)` first — a plain `set()` queues under
the write model. DUT-*produced* clocks are never started, only awaited.
See [Clocking](../clocking.md).

### now

```cpp
coro::SimTime now() const;
```

Current simulation time.

## Randomization

### random

```cpp
Random& random() const;
```

The calling process's deterministic stream, derived from the master seed
and the process's stable ID — so concurrent processes never steal each
other's draws and any process replays independently.

```cpp
uint64_t next_u64();
Value    randint(Value minimum, Value maximum);   // inclusive; throws if min > max
Bits<W>  randbits<W>();
choice(range);  weighted_choice(range);  shuffle(range);
uint64_t seed() const;  void reseed(uint64_t seed);
```

The master seed defaults to 1 and is set per run with `CPPTB_RANDOM_SEED`
(decimal or `0x`-hex). See
[Reproducibility](../randomization/reproducibility.md).

### randomize
### randomize_with

```cpp
void randomize(Randomized& item) const;
void randomize_with(Randomized& item, Constraint constraint) const;
```

Solve a constrained transaction in place; a failed solve is fatal to the
test. See [Constrained transactions](../randomization/constrained-transactions.md).

## Logging

### logger

```cpp
Logger logger(std::string_view scope = {}) const;
```

A scoped structured logger: per-level calls (`trace` through `error`),
each with an eager string overload and a lazy factory overload that skips
message construction when the level is disabled. Run-time level comes from
`CPPTB_LOG_LEVEL`. See [Logging](../logging.md).

## Environment variables

The complete framework-level set read by a built simulator process:

| Variable | Effect |
|---|---|
| `CPPTB_TEST` | Exact test to run; empty auto-selects only when a single test is registered |
| `CPPTB_LIST_TESTS` | List the catalog (one `CPPTB_TEST <name>` line each) without running |
| `CPPTB_RANDOM_SEED` | Master 64-bit seed, decimal or `0x`-prefixed hex; default 1 |
| `CPPTB_LOG_LEVEL` | `trace`, `debug`, `info`, `warning`, `error`, or `off` |
| `CPPTB_RESULT_FILE` | Path for the structured JSON result |
| `CPPTB_WAVE` | Waveform dump path in a `--wave` build |

An unparsable seed or level does not abort — it surfaces as a selection
error in the structured result.

## See also

- [Tasks and coordination](coordination.md) — the `Process` handle's
  methods and the primitives spawned processes coordinate with.
- [Embedding and results](embedding.md) — running registered tests from
  your own harness, and the JSON these checks feed.
