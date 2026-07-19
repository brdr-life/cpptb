#pragma once

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "cpptb/logic_bits.hpp"

namespace cpptb {

struct SimulatorCapabilities {
    std::string_view name;
    bool four_state_values;
    bool four_state_net_resolution;
};

inline constexpr SimulatorCapabilities simulator_capabilities() {
#if defined(VERILATOR)
    return {"Verilator", false, false};
#else
    return {"SystemVerilog simulator", true, true};
#endif
}

template <std::size_t Width>
inline void require_logic_write_supported(const LogicBits<Width>& value,
                                          const char* path,
                                          const char* operation) {
    const auto capabilities = simulator_capabilities();
    if (capabilities.four_state_values || value.is_known()) return;
    std::fprintf(
        stderr,
        "cpptb: %s on '%s' contains X or Z, but %.*s is a two-state "
        "simulator and would silently coerce the value; Verilator's "
        "--fourstate flag is currently upstream-under-development, so run "
        "this test on a standards-compliant four-state backend or drive a "
        "known 0/1 value\n",
        operation, path, static_cast<int>(capabilities.name.size()),
        capabilities.name.data());
    std::abort();
}

}  // namespace cpptb
