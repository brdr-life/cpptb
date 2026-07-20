#include <cstdint>
#include <cstdio>
#include <string_view>

#include "cpptb/sim_logging.hpp"

namespace {

std::string_view view_or_empty(const char* value) {
    return value ? std::string_view{value} : std::string_view{};
}

}  // namespace

extern "C" void cpptb_sv_log(unsigned int level, const char* message,
                              const char* scope, const char* source_file,
                              unsigned int source_line, const char* hierarchy,
                              unsigned long long simulation_time_fs) noexcept {
    if (level > static_cast<unsigned int>(cpptb::LogLevel::Error)) {
        std::fprintf(stderr,
                     "cpptb: ignored SystemVerilog log with invalid level %u\n",
                     level);
        return;
    }
    cpptb::detail::emit_sim_log({
        .level = static_cast<cpptb::LogLevel>(level),
        .message = view_or_empty(message),
        .scope = view_or_empty(scope),
        .source_file = view_or_empty(source_file),
        .hierarchy = view_or_empty(hierarchy),
        .simulation_time_fs = simulation_time_fs,
        .source_line = source_line,
    });
}

extern "C" unsigned int cpptb_sv_log_minimum_level() noexcept {
    return static_cast<unsigned int>(cpptb::detail::sim_log_minimum_level());
}
