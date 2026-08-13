# Functional coverage

<!-- api-headers: include/cpptb/coverage.hpp -->

The language-native covergroup API. All in `namespace cpptb`.
[Functional coverage](../randomization/functional-coverage.md) is the
guide; this page is the surface. Sampling updates in-process counters
only — no simulator interaction, no time.

## Building the model

### Covergroup

```cpp
template <typename Sample> class Covergroup {
    explicit Covergroup(std::string name);

    auto& coverpoint(std::string name, Extractor extractor);
    CrossRef cross(std::string name, Coverpoint<Values>&... points);  // two or more

    CoverageSampleResult sample(const Sample& value);
    CoverageSnapshot snapshot() const;
};
```

`Extractor` is a member pointer, lambda, or any callable returning an
integral or enum. The model freezes at the first `sample()` — adding
points or crosses afterwards throws, as do duplicate names. Construction
may allocate; repeated `sample()` does not.

### Coverpoint

```cpp
Coverpoint& bin(std::string name, Value value);
Coverpoint& bin(std::string name, Value minimum, Value maximum);
Coverpoint& bin(std::string name, std::initializer_list<Range> ranges);
Coverpoint& bin_array(const std::string& name, Value min, Value max);  // name[i] per value
Coverpoint& ignore_bin(...);            // same three shapes
Coverpoint& illegal_bin(...);           // same three shapes
Coverpoint& transition_bin(std::string name, Value from, Value to);
Coverpoint& wildcard_bin(std::string name, std::string_view pattern);  // "1?????"
Coverpoint& iff(std::function<bool()> guard);   // rejected sample is not a sample
```

Bin precedence matches SystemVerilog: illegal and ignore bins win over
ordinary and transition bins; overlapping ordinary bins all count. An
illegal hit is **data** — reported through the sample result, never an
implicit test failure.

### binsof

```cpp
cross.illegal("m_write", binsof(mode, {Mode::M}) && binsof(op, {Op::Write}));
cross.ignore("idle", !binsof(active));
```

Cross filters compose with `&&`, `||`, `!` — the `binsof ... intersect`
transcription. `where(text, ...)` names an arbitrary predicate filter.

## Results and reporting

### CoverageSampleResult

```cpp
struct CoverageSampleResult {
    uint32_t illegal_hits;
    bool legal() const;               // illegal_hits == 0
    explicit operator bool() const;
};
```

What `sample()` returns — check it where an illegal bin should fail the
test, count it where it should not.

### CoverageSnapshot

```cpp
struct CoverageSnapshot {
    uint64_t samples, illegal_hits;
    uint64_t coverable_bins() const;
    uint64_t covered_bins() const;
    double coverage_percent() const;      // 100.0 with nothing coverable
    void merge(const CoverageSnapshot& other);  // throws on a different model
};
```

`merge()` requires identical group, point, bin, and cross structure — the
cross-run accumulation primitive.

### write_coverage_json

```cpp
bool write_coverage_json(const char* path, const CoverageSnapshot& coverage);
```

The schema-1 structured coverage report.

## See also

- [Functional coverage guide](../randomization/functional-coverage.md) —
  bin behavior in detail, sampling from monitors, and reporting.
- [Randomization](randomization.md) — the stimulus side of closing
  coverage.
