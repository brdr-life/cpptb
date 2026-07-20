#pragma once

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace cpptb {

enum class WaitReason : uint8_t {
    None,
    Task,
    Process,
    Join,
    RisingEdge,
    FallingEdge,
    Edge,
    Delay,
    ReadWrite,
    ReadOnly,
    NextTimeStep,
    Event,
    QueueGet,
    QueuePut,
    Semaphore,
    Lock,
    Timeout,
    First,
};

enum class WaitGraphStatus : uint8_t {
    Complete,
    Runnable,
    WaitingForTime,
    WaitingForSimulator,
    Deadlocked,
};

inline constexpr std::string_view wait_reason_name(WaitReason reason) {
    switch (reason) {
        case WaitReason::None:
            return "none";
        case WaitReason::Task:
            return "task";
        case WaitReason::Process:
            return "process";
        case WaitReason::Join:
            return "Join";
        case WaitReason::RisingEdge:
            return "RisingEdge";
        case WaitReason::FallingEdge:
            return "FallingEdge";
        case WaitReason::Edge:
            return "Edge";
        case WaitReason::Delay:
            return "Delay";
        case WaitReason::ReadWrite:
            return "ReadWrite";
        case WaitReason::ReadOnly:
            return "ReadOnly";
        case WaitReason::NextTimeStep:
            return "NextTimeStep";
        case WaitReason::Event:
            return "Event";
        case WaitReason::QueueGet:
            return "Queue.get";
        case WaitReason::QueuePut:
            return "Queue.put";
        case WaitReason::Semaphore:
            return "Semaphore.acquire";
        case WaitReason::Lock:
            return "Lock.acquire";
        case WaitReason::Timeout:
            return "with_timeout";
        case WaitReason::First:
            return "First";
    }
    return "unknown";
}

inline constexpr std::string_view wait_graph_status_name(
    WaitGraphStatus status) {
    switch (status) {
        case WaitGraphStatus::Complete:
            return "complete";
        case WaitGraphStatus::Runnable:
            return "runnable";
        case WaitGraphStatus::WaitingForTime:
            return "waiting_for_time";
        case WaitGraphStatus::WaitingForSimulator:
            return "waiting_for_simulator";
        case WaitGraphStatus::Deadlocked:
            return "deadlocked";
    }
    return "unknown";
}

struct WaitGraphNode {
    uint64_t id = 0;
    uint64_t parent_id = 0;
    uint64_t process_id = 0;
    uint64_t parent_process_id = 0;
    std::string process;
    std::string process_source_file;
    uint32_t process_source_line = 0;
    WaitReason reason = WaitReason::None;
    std::string resource;
    std::string wait_source_file;
    uint32_t wait_source_line = 0;
    uint64_t wait_started_fs = 0;
    std::optional<uint64_t> deadline_fs;
    std::vector<uint64_t> dependencies;
    bool ready = false;
    bool running = false;
};

struct WaitGraphSnapshot {
    WaitGraphStatus status = WaitGraphStatus::Complete;
    uint64_t simulation_time_fs = 0;
    std::vector<WaitGraphNode> nodes;

    bool deadlocked() const noexcept {
        return status == WaitGraphStatus::Deadlocked;
    }
};

inline std::string format_wait_graph(const WaitGraphSnapshot& graph) {
    std::ostringstream output;
    output << "cpptb wait graph at " << graph.simulation_time_fs << " fs: "
           << wait_graph_status_name(graph.status) << " (" << graph.nodes.size()
           << " active coroutine" << (graph.nodes.size() == 1 ? "" : "s")
           << ")";

    for (const auto& node : graph.nodes) {
        output << "\n  [" << node.id << "] ";
        if (!node.process.empty()) {
            output << node.process;
            if (node.process_id != 0) output << " #" << node.process_id;
        } else {
            output << "scheduler coroutine";
        }
        if (node.parent_id != 0) output << " child-of [" << node.parent_id << "]";
        if (!node.process_source_file.empty()) {
            output << " spawned at " << node.process_source_file << ':'
                   << node.process_source_line;
        }

        output << "\n      ";
        if (node.running) {
            output << "running";
        } else if (node.ready) {
            output << "ready";
        } else {
            output << "waiting on " << wait_reason_name(node.reason);
            if (!node.resource.empty()) output << " (" << node.resource << ')';
            output << " since " << node.wait_started_fs << " fs";
            if (node.deadline_fs) {
                output << ", deadline " << *node.deadline_fs << " fs";
            }
            if (!node.wait_source_file.empty()) {
                output << " at " << node.wait_source_file << ':'
                       << node.wait_source_line;
            }
        }
        if (!node.dependencies.empty()) {
            output << "\n      depends on";
            for (const auto dependency : node.dependencies) {
                output << " [" << dependency << ']';
            }
        }
    }
    return output.str();
}

}  // namespace cpptb
