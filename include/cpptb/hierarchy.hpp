#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "cpptb/access_discovery.hpp"
#include "cpptb/coro_runtime.hpp"
#include "cpptb/logic_bits.hpp"
#include "cpptb/probe.hpp"
#include "cpptb/simulator_capabilities.hpp"

namespace cpptb::hierarchy {

enum class Operation : std::uint8_t {
    Get,
    Deposit,
    Force,
    Release,
    RisingEdge,
    FallingEdge,
    AnyEdge,
    GetLogic,
    DepositLogic,
    ForceLogic,
};

constexpr std::string_view operation_name(Operation operation) {
    switch (operation) {
        case Operation::Get:
            return "get";
        case Operation::Deposit:
            return "deposit";
        case Operation::Force:
            return "force";
        case Operation::Release:
            return "release";
        case Operation::RisingEdge:
            return "rising_edge";
        case Operation::FallingEdge:
            return "falling_edge";
        case Operation::AnyEdge:
            return "any_edge";
        case Operation::GetLogic:
            return "get_logic";
        case Operation::DepositLogic:
            return "deposit_logic";
        case Operation::ForceLogic:
            return "force_logic";
    }
    return "unknown";
}

template <std::size_t Size>
struct FixedString {
    char value[Size]{};

    consteval FixedString(const char (&text)[Size]) {
        for (std::size_t index = 0; index < Size; ++index) {
            value[index] = text[index];
        }
    }

    [[nodiscard]] constexpr std::string_view view() const {
        return {value, Size - 1};
    }
};

template <std::size_t Size>
FixedString(const char (&)[Size]) -> FixedString<Size>;

template <std::int32_t Index, typename Scope>
struct ScopeElement {
    static constexpr std::int32_t index = Index;
    using scope_type = Scope;
};

namespace detail {

template <std::int32_t Index, typename... Elements>
struct ScopeAt;

template <std::int32_t Index, typename First, typename... Rest>
struct ScopeAt<Index, First, Rest...> {
    using type = typename ScopeAt<Index, Rest...>::type;
};

template <std::int32_t Index, typename Scope, typename... Rest>
struct ScopeAt<Index, ScopeElement<Index, Scope>, Rest...> {
    using type = Scope;
};

template <std::int32_t Index>
struct ScopeAt<Index> {
    static_assert(Index != Index, "hierarchy scope array index is out of range");
};

}  // namespace detail

template <typename... Elements>
class ScopeArray {
   public:
    static constexpr std::size_t size = sizeof...(Elements);

    template <std::int32_t Index>
    [[nodiscard]] constexpr auto at() const {
        using Scope = typename detail::ScopeAt<Index, Elements...>::type;
        return Scope{};
    }
};

struct UnsupportedSignal {};

template <std::int32_t Left, std::int32_t Right>
struct Dimension {
    static constexpr std::int32_t left = Left;
    static constexpr std::int32_t right = Right;
    static constexpr std::int32_t low = Left < Right ? Left : Right;
    static constexpr std::int32_t high = Left < Right ? Right : Left;
    static constexpr std::size_t size =
        static_cast<std::size_t>(high - low + 1);
};

struct Access {
    std::string_view path;
    Operation operation = Operation::Get;

    friend bool operator==(const Access&, const Access&) = default;
};

namespace detail {

inline std::vector<Access>& discovered_accesses() {
    static std::vector<Access> accesses;
    return accesses;
}

template <FixedString Path, Operation SelectedOperation>
struct AccessMarker {
    struct Registration {
        Registration() {
            discovered_accesses().push_back(
                Access{Path.view(), SelectedOperation});
        }
    };

    inline static Registration registration{};
};

#ifdef CPPTB_HIERARCHY_DISCOVERY
// The same registrations, readable without running anything. Each marker
// instantiation also plants a NUL-terminated record in a dedicated object
// section, so compiling the testbench translation units with `-c` is enough
// for the build to recover the access set: it scans the objects for the
// records below, with no generated main, no link, and no execution of test
// code. The executed path produces the identical plan -- the build compares
// the two byte for byte until the executed path is retired.
#ifdef __APPLE__
#define CPPTB_DISCOVERY_SECTION "__DATA,cpptb_access"
#else
#define CPPTB_DISCOVERY_SECTION "cpptb_access"
#endif

template <std::size_t Size>
struct SectionRecord {
    char value[Size]{};
};

template <FixedString Path, Operation SelectedOperation>
consteval auto make_access_record() {
    constexpr std::string_view prefix = "CPPTB-ACCESS-v1;";
    constexpr std::string_view operation = operation_name(SelectedOperation);
    constexpr std::string_view path = Path.view();
    SectionRecord<16 + operation.size() + 1 + path.size() + 1> record{};
    std::size_t at = 0;
    for (const char character : prefix) record.value[at++] = character;
    for (const char character : operation) record.value[at++] = character;
    record.value[at++] = ';';
    for (const char character : path) record.value[at++] = character;
    record.value[at] = '\0';
    return record;
}

template <FixedString Path, Operation SelectedOperation>
__attribute__((used, section(CPPTB_DISCOVERY_SECTION)))
inline constexpr auto access_section_record =
    make_access_record<Path, SelectedOperation>();
#endif  // CPPTB_HIERARCHY_DISCOVERY

template <FixedString Path, Operation SelectedOperation>
inline void mark_access() {
    (void)&AccessMarker<Path, SelectedOperation>::registration;
#ifdef CPPTB_HIERARCHY_DISCOVERY
    (void)&access_section_record<Path, SelectedOperation>;
#endif
}

inline void mark_access(std::string_view path, Operation operation) {
    discovered_accesses().push_back(Access{path, operation});
}

inline std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        switch (character) {
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result += character;
                break;
        }
    }
    return result;
}

template <typename Value>
constexpr Value normalize(Value value, std::size_t width) {
    if constexpr (std::is_same_v<Value, std::uint32_t>) {
        if (width < 32) value &= (std::uint32_t{1} << width) - 1;
    } else if constexpr (std::is_same_v<Value, std::uint64_t>) {
        if (width < 64) value &= (std::uint64_t{1} << width) - 1;
    }
    return value;
}

template <typename Value, typename Raw>
constexpr Value from_transport(Raw value) {
    if constexpr (std::is_same_v<Value, Raw>) {
        return value;
    } else {
        return Value::from_signal_value(std::move(value));
    }
}

template <typename Raw, typename Value>
constexpr Raw to_transport(Value value) {
    if constexpr (std::is_same_v<Value, Raw>) {
        return value;
    } else {
        return value.signal_value();
    }
}

}  // namespace detail

struct RuntimeAccessPaths {
    template <Operation SelectedOperation>
    static void mark(std::string_view path) {
        detail::mark_access(path, SelectedOperation);
    }
};

template <FixedString... Paths>
struct AccessPathSet {
    template <Operation SelectedOperation>
    static void mark(std::string_view) {
        (detail::mark_access<Paths, SelectedOperation>(), ...);
    }
};

inline std::vector<Access> discovered_access_plan() {
    auto accesses = detail::discovered_accesses();
    std::sort(accesses.begin(), accesses.end(), [](Access left, Access right) {
        return std::pair{left.path, left.operation} <
               std::pair{right.path, right.operation};
    });
    accesses.erase(std::unique(accesses.begin(), accesses.end()),
                   accesses.end());
    return accesses;
}

inline bool write_discovered_access_plan(const char* output_path) {
    std::ofstream output(output_path);
    if (!output) {
        std::fprintf(stderr, "cpptb: cannot write hierarchy access plan '%s'\n",
                     output_path ? output_path : "<null>");
        return false;
    }
    const auto accesses = discovered_access_plan();
    output << "{\n  \"schema_version\": 1,\n  \"accesses\": [";
    for (std::size_t index = 0; index < accesses.size(); ++index) {
        const auto access = accesses[index];
        output << (index == 0 ? "\n" : ",\n")
               << "    {\"path\": \""
               << detail::json_escape(access.path)
               << "\", \"operation\": \""
               << operation_name(access.operation) << "\"}";
    }
    output << (accesses.empty() ? "" : "\n  ") << "],\n";
    const auto port_edges = discovery::discovered_port_edge_ids();
    output << "  \"port_edges\": [";
    for (std::size_t index = 0; index < port_edges.size(); ++index) {
        output << (index == 0 ? "" : ", ") << port_edges[index];
    }
    output << "]\n}\n";
    return static_cast<bool>(output);
}

template <typename Transport, std::uint32_t Id, FixedString Path,
          std::size_t Width, bool Depositable,
          typename UserValue = probe::Value<Width>, bool FourState = true>
class Signal {
   public:
    static_assert(Width > 0);
    using raw_value_type = probe::Value<Width>;
    using value_type = UserValue;
    static constexpr std::size_t width = Width;
    static constexpr std::uint32_t id = Id;
    static constexpr std::string_view path = Path.view();
    static constexpr bool four_state = FourState;

    [[nodiscard]] value_type get() const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        detail::mark_access<Path, Operation::Get>();
        return {};
#else
        probe::detail::require_callback(Path.value);
        return detail::from_transport<value_type>(detail::normalize(
            Transport::template get<Width>(Id, 0), Width));
#endif
    }

    [[nodiscard]] LogicBits<Width> get_logic() const requires(FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        detail::mark_access<Path, Operation::GetLogic>();
        return {};
#else
        probe::detail::require_callback(Path.value);
        return Transport::template get_logic<Width>(Id, 0);
#endif
    }

    template <typename View>
        requires requires(value_type value) { View::from_raw(value); }
    [[nodiscard]] View get_as() const {
        return View::from_raw(get());
    }

    void deposit(value_type value) const requires(Depositable) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        detail::mark_access<Path, Operation::Deposit>();
        (void)value;
#else
        probe::detail::require_callback(Path.value);
        probe::detail::require_write_allowed(Path.value, "deposit");
        auto raw = detail::to_transport<raw_value_type>(std::move(value));
        Transport::template deposit<Width>(
            Id, 0, detail::normalize(std::move(raw), Width));
#endif
    }

    void deposit_logic(LogicBits<Width> value) const
        requires(Depositable && FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        detail::mark_access<Path, Operation::DepositLogic>();
        (void)value;
#else
        require_logic_write_supported(value, Path.value, "deposit_logic");
        probe::detail::require_callback(Path.value);
        probe::detail::require_write_allowed(Path.value, "deposit_logic");
        Transport::template deposit_logic<Width>(Id, 0, std::move(value));
#endif
    }

    template <typename View>
        requires(Depositable && requires(View value) { value.raw(); })
    void deposit_as(const View& value) const {
        if constexpr (Width <= 64) {
            deposit(static_cast<value_type>(value.raw().to_uint64()));
        } else {
            deposit(value.raw());
        }
    }

    void force(value_type value) const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        detail::mark_access<Path, Operation::Force>();
        (void)value;
#else
        probe::detail::require_callback(Path.value);
        probe::detail::require_write_allowed(Path.value, "force");
        auto raw = detail::to_transport<raw_value_type>(std::move(value));
        Transport::template force<Width>(
            Id, 0, detail::normalize(std::move(raw), Width));
#endif
    }

    void force_logic(LogicBits<Width> value) const requires(FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        detail::mark_access<Path, Operation::ForceLogic>();
        (void)value;
#else
        require_logic_write_supported(value, Path.value, "force_logic");
        probe::detail::require_callback(Path.value);
        probe::detail::require_write_allowed(Path.value, "force_logic");
        Transport::template force_logic<Width>(Id, 0, std::move(value));
#endif
    }

    void release() const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        detail::mark_access<Path, Operation::Release>();
#else
        probe::detail::require_callback(Path.value);
        probe::detail::require_write_allowed(Path.value, "release");
        Transport::release(Id, 0);
#endif
    }

    operator coro::Signal() const requires(Width == 1) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        detail::mark_access<Path, Operation::AnyEdge>();
        return {nullptr, 0, Path.value};
#else
        probe::detail::require_callback(Path.value);
        return Transport::signal(Id, Path.value);
#endif
    }
};

template <typename Transport, std::size_t Width, bool Depositable,
          typename UserValue = probe::Value<Width>, bool FourState = true,
          typename AccessPaths = RuntimeAccessPaths>
class SelectedSignal {
   public:
    using raw_value_type = probe::Value<Width>;
    using value_type = UserValue;
    static constexpr std::size_t width = Width;
    static constexpr bool four_state = FourState;

    std::uint32_t id = 0;
    const char* path = "";

    [[nodiscard]] value_type get() const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        AccessPaths::template mark<Operation::Get>(path);
        return {};
#else
        probe::detail::require_callback(path);
        return detail::from_transport<value_type>(detail::normalize(
            Transport::template get<Width>(id, 0), Width));
#endif
    }

    [[nodiscard]] LogicBits<Width> get_logic() const requires(FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        AccessPaths::template mark<Operation::GetLogic>(path);
        return {};
#else
        probe::detail::require_callback(path);
        return Transport::template get_logic<Width>(id, 0);
#endif
    }

    template <typename View>
        requires requires(value_type value) { View::from_raw(value); }
    [[nodiscard]] View get_as() const {
        return View::from_raw(get());
    }

    void deposit(value_type value) const requires(Depositable) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        AccessPaths::template mark<Operation::Deposit>(path);
        (void)value;
#else
        probe::detail::require_callback(path);
        probe::detail::require_write_allowed(path, "deposit");
        auto raw = detail::to_transport<raw_value_type>(std::move(value));
        Transport::template deposit<Width>(
            id, 0, detail::normalize(std::move(raw), Width));
#endif
    }

    void deposit_logic(LogicBits<Width> value) const
        requires(Depositable && FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        AccessPaths::template mark<Operation::DepositLogic>(path);
        (void)value;
#else
        require_logic_write_supported(value, path, "deposit_logic");
        probe::detail::require_callback(path);
        probe::detail::require_write_allowed(path, "deposit_logic");
        Transport::template deposit_logic<Width>(id, 0, std::move(value));
#endif
    }

    template <typename View>
        requires(Depositable && requires(View value) { value.raw(); })
    void deposit_as(const View& value) const {
        if constexpr (Width <= 64) {
            deposit(static_cast<value_type>(value.raw().to_uint64()));
        } else {
            deposit(value.raw());
        }
    }

    void force(value_type value) const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        AccessPaths::template mark<Operation::Force>(path);
        (void)value;
#else
        probe::detail::require_callback(path);
        probe::detail::require_write_allowed(path, "force");
        auto raw = detail::to_transport<raw_value_type>(std::move(value));
        Transport::template force<Width>(
            id, 0, detail::normalize(std::move(raw), Width));
#endif
    }

    void force_logic(LogicBits<Width> value) const requires(FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        AccessPaths::template mark<Operation::ForceLogic>(path);
        (void)value;
#else
        require_logic_write_supported(value, path, "force_logic");
        probe::detail::require_callback(path);
        probe::detail::require_write_allowed(path, "force_logic");
        Transport::template force_logic<Width>(id, 0, std::move(value));
#endif
    }

    void release() const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        AccessPaths::template mark<Operation::Release>(path);
#else
        probe::detail::require_callback(path);
        probe::detail::require_write_allowed(path, "release");
        Transport::release(id, 0);
#endif
    }

    operator coro::Signal() const requires(Width == 1) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        AccessPaths::template mark<Operation::AnyEdge>(path);
        return {nullptr, 0, path};
#else
        probe::detail::require_callback(path);
        return Transport::signal(id, path);
#endif
    }
};

template <typename Transport, std::uint32_t Id, FixedString Path,
          std::size_t Width, std::int32_t Left, std::int32_t Right,
          bool Depositable, typename UserValue = probe::Value<Width>,
          bool FourState = true>
class Memory {
   public:
    using raw_value_type = probe::Value<Width>;
    using value_type = UserValue;
    static constexpr std::size_t width = Width;
    static constexpr std::int32_t left = Left;
    static constexpr std::int32_t right = Right;
    static constexpr std::int32_t low = Left < Right ? Left : Right;
    static constexpr std::int32_t high = Left < Right ? Right : Left;
    static constexpr std::size_t size =
        static_cast<std::size_t>(high - low + 1);
    static constexpr bool four_state = FourState;

    class Element {
       public:
        [[nodiscard]] value_type get() const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            detail::mark_access<Path, Operation::Get>();
            return {};
#else
            probe::detail::require_callback(Path.value);
            return detail::from_transport<value_type>(detail::normalize(
                Transport::template get<Width>(Id, index_), Width));
#endif
        }

        [[nodiscard]] LogicBits<Width> get_logic() const requires(FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            detail::mark_access<Path, Operation::GetLogic>();
            return {};
#else
            probe::detail::require_callback(Path.value);
            return Transport::template get_logic<Width>(Id, index_);
#endif
        }

        template <typename View>
            requires requires(value_type value) { View::from_raw(value); }
        [[nodiscard]] View get_as() const {
            return View::from_raw(get());
        }

        void deposit(value_type value) const requires(Depositable) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            detail::mark_access<Path, Operation::Deposit>();
            (void)value;
#else
            probe::detail::require_callback(Path.value);
            probe::detail::require_write_allowed(Path.value, "deposit");
            auto raw = detail::to_transport<raw_value_type>(std::move(value));
            Transport::template deposit<Width>(
                Id, index_, detail::normalize(std::move(raw), Width));
#endif
        }

        void deposit_logic(LogicBits<Width> value) const
            requires(Depositable && FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            detail::mark_access<Path, Operation::DepositLogic>();
            (void)value;
#else
            require_logic_write_supported(value, Path.value, "deposit_logic");
            probe::detail::require_callback(Path.value);
            probe::detail::require_write_allowed(Path.value, "deposit_logic");
            Transport::template deposit_logic<Width>(Id, index_,
                                                      std::move(value));
#endif
        }

        template <typename View>
            requires(Depositable && requires(View value) { value.raw(); })
        void deposit_as(const View& value) const {
            if constexpr (Width <= 64) {
                deposit(static_cast<value_type>(value.raw().to_uint64()));
            } else {
                deposit(value.raw());
            }
        }

        void force(value_type value) const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            detail::mark_access<Path, Operation::Force>();
            (void)value;
#else
            probe::detail::require_callback(Path.value);
            probe::detail::require_write_allowed(Path.value, "force");
            auto raw = detail::to_transport<raw_value_type>(std::move(value));
            Transport::template force<Width>(
                Id, index_, detail::normalize(std::move(raw), Width));
#endif
        }

        void force_logic(LogicBits<Width> value) const requires(FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            detail::mark_access<Path, Operation::ForceLogic>();
            (void)value;
#else
            require_logic_write_supported(value, Path.value, "force_logic");
            probe::detail::require_callback(Path.value);
            probe::detail::require_write_allowed(Path.value, "force_logic");
            Transport::template force_logic<Width>(Id, index_,
                                                    std::move(value));
#endif
        }

        void release() const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            detail::mark_access<Path, Operation::Release>();
#else
            probe::detail::require_callback(Path.value);
            probe::detail::require_write_allowed(Path.value, "release");
            Transport::release(Id, index_);
#endif
        }

       private:
        friend Memory;
        explicit constexpr Element(std::int32_t index) : index_(index) {}
        std::int32_t index_ = 0;
    };

    [[nodiscard]] Element at(std::int32_t index) const {
        if (index < low || index > high) {
            std::fprintf(stderr,
                         "cpptb: hierarchy memory '%s' index %d is out of "
                         "bounds [%d:%d]\n",
                         Path.value, index, Left, Right);
            std::abort();
        }
        return Element{index};
    }

    [[nodiscard]] Element operator[](std::int32_t index) const {
        return at(index);
    }

    void get_into(std::int32_t first_index,
                  std::span<value_type> values) const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        detail::mark_access<Path, Operation::Get>();
        (void)first_index;
        (void)values;
#else
        check_range(first_index, values.size());
        if (values.empty()) return;
        probe::detail::require_callback(Path.value);
        if constexpr (
            std::is_same_v<value_type, raw_value_type> &&
            requires(std::span<raw_value_type> raw_values) {
                Transport::template get_span<Width>(Id, first_index,
                                                    raw_values);
            }) {
            Transport::template get_span<Width>(Id, first_index, values);
            if constexpr (Width != 32 && Width != 64) {
                for (auto& value : values) {
                    value = detail::normalize(std::move(value), Width);
                }
            }
        } else {
            for (std::size_t offset = 0; offset < values.size(); ++offset) {
                values[offset] = at(static_cast<std::int32_t>(
                                        static_cast<std::int64_t>(first_index) +
                                        static_cast<std::int64_t>(offset)))
                                     .get();
            }
        }
#endif
    }

    void deposit(std::int32_t first_index,
                 std::span<const value_type> values) const
        requires(Depositable) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        detail::mark_access<Path, Operation::Deposit>();
        (void)first_index;
        (void)values;
#else
        check_range(first_index, values.size());
        if (values.empty()) return;
        probe::detail::require_write_callback(Path.value, "deposit");
        if constexpr (
            std::is_same_v<value_type, raw_value_type> &&
            requires(std::span<const raw_value_type> raw_values) {
                Transport::template deposit_span<Width>(Id, first_index,
                                                        raw_values);
            }) {
            Transport::template deposit_span<Width>(Id, first_index, values);
        } else {
            for (std::size_t offset = 0; offset < values.size(); ++offset) {
                at(static_cast<std::int32_t>(
                       static_cast<std::int64_t>(first_index) +
                       static_cast<std::int64_t>(offset)))
                    .deposit(values[offset]);
            }
        }
#endif
    }

   private:
    static void check_range(std::int32_t first_index, std::size_t count) {
        const auto first = static_cast<std::int64_t>(first_index);
        const auto available = first <= high
                                   ? static_cast<std::uint64_t>(high - first + 1)
                                   : 0;
        if (first >= low && count <= available) return;
        fail_range(first_index, count);
    }

    [[noreturn]] static void fail_range(std::int32_t first_index,
                                        std::size_t count) {
        std::fprintf(stderr,
                     "cpptb: hierarchy memory '%s' range [%d, +%zu) is out "
                     "of bounds [%d:%d]\n",
                     Path.value, first_index, count, Left, Right);
        std::abort();
    }
};

template <typename Transport, std::size_t Width, std::int32_t Left,
          std::int32_t Right, bool Depositable,
          typename UserValue = probe::Value<Width>, bool FourState = true,
          typename AccessPaths = RuntimeAccessPaths>
class SelectedMemory {
   public:
    using raw_value_type = probe::Value<Width>;
    using value_type = UserValue;
    static constexpr std::size_t width = Width;
    static constexpr std::int32_t left = Left;
    static constexpr std::int32_t right = Right;
    static constexpr std::int32_t low = Left < Right ? Left : Right;
    static constexpr std::int32_t high = Left < Right ? Right : Left;
    static constexpr std::size_t size =
        static_cast<std::size_t>(high - low + 1);
    static constexpr bool four_state = FourState;

    class Element {
       public:
        [[nodiscard]] value_type get() const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            AccessPaths::template mark<Operation::Get>(path_);
            return {};
#else
            probe::detail::require_callback(path_);
            return detail::from_transport<value_type>(detail::normalize(
                Transport::template get<Width>(id_, index_), Width));
#endif
        }

        [[nodiscard]] LogicBits<Width> get_logic() const requires(FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            AccessPaths::template mark<Operation::GetLogic>(path_);
            return {};
#else
            probe::detail::require_callback(path_);
            return Transport::template get_logic<Width>(id_, index_);
#endif
        }

        template <typename View>
            requires requires(value_type value) { View::from_raw(value); }
        [[nodiscard]] View get_as() const {
            return View::from_raw(get());
        }

        void deposit(value_type value) const requires(Depositable) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            AccessPaths::template mark<Operation::Deposit>(path_);
            (void)value;
#else
            probe::detail::require_callback(path_);
            probe::detail::require_write_allowed(path_, "deposit");
            auto raw = detail::to_transport<raw_value_type>(std::move(value));
            Transport::template deposit<Width>(
                id_, index_, detail::normalize(std::move(raw), Width));
#endif
        }

        void deposit_logic(LogicBits<Width> value) const
            requires(Depositable && FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            AccessPaths::template mark<Operation::DepositLogic>(path_);
            (void)value;
#else
            require_logic_write_supported(value, path_, "deposit_logic");
            probe::detail::require_callback(path_);
            probe::detail::require_write_allowed(path_, "deposit_logic");
            Transport::template deposit_logic<Width>(id_, index_,
                                                      std::move(value));
#endif
        }

        template <typename View>
            requires(Depositable && requires(View value) { value.raw(); })
        void deposit_as(const View& value) const {
            if constexpr (Width <= 64) {
                deposit(static_cast<value_type>(value.raw().to_uint64()));
            } else {
                deposit(value.raw());
            }
        }

        void force(value_type value) const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            AccessPaths::template mark<Operation::Force>(path_);
            (void)value;
#else
            probe::detail::require_callback(path_);
            probe::detail::require_write_allowed(path_, "force");
            auto raw = detail::to_transport<raw_value_type>(std::move(value));
            Transport::template force<Width>(
                id_, index_, detail::normalize(std::move(raw), Width));
#endif
        }

        void force_logic(LogicBits<Width> value) const requires(FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            AccessPaths::template mark<Operation::ForceLogic>(path_);
            (void)value;
#else
            require_logic_write_supported(value, path_, "force_logic");
            probe::detail::require_callback(path_);
            probe::detail::require_write_allowed(path_, "force_logic");
            Transport::template force_logic<Width>(id_, index_,
                                                    std::move(value));
#endif
        }

        void release() const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            AccessPaths::template mark<Operation::Release>(path_);
#else
            probe::detail::require_callback(path_);
            probe::detail::require_write_allowed(path_, "release");
            Transport::release(id_, index_);
#endif
        }

       private:
        friend SelectedMemory;
        Element(std::uint32_t id, const char* path, std::int32_t index)
            : id_(id), path_(path), index_(index) {}
        std::uint32_t id_ = 0;
        const char* path_ = "";
        std::int32_t index_ = 0;
    };

    std::uint32_t id = 0;
    const char* path = "";

    [[nodiscard]] Element operator[](std::int32_t index) const {
        if (index < low || index > high) {
            std::fprintf(stderr,
                         "cpptb: hierarchy memory '%s' index %d is out of "
                         "bounds [%d:%d]\n",
                         path, index, Left, Right);
            std::abort();
        }
        return Element{id, path, index};
    }

    [[nodiscard]] Element at(std::int32_t index) const {
        return (*this)[index];
    }

    void get_into(std::int32_t first_index,
                  std::span<value_type> values) const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        AccessPaths::template mark<Operation::Get>(path);
        (void)first_index;
        (void)values;
#else
        check_range(first_index, values.size());
        if (values.empty()) return;
        probe::detail::require_callback(path);
        if constexpr (
            std::is_same_v<value_type, raw_value_type> &&
            requires(std::span<raw_value_type> raw_values) {
                Transport::template get_span<Width>(id, first_index,
                                                    raw_values);
            }) {
            Transport::template get_span<Width>(id, first_index, values);
            if constexpr (Width != 32 && Width != 64) {
                for (auto& value : values) {
                    value = detail::normalize(std::move(value), Width);
                }
            }
        } else {
            for (std::size_t offset = 0; offset < values.size(); ++offset) {
                values[offset] = (*this)[static_cast<std::int32_t>(
                    static_cast<std::int64_t>(first_index) +
                    static_cast<std::int64_t>(offset))]
                                     .get();
            }
        }
#endif
    }

    void deposit(std::int32_t first_index,
                 std::span<const value_type> values) const
        requires(Depositable) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        AccessPaths::template mark<Operation::Deposit>(path);
        (void)first_index;
        (void)values;
#else
        check_range(first_index, values.size());
        if (values.empty()) return;
        probe::detail::require_write_callback(path, "deposit");
        if constexpr (
            std::is_same_v<value_type, raw_value_type> &&
            requires(std::span<const raw_value_type> raw_values) {
                Transport::template deposit_span<Width>(id, first_index,
                                                        raw_values);
            }) {
            Transport::template deposit_span<Width>(id, first_index, values);
        } else {
            for (std::size_t offset = 0; offset < values.size(); ++offset) {
                (*this)[static_cast<std::int32_t>(
                    static_cast<std::int64_t>(first_index) +
                    static_cast<std::int64_t>(offset))]
                    .deposit(values[offset]);
            }
        }
#endif
    }

   private:
    void check_range(std::int32_t first_index, std::size_t count) const {
        const auto first = static_cast<std::int64_t>(first_index);
        const auto available = first <= high
                                   ? static_cast<std::uint64_t>(high - first + 1)
                                   : 0;
        if (first >= low && count <= available) return;
        std::fprintf(stderr,
                     "cpptb: hierarchy memory '%s' range [%d, +%zu) is out "
                     "of bounds [%d:%d]\n",
                     path, first_index, count, Left, Right);
        std::abort();
    }
};

template <typename Transport, std::uint32_t Id, FixedString Path,
          std::size_t Width, bool Depositable, bool FourState,
          typename UserValue, typename... Dimensions>
class MemoryND {
    static_assert(sizeof...(Dimensions) > 1);
    static_assert((std::is_class_v<Dimensions> && ...));

   public:
    using raw_value_type = probe::Value<Width>;
    using value_type = UserValue;
    static constexpr std::size_t width = Width;
    static constexpr std::size_t rank = sizeof...(Dimensions);
    static constexpr std::size_t size = (Dimensions::size * ... * 1);
    static constexpr bool four_state = FourState;

   private:
    class Element {
       public:
        [[nodiscard]] value_type get() const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            detail::mark_access<Path, Operation::Get>();
            return {};
#else
            probe::detail::require_callback(Path.value);
            return detail::from_transport<value_type>(detail::normalize(
                Transport::template get<Width>(Id, linear_index_), Width));
#endif
        }

        [[nodiscard]] LogicBits<Width> get_logic() const requires(FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            detail::mark_access<Path, Operation::GetLogic>();
            return {};
#else
            probe::detail::require_callback(Path.value);
            return Transport::template get_logic<Width>(Id, linear_index_);
#endif
        }

        template <typename View>
            requires requires(value_type value) { View::from_raw(value); }
        [[nodiscard]] View get_as() const {
            return View::from_raw(get());
        }

        void deposit(value_type value) const requires(Depositable) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            detail::mark_access<Path, Operation::Deposit>();
            (void)value;
#else
            probe::detail::require_callback(Path.value);
            probe::detail::require_write_allowed(Path.value, "deposit");
            auto raw = detail::to_transport<raw_value_type>(std::move(value));
            Transport::template deposit<Width>(
                Id, linear_index_, detail::normalize(std::move(raw), Width));
#endif
        }

        void deposit_logic(LogicBits<Width> value) const
            requires(Depositable && FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            detail::mark_access<Path, Operation::DepositLogic>();
            (void)value;
#else
            require_logic_write_supported(value, Path.value, "deposit_logic");
            probe::detail::require_callback(Path.value);
            probe::detail::require_write_allowed(Path.value, "deposit_logic");
            Transport::template deposit_logic<Width>(Id, linear_index_,
                                                      std::move(value));
#endif
        }

        template <typename View>
            requires(Depositable && requires(View value) { value.raw(); })
        void deposit_as(const View& value) const {
            if constexpr (Width <= 64) {
                deposit(static_cast<value_type>(value.raw().to_uint64()));
            } else {
                deposit(value.raw());
            }
        }

        void force(value_type value) const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            detail::mark_access<Path, Operation::Force>();
            (void)value;
#else
            probe::detail::require_callback(Path.value);
            probe::detail::require_write_allowed(Path.value, "force");
            auto raw = detail::to_transport<raw_value_type>(std::move(value));
            Transport::template force<Width>(
                Id, linear_index_, detail::normalize(std::move(raw), Width));
#endif
        }

        void force_logic(LogicBits<Width> value) const requires(FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            detail::mark_access<Path, Operation::ForceLogic>();
            (void)value;
#else
            require_logic_write_supported(value, Path.value, "force_logic");
            probe::detail::require_callback(Path.value);
            probe::detail::require_write_allowed(Path.value, "force_logic");
            Transport::template force_logic<Width>(Id, linear_index_,
                                                    std::move(value));
#endif
        }

        void release() const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            detail::mark_access<Path, Operation::Release>();
#else
            probe::detail::require_callback(Path.value);
            probe::detail::require_write_allowed(Path.value, "release");
            Transport::release(Id, linear_index_);
#endif
        }

       public:
        explicit constexpr Element(std::int32_t linear_index)
            : linear_index_(linear_index) {}

       private:
        std::int32_t linear_index_ = 0;
    };

    template <std::size_t NextDimension>
    class Selection {
       public:
        [[nodiscard]] auto at(std::int32_t index) const {
            using Current = std::tuple_element_t<
                NextDimension, std::tuple<Dimensions...>>;
            if (index < Current::low || index > Current::high) {
                fail_index(index, Current::left, Current::right);
            }
            const auto linear = static_cast<std::int32_t>(
                linear_index_ * Current::size + (index - Current::low));
            if constexpr (NextDimension + 1 == rank) {
                return Element{linear};
            } else {
                return Selection<NextDimension + 1>{linear};
            }
        }

        [[nodiscard]] auto operator[](std::int32_t index) const {
            return at(index);
        }

       private:
        friend MemoryND;
        template <std::size_t>
        friend class Selection;
        explicit constexpr Selection(std::int32_t linear_index)
            : linear_index_(linear_index) {}
        std::int32_t linear_index_ = 0;
    };

   public:
    [[nodiscard]] auto at(std::int32_t index) const {
        using First = std::tuple_element_t<0, std::tuple<Dimensions...>>;
        if (index < First::low || index > First::high) {
            fail_index(index, First::left, First::right);
        }
        return Selection<1>{index - First::low};
    }

    [[nodiscard]] auto operator[](std::int32_t index) const {
        return at(index);
    }

   private:
    [[noreturn]] static void fail_index(std::int32_t index,
                                        std::int32_t left,
                                        std::int32_t right) {
        std::fprintf(stderr,
                     "cpptb: hierarchy memory '%s' index %d is out of "
                     "bounds [%d:%d]\n",
                     Path.value, index, left, right);
        std::abort();
    }
};

template <typename Transport, std::size_t Width, bool Depositable,
          bool FourState, typename UserValue, typename AccessPaths,
          typename... Dimensions>
class SelectedMemoryND {
    static_assert(sizeof...(Dimensions) > 1);

   public:
    using raw_value_type = probe::Value<Width>;
    using value_type = UserValue;
    static constexpr std::size_t width = Width;
    static constexpr std::size_t rank = sizeof...(Dimensions);
    static constexpr std::size_t size = (Dimensions::size * ... * 1);
    static constexpr bool four_state = FourState;

   private:
    class Element {
       public:
        [[nodiscard]] value_type get() const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            AccessPaths::template mark<Operation::Get>(path_);
            return {};
#else
            probe::detail::require_callback(path_);
            return detail::from_transport<value_type>(detail::normalize(
                Transport::template get<Width>(id_, linear_index_), Width));
#endif
        }

        [[nodiscard]] LogicBits<Width> get_logic() const requires(FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            AccessPaths::template mark<Operation::GetLogic>(path_);
            return {};
#else
            probe::detail::require_callback(path_);
            return Transport::template get_logic<Width>(id_, linear_index_);
#endif
        }

        template <typename View>
            requires requires(value_type value) { View::from_raw(value); }
        [[nodiscard]] View get_as() const {
            return View::from_raw(get());
        }

        void deposit(value_type value) const requires(Depositable) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            AccessPaths::template mark<Operation::Deposit>(path_);
            (void)value;
#else
            probe::detail::require_callback(path_);
            probe::detail::require_write_allowed(path_, "deposit");
            auto raw = detail::to_transport<raw_value_type>(std::move(value));
            Transport::template deposit<Width>(
                id_, linear_index_, detail::normalize(std::move(raw), Width));
#endif
        }

        void deposit_logic(LogicBits<Width> value) const
            requires(Depositable && FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            AccessPaths::template mark<Operation::DepositLogic>(path_);
            (void)value;
#else
            require_logic_write_supported(value, path_, "deposit_logic");
            probe::detail::require_callback(path_);
            probe::detail::require_write_allowed(path_, "deposit_logic");
            Transport::template deposit_logic<Width>(id_, linear_index_,
                                                      std::move(value));
#endif
        }

        template <typename View>
            requires(Depositable && requires(View value) { value.raw(); })
        void deposit_as(const View& value) const {
            if constexpr (Width <= 64) {
                deposit(static_cast<value_type>(value.raw().to_uint64()));
            } else {
                deposit(value.raw());
            }
        }

        void force(value_type value) const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            AccessPaths::template mark<Operation::Force>(path_);
            (void)value;
#else
            probe::detail::require_callback(path_);
            probe::detail::require_write_allowed(path_, "force");
            auto raw = detail::to_transport<raw_value_type>(std::move(value));
            Transport::template force<Width>(
                id_, linear_index_, detail::normalize(std::move(raw), Width));
#endif
        }

        void force_logic(LogicBits<Width> value) const requires(FourState) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            AccessPaths::template mark<Operation::ForceLogic>(path_);
            (void)value;
#else
            require_logic_write_supported(value, path_, "force_logic");
            probe::detail::require_callback(path_);
            probe::detail::require_write_allowed(path_, "force_logic");
            Transport::template force_logic<Width>(id_, linear_index_,
                                                    std::move(value));
#endif
        }

        void release() const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
            AccessPaths::template mark<Operation::Release>(path_);
#else
            probe::detail::require_callback(path_);
            probe::detail::require_write_allowed(path_, "release");
            Transport::release(id_, linear_index_);
#endif
        }

        Element(std::uint32_t id, const char* path,
                std::int32_t linear_index)
            : id_(id), path_(path), linear_index_(linear_index) {}

       private:
        std::uint32_t id_ = 0;
        const char* path_ = "";
        std::int32_t linear_index_ = 0;
    };

    template <std::size_t NextDimension>
    class Selection {
       public:
        [[nodiscard]] auto operator[](std::int32_t index) const {
            using Current = std::tuple_element_t<
                NextDimension, std::tuple<Dimensions...>>;
            check_index(index, Current::left, Current::right, path_);
            const auto linear = static_cast<std::int32_t>(
                linear_index_ * Current::size + (index - Current::low));
            if constexpr (NextDimension + 1 == rank) {
                return Element{id_, path_, linear};
            } else {
                return Selection<NextDimension + 1>{id_, path_, linear};
            }
        }

        [[nodiscard]] auto at(std::int32_t index) const {
            return (*this)[index];
        }

        std::uint32_t id_ = 0;
        const char* path_ = "";
        std::int32_t linear_index_ = 0;
    };

   public:
    std::uint32_t id = 0;
    const char* path = "";

    [[nodiscard]] auto operator[](std::int32_t index) const {
        using First = std::tuple_element_t<0, std::tuple<Dimensions...>>;
        check_index(index, First::left, First::right, path);
        return Selection<1>{id, path, index - First::low};
    }

    [[nodiscard]] auto at(std::int32_t index) const {
        return (*this)[index];
    }

   private:
    static void check_index(std::int32_t index, std::int32_t left,
                            std::int32_t right, const char* path) {
        const auto low = left < right ? left : right;
        const auto high = left < right ? right : left;
        if (index >= low && index <= high) return;
        std::fprintf(stderr,
                     "cpptb: hierarchy memory '%s' index %d is out of "
                     "bounds [%d:%d]\n",
                     path, index, left, right);
        std::abort();
    }
};

}  // namespace cpptb::hierarchy
