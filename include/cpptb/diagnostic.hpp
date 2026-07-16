#pragma once

#include <concepts>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>

#include "cpptb/logic_bits.hpp"
#include "cpptb/packed_bits.hpp"

namespace cpptb {

// Specialize this for user-defined transaction types. Formatting is only
// invoked after a comparison fails.
template <typename Value>
struct DiagnosticFormatter;

namespace detail {

void cpptb_diagnostic_name() = delete;

template <typename Value>
concept HasDiagnosticName = requires(const Value& value) {
    { cpptb_diagnostic_name(value) } -> std::convertible_to<std::string_view>;
};

template <typename Value>
concept HasCustomDiagnosticFormatter = requires(const Value& value) {
    { DiagnosticFormatter<std::remove_cvref_t<Value>>::format(value) }
        -> std::convertible_to<std::string>;
};

template <typename Value>
concept HasRawBits = requires(const Value& value) {
    value.raw_bits();
};

template <typename Value>
struct IsBits : std::false_type {};

template <std::size_t Width>
struct IsBits<Bits<Width>> : std::true_type {};

template <typename Value>
struct IsLogicBits : std::false_type {};

template <std::size_t Width>
struct IsLogicBits<LogicBits<Width>> : std::true_type {};

template <std::size_t Width>
std::string format_bits(const Bits<Width>& value) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    constexpr std::size_t digit_count = (Width + 3) / 4;
    std::string result = std::to_string(Width) + "'h";
    result.reserve(result.size() + digit_count);
    for (std::size_t digit = digit_count; digit > 0; --digit) {
        const std::size_t lsb = (digit - 1) * 4;
        unsigned nibble = 0;
        for (unsigned bit = 0; bit < 4 && lsb + bit < Width; ++bit) {
            if (value.bit(lsb + bit)) nibble |= 1u << bit;
        }
        result.push_back(kHexDigits[nibble]);
    }
    return result;
}

template <std::size_t Width>
std::string format_logic_bits(const LogicBits<Width>& value) {
    std::string result = std::to_string(Width) + "'b";
    result.reserve(result.size() + Width);
    for (std::size_t bit = Width; bit > 0; --bit) {
        switch (value.state(bit - 1)) {
            case LogicState::Zero:
                result.push_back('0');
                break;
            case LogicState::One:
                result.push_back('1');
                break;
            case LogicState::X:
                result.push_back('X');
                break;
            case LogicState::Z:
                result.push_back('Z');
                break;
        }
    }
    return result;
}

template <typename Value>
std::optional<std::string> try_format_diagnostic_value(const Value& value);

template <std::ranges::input_range Range>
std::optional<std::string> format_range(const Range& values) {
    std::string result{"["};
    bool first = true;
    for (const auto& value : values) {
        auto formatted = try_format_diagnostic_value(value);
        if (!formatted) return std::nullopt;
        if (!first) result += ", ";
        first = false;
        result += *formatted;
    }
    result.push_back(']');
    return result;
}

template <typename Value>
std::optional<std::string> try_format_diagnostic_value(const Value& value) {
    using Type = std::remove_cvref_t<Value>;
    if constexpr (HasCustomDiagnosticFormatter<Type>) {
        return std::string{DiagnosticFormatter<Type>::format(value)};
    } else if constexpr (IsBits<Type>::value) {
        return format_bits(value);
    } else if constexpr (IsLogicBits<Type>::value) {
        return format_logic_bits(value);
    } else if constexpr (HasRawBits<Type>) {
        auto formatted = try_format_diagnostic_value(value.raw_bits());
        if (!formatted) return std::nullopt;
        return "raw=" + *formatted;
    } else if constexpr (std::is_same_v<Type, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_enum_v<Type>) {
        if constexpr (HasDiagnosticName<Type>) {
            const std::string_view name = cpptb_diagnostic_name(value);
            if (!name.empty()) return std::string{name};
        }
        return try_format_diagnostic_value(
            static_cast<std::underlying_type_t<Type>>(value));
    } else if constexpr (std::is_integral_v<Type>) {
        char buffer[64]{};
        if constexpr (std::is_signed_v<Type>) {
            std::snprintf(buffer, sizeof(buffer), "%lld",
                          static_cast<long long>(value));
        } else {
            std::snprintf(buffer, sizeof(buffer), "%llu",
                          static_cast<unsigned long long>(value));
        }
        return std::string{buffer};
    } else if constexpr (std::is_floating_point_v<Type>) {
        char buffer[128]{};
        std::snprintf(buffer, sizeof(buffer), "%.17g",
                      static_cast<double>(value));
        return std::string{buffer};
    } else if constexpr (std::convertible_to<const Value&, std::string_view>) {
        return std::string{std::string_view{value}};
    } else if constexpr (std::ranges::input_range<Type>) {
        return format_range(value);
    } else {
        return std::nullopt;
    }
}

}  // namespace detail

template <typename Value>
std::optional<std::string> format_diagnostic(const Value& value) {
    return detail::try_format_diagnostic_value(value);
}

}  // namespace cpptb
