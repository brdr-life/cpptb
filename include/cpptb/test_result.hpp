// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "cpptb/wait_graph.hpp"

namespace cpptb {

enum class TestStatus : uint8_t {
    NotRun,
    Running,
    Passed,
    Failed,
    Error,
    Skipped,
    ExpectedFailure,
    UnexpectedPass,
    TimedOut,
};

enum class FailureKind : uint8_t {
    Expectation,
    Requirement,
    Exception,
    Selection,
    Timeout,
    UnexpectedPass,
};

inline constexpr std::string_view test_status_name(TestStatus status) {
    switch (status) {
        case TestStatus::NotRun:
            return "not_run";
        case TestStatus::Running:
            return "running";
        case TestStatus::Passed:
            return "passed";
        case TestStatus::Failed:
            return "failed";
        case TestStatus::Error:
            return "error";
        case TestStatus::Skipped:
            return "skipped";
        case TestStatus::ExpectedFailure:
            return "expected_failure";
        case TestStatus::UnexpectedPass:
            return "unexpected_pass";
        case TestStatus::TimedOut:
            return "timed_out";
    }
    return "error";
}

inline constexpr std::string_view failure_kind_name(FailureKind kind) {
    switch (kind) {
        case FailureKind::Expectation:
            return "expectation";
        case FailureKind::Requirement:
            return "requirement";
        case FailureKind::Exception:
            return "exception";
        case FailureKind::Selection:
            return "selection";
        case FailureKind::Timeout:
            return "timeout";
        case FailureKind::UnexpectedPass:
            return "unexpected_pass";
    }
    return "exception";
}

inline constexpr bool test_status_successful(TestStatus status) {
    return status == TestStatus::Passed || status == TestStatus::Skipped ||
           status == TestStatus::ExpectedFailure;
}

struct FailureRecord {
    FailureKind kind = FailureKind::Expectation;
    std::string label;
    std::string actual;
    std::string expected;
    std::string source_file;
    std::string process;
    std::string process_source_file;
    uint64_t simulation_time_fs = 0;
    uint64_t process_id = 0;
    uint32_t source_line = 0;
    uint32_t process_source_line = 0;
    bool has_comparison = false;
};

struct WarningRecord {
    std::string label;
    std::string source_file;
    std::string process;
    std::string process_source_file;
    uint64_t simulation_time_fs = 0;
    uint64_t process_id = 0;
    uint32_t source_line = 0;
    uint32_t process_source_line = 0;
};

struct TestResult {
    uint64_t checks = 0;
    uint32_t failures = 0;
    uint32_t warnings = 0;
    TestStatus status = TestStatus::NotRun;
    std::string test_name;
    std::string case_name;
    std::string status_reason;
    std::vector<std::string> tags;
    std::vector<FailureRecord> failure_records;
    std::vector<WarningRecord> warning_records;
    std::optional<WaitGraphSnapshot> wait_graph;
    std::optional<uint64_t> random_seed;
    std::string random_algorithm;
    std::string constraint_backend;
    std::string constraint_backend_version;
    uint64_t random_sampling_solves = 0;
    uint64_t random_solver_solves = 0;
    uint64_t simulation_time_fs = 0;
    uint64_t wall_time_ns = 0;
    bool finished = false;
};

class ResultSink {
   public:
    virtual ~ResultSink() = default;
    virtual void test_started(const TestResult&) {}
    virtual void failure_recorded(const FailureRecord&) {}
    virtual void warning_recorded(const WarningRecord&) {}
    virtual void test_finished(const TestResult&) {}
};

}  // namespace cpptb
