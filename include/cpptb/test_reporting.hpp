#pragma once

#include <cstddef>
#include <cstdio>
#include <string_view>

#include "cpptb/test_result.hpp"

namespace cpptb {

namespace detail {

inline void write_json_string(FILE* stream, std::string_view value) {
    std::fputc('"', stream);
    for (const unsigned char character : value) {
        switch (character) {
            case '"':
                std::fputs("\\\"", stream);
                break;
            case '\\':
                std::fputs("\\\\", stream);
                break;
            case '\b':
                std::fputs("\\b", stream);
                break;
            case '\f':
                std::fputs("\\f", stream);
                break;
            case '\n':
                std::fputs("\\n", stream);
                break;
            case '\r':
                std::fputs("\\r", stream);
                break;
            case '\t':
                std::fputs("\\t", stream);
                break;
            default:
                if (character < 0x20) {
                    std::fprintf(stream, "\\u%04x", character);
                } else {
                    std::fputc(character, stream);
                }
                break;
        }
    }
    std::fputc('"', stream);
}

inline void write_json_field(FILE* stream, std::string_view name,
                             std::string_view value) {
    write_json_string(stream, name);
    std::fputc(':', stream);
    write_json_string(stream, value);
}

inline void write_json_string_array(FILE* stream,
                                    const std::vector<std::string>& values) {
    std::fputc('[', stream);
    for (size_t index = 0; index < values.size(); ++index) {
        if (index != 0) std::fputc(',', stream);
        write_json_string(stream, values[index]);
    }
    std::fputc(']', stream);
}

}  // namespace detail

inline bool write_test_result_json(const char* path,
                                   const TestResult& result) {
    if (!path || path[0] == '\0') return true;
    FILE* stream = std::fopen(path, "w");
    if (!stream) return false;

    std::fputs("{\n  \"schema_version\":5,\n  ", stream);
    detail::write_json_field(stream, "test_name", result.test_name);
    std::fputs(",\n  ", stream);
    detail::write_json_field(stream, "case_name", result.case_name);
    std::fputs(",\n  ", stream);
    detail::write_json_field(stream, "status",
                             test_status_name(result.status));
    std::fputs(",\n  ", stream);
    detail::write_json_field(stream, "status_reason", result.status_reason);
    std::fputs(",\n  \"tags\":", stream);
    detail::write_json_string_array(stream, result.tags);
    std::fputs(",\n  \"random_seed\":", stream);
    if (result.random_seed) {
        std::fprintf(stream, "%llu",
                     static_cast<unsigned long long>(*result.random_seed));
    } else {
        std::fputs("null", stream);
    }
    std::fputs(",\n  ", stream);
    detail::write_json_field(stream, "random_algorithm",
                             result.random_algorithm);
    std::fputs(",\n  ", stream);
    detail::write_json_field(stream, "constraint_backend",
                             result.constraint_backend);
    std::fputs(",\n  ", stream);
    detail::write_json_field(stream, "constraint_backend_version",
                             result.constraint_backend_version);
    std::fprintf(stream,
                 ",\n  \"random_sampling_solves\":%llu,\n"
                 "  \"random_solver_solves\":%llu,\n"
                 "  \"checks\":%llu,\n  \"failures\":%u,\n"
                 "  \"warnings\":%u,\n  \"simulation_time_fs\":%llu,\n"
                 "  \"wall_time_ns\":%llu,\n  \"failure_records\":[",
                 static_cast<unsigned long long>(
                     result.random_sampling_solves),
                 static_cast<unsigned long long>(result.random_solver_solves),
                 static_cast<unsigned long long>(result.checks),
                 result.failures, result.warnings,
                 static_cast<unsigned long long>(result.simulation_time_fs),
                 static_cast<unsigned long long>(result.wall_time_ns));

    for (size_t index = 0; index < result.failure_records.size(); ++index) {
        const auto& failure = result.failure_records[index];
        std::fputs(index == 0 ? "\n    {" : ",\n    {", stream);
        detail::write_json_field(stream, "kind",
                                 failure_kind_name(failure.kind));
        std::fputs(",", stream);
        detail::write_json_field(stream, "label", failure.label);
        std::fputs(",", stream);
        detail::write_json_field(stream, "actual", failure.actual);
        std::fputs(",", stream);
        detail::write_json_field(stream, "expected", failure.expected);
        std::fputs(",", stream);
        detail::write_json_field(stream, "source_file", failure.source_file);
        std::fputs(",", stream);
        detail::write_json_field(stream, "process", failure.process);
        std::fputs(",", stream);
        detail::write_json_field(stream, "process_source_file",
                                 failure.process_source_file);
        std::fprintf(stream,
                     ",\"source_line\":%u,\"simulation_time_fs\":%llu,"
                     "\"process_id\":%llu,\"process_source_line\":%u,"
                     "\"has_comparison\":%s}",
                     failure.source_line,
                     static_cast<unsigned long long>(
                         failure.simulation_time_fs),
                     static_cast<unsigned long long>(failure.process_id),
                     failure.process_source_line,
                     failure.has_comparison ? "true" : "false");
    }
    if (!result.failure_records.empty()) std::fputc('\n', stream);
    std::fputs("  ],\n  \"warning_records\":[", stream);
    for (size_t index = 0; index < result.warning_records.size(); ++index) {
        const auto& warning = result.warning_records[index];
        std::fputs(index == 0 ? "\n    {" : ",\n    {", stream);
        detail::write_json_field(stream, "label", warning.label);
        std::fputs(",", stream);
        detail::write_json_field(stream, "source_file", warning.source_file);
        std::fputs(",", stream);
        detail::write_json_field(stream, "process", warning.process);
        std::fputs(",", stream);
        detail::write_json_field(stream, "process_source_file",
                                 warning.process_source_file);
        std::fprintf(stream,
                     ",\"source_line\":%u,\"simulation_time_fs\":%llu,"
                     "\"process_id\":%llu,\"process_source_line\":%u}",
                     warning.source_line,
                     static_cast<unsigned long long>(
                         warning.simulation_time_fs),
                     static_cast<unsigned long long>(warning.process_id),
                     warning.process_source_line);
    }
    if (!result.warning_records.empty()) std::fputc('\n', stream);
    std::fputs("  ],\n  \"wait_graph\":", stream);
    if (!result.wait_graph) {
        std::fputs("null", stream);
    } else {
        const auto& graph = *result.wait_graph;
        std::fputs("{", stream);
        detail::write_json_field(stream, "status",
                                 wait_graph_status_name(graph.status));
        std::fprintf(stream, ",\"simulation_time_fs\":%llu,\"nodes\":[",
                     static_cast<unsigned long long>(
                         graph.simulation_time_fs));
        for (size_t index = 0; index < graph.nodes.size(); ++index) {
            const auto& node = graph.nodes[index];
            std::fputs(index == 0 ? "{" : ",{", stream);
            std::fprintf(stream,
                         "\"id\":%llu,\"parent_id\":%llu,"
                         "\"process_id\":%llu,\"parent_process_id\":%llu,",
                         static_cast<unsigned long long>(node.id),
                         static_cast<unsigned long long>(node.parent_id),
                         static_cast<unsigned long long>(node.process_id),
                         static_cast<unsigned long long>(
                             node.parent_process_id));
            detail::write_json_field(stream, "process", node.process);
            std::fputs(",", stream);
            detail::write_json_field(stream, "process_source_file",
                                     node.process_source_file);
            std::fprintf(stream, ",\"process_source_line\":%u,",
                         node.process_source_line);
            detail::write_json_field(stream, "reason",
                                     wait_reason_name(node.reason));
            std::fputs(",", stream);
            detail::write_json_field(stream, "resource", node.resource);
            std::fputs(",", stream);
            detail::write_json_field(stream, "wait_source_file",
                                     node.wait_source_file);
            std::fprintf(stream,
                         ",\"wait_source_line\":%u,"
                         "\"wait_started_fs\":%llu,\"deadline_fs\":",
                         node.wait_source_line,
                         static_cast<unsigned long long>(node.wait_started_fs));
            if (node.deadline_fs) {
                std::fprintf(stream, "%llu",
                             static_cast<unsigned long long>(*node.deadline_fs));
            } else {
                std::fputs("null", stream);
            }
            std::fputs(",\"dependencies\":[", stream);
            for (size_t dependency = 0;
                 dependency < node.dependencies.size(); ++dependency) {
                std::fprintf(stream, "%s%llu", dependency == 0 ? "" : ",",
                             static_cast<unsigned long long>(
                                 node.dependencies[dependency]));
            }
            std::fprintf(stream, "],\"ready\":%s,\"running\":%s}",
                         node.ready ? "true" : "false",
                         node.running ? "true" : "false");
        }
        std::fputs("]}", stream);
    }
    std::fputs("\n}\n", stream);
    const bool success = std::fclose(stream) == 0;
    return success;
}

}  // namespace cpptb
