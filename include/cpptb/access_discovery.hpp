#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace cpptb::discovery {

namespace detail {

inline std::vector<std::uint32_t>& port_edge_ids() {
    static std::vector<std::uint32_t> ids;
    return ids;
}

template <std::uint32_t Id>
struct PortEdgeMarker {
    struct Registration {
        Registration() { port_edge_ids().push_back(Id); }
    };

    inline static Registration registration{};
};

}  // namespace detail

template <std::uint32_t Id>
inline void mark_port_edge() {
    (void)&detail::PortEdgeMarker<Id>::registration;
}

inline std::vector<std::uint32_t> discovered_port_edge_ids() {
    auto ids = detail::port_edge_ids();
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

}  // namespace cpptb::discovery
