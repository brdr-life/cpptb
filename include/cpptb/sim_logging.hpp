// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cpptb/logging.hpp"

namespace cpptb::detail {

struct SimLogMessage {
    LogLevel level = LogLevel::Info;
    std::string_view message;
    std::string_view scope;
    std::string_view source_file;
    std::string_view hierarchy;
    uint64_t simulation_time_fs = 0;
    uint32_t source_line = 0;
};

struct StoredSimLogMessage {
    LogLevel level = LogLevel::Info;
    std::string message;
    std::string scope;
    std::string source_file;
    std::string hierarchy;
    uint64_t simulation_time_fs = 0;
    uint32_t source_line = 0;

    SimLogMessage view() const {
        return {
            .level = level,
            .message = message,
            .scope = scope,
            .source_file = source_file,
            .hierarchy = hierarchy,
            .simulation_time_fs = simulation_time_fs,
            .source_line = source_line,
        };
    }
};

class SimLogEndpoint {
   public:
    using EmitFunction = void (*)(void*, const SimLogMessage&);

    explicit SimLogEndpoint(LogSink* unowned_sink = nullptr)
        : unowned_sink_(unowned_sink ? unowned_sink : &default_log_sink()) {}

    void prepare_for_run() {
        if (state_ != State::Collecting) pending_.clear();
        owner_ = nullptr;
        emit_ = nullptr;
        minimum_level_ = LogLevel::Trace;
        state_ = State::Collecting;
    }

    void bind(void* owner, EmitFunction emit, LogLevel minimum_level) {
        owner_ = owner;
        emit_ = emit;
        minimum_level_ = minimum_level;
        state_ = State::Bound;

        auto pending = std::move(pending_);
        pending_.clear();
        for (const auto& record : pending) emit_record(record.view());
    }

    void finish_unowned(LogLevel minimum_level) {
        owner_ = nullptr;
        emit_ = nullptr;
        minimum_level_ = minimum_level;
        state_ = State::Finished;

        auto pending = std::move(pending_);
        pending_.clear();
        for (const auto& record : pending) emit_unowned(record.view());
    }

    void unbind(void* owner, bool finished) {
        if (owner_ != owner) return;
        owner_ = nullptr;
        emit_ = nullptr;
        state_ = finished ? State::Finished : State::Collecting;
    }

    void emit(const SimLogMessage& record) {
        if (state_ == State::Bound) {
            emit_record(record);
            return;
        }
        if (state_ == State::Collecting) {
            pending_.push_back({
                .level = record.level,
                .message = std::string{record.message},
                .scope = std::string{record.scope},
                .source_file = std::string{record.source_file},
                .hierarchy = std::string{record.hierarchy},
                .simulation_time_fs = record.simulation_time_fs,
                .source_line = record.source_line,
            });
            return;
        }
        emit_unowned(record);
    }

    LogLevel minimum_level() const noexcept {
        return state_ == State::Collecting ? LogLevel::Trace : minimum_level_;
    }

    size_t pending_size() const noexcept { return pending_.size(); }

   private:
    enum class State : uint8_t {
        Collecting,
        Bound,
        Finished,
    };

    void emit_record(const SimLogMessage& record) {
        if (emit_) emit_(owner_, record);
    }

    void emit_unowned(const SimLogMessage& record) {
        if (!log_level_enabled(record.level, minimum_level_)) return;
        unowned_sink_->emit({
            .level = record.level,
            .origin = LogOrigin::SystemVerilog,
            .message = record.message,
            .scope = record.scope,
            .source_file = record.source_file,
            .hierarchy = record.hierarchy,
            .simulation_time_fs = record.simulation_time_fs,
            .source_line = record.source_line,
        });
    }

    void* owner_ = nullptr;
    EmitFunction emit_ = nullptr;
    LogSink* unowned_sink_ = nullptr;
    LogLevel minimum_level_ = LogLevel::Trace;
    State state_ = State::Collecting;
    std::vector<StoredSimLogMessage> pending_;
};

struct SimLogRuntimeEntry {
    void* owner = nullptr;
    SimLogEndpoint* endpoint = nullptr;
    std::string hierarchy;
};

inline std::vector<SimLogRuntimeEntry>& sim_log_runtimes() {
    static std::vector<SimLogRuntimeEntry> runtimes;
    return runtimes;
}

inline void register_sim_log_runtime(void* owner, SimLogEndpoint& endpoint) {
    auto& runtimes = sim_log_runtimes();
    const auto existing = std::find_if(
        runtimes.begin(), runtimes.end(),
        [owner](const auto& runtime) { return runtime.owner == owner; });
    if (existing != runtimes.end()) {
        existing->endpoint = &endpoint;
        return;
    }
    runtimes.push_back({.owner = owner, .endpoint = &endpoint});
}

inline void update_sim_log_runtime_hierarchy(void* owner,
                                             std::string_view hierarchy) {
    for (auto& runtime : sim_log_runtimes()) {
        if (runtime.owner != owner) continue;
        runtime.hierarchy.assign(hierarchy);
        return;
    }
}

inline void unregister_sim_log_runtime(void* owner) {
    auto& runtimes = sim_log_runtimes();
    runtimes.erase(
        std::remove_if(runtimes.begin(), runtimes.end(),
                       [owner](const auto& runtime) {
                           return runtime.owner == owner;
                       }),
        runtimes.end());
}

inline bool hierarchy_contains(std::string_view hierarchy,
                               std::string_view root) {
    if (root.empty() || hierarchy.size() < root.size() ||
        hierarchy.substr(0, root.size()) != root) {
        return false;
    }
    return hierarchy.size() == root.size() || hierarchy[root.size()] == '.';
}

inline SimLogEndpoint* find_sim_log_endpoint(std::string_view hierarchy) {
    auto& runtimes = sim_log_runtimes();
    SimLogRuntimeEntry* best = nullptr;
    for (auto& runtime : runtimes) {
        if (!hierarchy_contains(hierarchy, runtime.hierarchy)) continue;
        if (!best || runtime.hierarchy.size() > best->hierarchy.size()) {
            best = &runtime;
        }
    }
    if (best) return best->endpoint;
    return runtimes.size() == 1 ? runtimes.front().endpoint : nullptr;
}

inline void emit_sim_log(const SimLogMessage& record) {
    if (auto* endpoint = find_sim_log_endpoint(record.hierarchy)) {
        endpoint->emit(record);
        return;
    }
    if (!log_level_enabled(record.level, LogLevel::Info)) return;
    default_log_sink().emit({
        .level = record.level,
        .origin = LogOrigin::SystemVerilog,
        .message = record.message,
        .scope = record.scope,
        .source_file = record.source_file,
        .hierarchy = record.hierarchy,
        .simulation_time_fs = record.simulation_time_fs,
        .source_line = record.source_line,
    });
}

inline LogLevel sim_log_minimum_level() {
    const auto& runtimes = sim_log_runtimes();
    if (runtimes.empty()) return LogLevel::Info;
    LogLevel minimum = LogLevel::Off;
    for (const auto& runtime : runtimes) {
        minimum = static_cast<LogLevel>(std::min(
            static_cast<uint8_t>(minimum),
            static_cast<uint8_t>(runtime.endpoint->minimum_level())));
    }
    return minimum;
}

}  // namespace cpptb::detail
