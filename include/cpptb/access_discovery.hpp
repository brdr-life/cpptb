// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
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

#ifdef CPPTB_HIERARCHY_DISCOVERY
// The object-section twin of the registration above; see the access records
// in hierarchy.hpp for the design. Scanned out of compile-only objects so
// the build recovers the observed-edge set without executing test code.
#ifdef __APPLE__
#define CPPTB_EDGE_DISCOVERY_SECTION "__DATA,cpptb_access"
#else
#define CPPTB_EDGE_DISCOVERY_SECTION "cpptb_access"
#endif

template <std::size_t Size>
struct EdgeSectionRecord {
    char value[Size]{};
};

template <std::uint32_t Id>
consteval auto make_port_edge_record() {
    constexpr char prefix[] = "CPPTB-EDGE-v1;";
    // Ten digits hold any uint32_t; the record is sized generously and the
    // scanner reads to the NUL.
    EdgeSectionRecord<sizeof(prefix) + 10> record{};
    std::size_t at = 0;
    for (std::size_t index = 0; prefix[index] != '\0'; ++index) {
        record.value[at++] = prefix[index];
    }
    char digits[10]{};
    std::size_t digit_count = 0;
    std::uint32_t remaining = Id;
    do {
        digits[digit_count++] = static_cast<char>('0' + remaining % 10);
        remaining /= 10;
    } while (remaining != 0);
    while (digit_count != 0) {
        record.value[at++] = digits[--digit_count];
    }
    record.value[at] = '\0';
    return record;
}

template <std::uint32_t Id>
__attribute__((used, section(CPPTB_EDGE_DISCOVERY_SECTION)))
inline constexpr auto port_edge_section_record = make_port_edge_record<Id>();
#endif  // CPPTB_HIERARCHY_DISCOVERY

}  // namespace detail

template <std::uint32_t Id>
inline void mark_port_edge() {
    (void)&detail::PortEdgeMarker<Id>::registration;
#ifdef CPPTB_HIERARCHY_DISCOVERY
    (void)&detail::port_edge_section_record<Id>;
#endif
}

inline std::vector<std::uint32_t> discovered_port_edge_ids() {
    auto ids = detail::port_edge_ids();
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

}  // namespace cpptb::discovery
