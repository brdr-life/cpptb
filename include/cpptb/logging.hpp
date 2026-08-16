// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cpptb {

enum class LogLevel : uint8_t {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Off,
};

enum class LogOrigin : uint8_t {
    Cpp,
    SystemVerilog,
};

inline constexpr std::string_view log_origin_name(LogOrigin origin) {
    switch (origin) {
        case LogOrigin::Cpp:
            return "cpp";
        case LogOrigin::SystemVerilog:
            return "systemverilog";
    }
    return "cpp";
}

inline constexpr std::string_view log_level_name(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
            return "trace";
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warning:
            return "warning";
        case LogLevel::Error:
            return "error";
        case LogLevel::Off:
            return "off";
    }
    return "off";
}

inline constexpr std::optional<LogLevel> parse_log_level(
    std::string_view name) {
    if (name == "trace") return LogLevel::Trace;
    if (name == "debug") return LogLevel::Debug;
    if (name == "info") return LogLevel::Info;
    if (name == "warning" || name == "warn") return LogLevel::Warning;
    if (name == "error") return LogLevel::Error;
    if (name == "off") return LogLevel::Off;
    return std::nullopt;
}

inline constexpr bool log_level_enabled(LogLevel level,
                                        LogLevel minimum_level) {
    return minimum_level != LogLevel::Off && level != LogLevel::Off &&
           static_cast<uint8_t>(level) >=
               static_cast<uint8_t>(minimum_level);
}

// String views in a LogRecord remain valid only for the duration of emit().
// Sinks that retain records must copy the fields they need.
struct LogRecord {
    LogLevel level = LogLevel::Info;
    LogOrigin origin = LogOrigin::Cpp;
    std::string_view message;
    std::string_view scope;
    std::string_view test_name;
    std::string_view source_file;
    std::string_view hierarchy;
    std::string_view process;
    std::string_view process_source_file;
    uint64_t sequence = 0;
    uint64_t simulation_time_fs = 0;
    uint64_t process_id = 0;
    uint32_t source_line = 0;
    uint32_t process_source_line = 0;
};

class LogSink {
   public:
    virtual ~LogSink() = default;
    virtual void emit(const LogRecord& record) = 0;
};

struct StoredLogRecord {
    LogLevel level = LogLevel::Info;
    LogOrigin origin = LogOrigin::Cpp;
    std::string message;
    std::string scope;
    std::string test_name;
    std::string source_file;
    std::string hierarchy;
    std::string process;
    std::string process_source_file;
    uint64_t sequence = 0;
    uint64_t simulation_time_fs = 0;
    uint64_t process_id = 0;
    uint32_t source_line = 0;
    uint32_t process_source_line = 0;
};

class LogHistory final : public LogSink {
   public:
    using const_iterator = std::vector<StoredLogRecord>::const_iterator;

    void emit(const LogRecord& record) override {
        records_.push_back({
            .level = record.level,
            .origin = record.origin,
            .message = std::string{record.message},
            .scope = std::string{record.scope},
            .test_name = std::string{record.test_name},
            .source_file = std::string{record.source_file},
            .hierarchy = std::string{record.hierarchy},
            .process = std::string{record.process},
            .process_source_file = std::string{record.process_source_file},
            .sequence = record.sequence,
            .simulation_time_fs = record.simulation_time_fs,
            .process_id = record.process_id,
            .source_line = record.source_line,
            .process_source_line = record.process_source_line,
        });
    }

    const std::vector<StoredLogRecord>& records() const noexcept {
        return records_;
    }
    const StoredLogRecord& operator[](std::size_t index) const {
        return records_[index];
    }
    const_iterator begin() const noexcept { return records_.begin(); }
    const_iterator end() const noexcept { return records_.end(); }
    std::size_t size() const noexcept { return records_.size(); }
    bool empty() const noexcept { return records_.empty(); }
    void reserve(std::size_t capacity) { records_.reserve(capacity); }
    void clear() noexcept { records_.clear(); }

   private:
    std::vector<StoredLogRecord> records_;
};

class StderrLogSink final : public LogSink {
   public:
    StderrLogSink() = default;
    explicit StderrLogSink(FILE* stream) : stream_(stream ? stream : stderr) {}

    void emit(const LogRecord& record) override {
        if (!record.source_file.empty()) {
            std::fprintf(stream_, "cpptb: %.*s:%u: ",
                         static_cast<int>(record.source_file.size()),
                         record.source_file.data(), record.source_line);
        } else {
            std::fputs("cpptb: ", stream_);
        }
        std::fprintf(stream_, "%llu fs",
                     static_cast<unsigned long long>(record.simulation_time_fs));
        if (record.sequence != 0) {
            std::fprintf(stream_, " #%llu",
                         static_cast<unsigned long long>(record.sequence));
        }
        std::fprintf(stream_, ": %.*s",
                     static_cast<int>(log_level_name(record.level).size()),
                     log_level_name(record.level).data());
        if (!record.scope.empty()) {
            std::fprintf(stream_, " [%.*s]",
                         static_cast<int>(record.scope.size()),
                         record.scope.data());
        }
        if (record.origin == LogOrigin::SystemVerilog) {
            std::fputs(" [sv", stream_);
            if (!record.hierarchy.empty()) {
                std::fprintf(stream_, " %.*s",
                             static_cast<int>(record.hierarchy.size()),
                             record.hierarchy.data());
            }
            std::fputc(']', stream_);
        }
        if (record.process_id != 0) {
            std::fprintf(stream_, " [process %llu: %.*s]",
                         static_cast<unsigned long long>(record.process_id),
                         static_cast<int>(record.process.size()),
                         view_data(record.process));
        }
        std::fprintf(stream_, ": %.*s\n",
                     static_cast<int>(record.message.size()),
                     view_data(record.message));
    }

   private:
    static const char* view_data(std::string_view value) {
        return value.empty() ? "" : value.data();
    }

    FILE* stream_ = stderr;
};

inline LogSink& default_log_sink() {
    static StderrLogSink sink;
    return sink;
}

struct LoggingOptions {
    LogLevel minimum_level = LogLevel::Info;
    LogSink* sink = nullptr;
    LogHistory* history = nullptr;
};

}  // namespace cpptb
