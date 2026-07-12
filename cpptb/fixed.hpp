#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "cpptb/packed_bits.hpp"

namespace cpptb {

enum class Signedness { Unsigned, Signed };
enum class Round { Truncate, TowardZero = Truncate, NearestEven };
enum class Overflow { Wrap, Saturate, Trap };

namespace fixed_detail {

constexpr uint128_t width_mask(std::size_t width) {
    return width == 128 ? ~uint128_t{0} : (uint128_t{1} << width) - 1;
}

constexpr uint128_t unsigned_limit(std::size_t width) {
    return width_mask(width);
}

constexpr uint128_t signed_positive_limit(std::size_t width) {
    return (uint128_t{1} << (width - 1)) - 1;
}

constexpr uint128_t signed_negative_limit(std::size_t width) {
    return uint128_t{1} << (width - 1);
}

constexpr std::uint64_t magnitude(std::int64_t value) {
    if (value >= 0) return static_cast<std::uint64_t>(value);
    return static_cast<std::uint64_t>(-(value + 1)) + 1;
}

constexpr std::uint64_t gcd(std::uint64_t left, std::uint64_t right) {
    while (right != 0) {
        const std::uint64_t remainder = left % right;
        left = right;
        right = remainder;
    }
    return left;
}

struct SignedMagnitude {
    bool negative;
    uint128_t magnitude;
};

template <std::size_t W>
constexpr Bits<W> scalar_bits(uint128_t raw, bool negative) {
    typename Bits<W>::word_array words;
    words.fill(negative ? 0xffffffffU : 0U);
    constexpr std::size_t copied_words = Bits<W>::word_count < 4
                                             ? Bits<W>::word_count
                                             : 4;
    for (std::size_t index = 0; index < copied_words; ++index) {
        words[index] = static_cast<std::uint32_t>(raw);
        raw >>= 32;
    }
    return Bits<W>::from_words(words);
}

template <typename FixedValue>
constexpr SignedMagnitude decompose(const FixedValue& value) {
    constexpr std::size_t width = FixedValue::width;
    const uint128_t raw = value.raw().to_uint128();
    if constexpr (FixedValue::signedness == Signedness::Signed) {
        const uint128_t sign = uint128_t{1} << (width - 1);
        if ((raw & sign) != 0) {
            return {true, ((~raw) + 1) & width_mask(width)};
        }
    }
    return {false, raw};
}

template <std::size_t W>
constexpr Bits<W> encode(bool negative, uint128_t magnitude_value) {
    const uint128_t raw = negative ? uint128_t{0} - magnitude_value
                                   : magnitude_value;
    return Bits<W>::from_uint128(raw);
}

[[noreturn]] inline void ratio_fail(const char* reason) {
    std::fprintf(stderr, "cpptb::Fixed from_ratio error: %s\n", reason);
    std::abort();
}

[[noreturn]] inline void overflow_fail() {
    std::fprintf(stderr, "cpptb::Fixed quantize overflow\n");
    std::abort();
}

}  // namespace fixed_detail

template <std::size_t W, int IntegerBits,
          Signedness ValueSignedness = Signedness::Unsigned>
class Fixed {
    static_assert(W > 0, "cpptb::Fixed width must be greater than zero");
    static_assert(IntegerBits >= 0,
                  "cpptb::Fixed integer bits cannot be negative");
    static_assert(static_cast<std::size_t>(IntegerBits) <= W,
                  "cpptb::Fixed integer bits cannot exceed its width");

public:
    static constexpr std::size_t width = W;
    static constexpr int integer_bits = IntegerBits;
    static constexpr std::size_t fractional_bits = W - IntegerBits;
    static constexpr Signedness signedness = ValueSignedness;
    using raw_type = Bits<W>;
    using scaled_type =
        std::conditional_t<ValueSignedness == Signedness::Signed, int128_t,
                           uint128_t>;

    constexpr Fixed() = default;

    static constexpr Fixed from_raw(Bits<W> bits) { return Fixed(bits); }

    template <typename Integer>
        requires(std::is_integral_v<Integer> && sizeof(Integer) <= 8)
    static constexpr Fixed from_raw(Integer value) {
        const bool negative = std::is_signed_v<Integer> && value < 0;
        return Fixed(fixed_detail::scalar_bits<W>(
            static_cast<uint128_t>(value), negative));
    }

    static constexpr Fixed from_raw(uint128_t value) {
        return Fixed(fixed_detail::scalar_bits<W>(value, false));
    }

    static constexpr Fixed from_raw(int128_t value) {
        return Fixed(fixed_detail::scalar_bits<W>(
            static_cast<uint128_t>(value), value < 0));
    }

    [[nodiscard]] constexpr const Bits<W>& raw() const { return bits_; }

    [[nodiscard]] constexpr scaled_type scaled() const requires(W <= 128) {
        if constexpr (ValueSignedness == Signedness::Unsigned) {
            return bits_.to_uint128();
        } else {
            const auto value = fixed_detail::decompose(*this);
            if (!value.negative) return static_cast<int128_t>(value.magnitude);
            if (value.magnitude == (uint128_t{1} << 127)) {
                const int128_t half_min = -(int128_t{1} << 126);
                return half_min + half_min;
            }
            return -static_cast<int128_t>(value.magnitude);
        }
    }

    static constexpr Fixed from_ratio(std::int64_t numerator,
                                      std::uint64_t denominator)
        requires(W <= 128)
    {
        if (denominator == 0) fixed_detail::ratio_fail("zero denominator");

        bool negative = numerator < 0;
        std::uint64_t numerator_magnitude = fixed_detail::magnitude(numerator);
        const std::uint64_t divisor =
            fixed_detail::gcd(numerator_magnitude, denominator);
        numerator_magnitude /= divisor;
        denominator /= divisor;

        std::size_t denominator_power = 0;
        while ((denominator & 1U) == 0) {
            denominator >>= 1;
            ++denominator_power;
        }
        if (denominator != 1 || denominator_power > fractional_bits) {
            fixed_detail::ratio_fail("ratio has no exact representation");
        }

        const std::size_t shift = fractional_bits - denominator_power;
        uint128_t scaled_magnitude = numerator_magnitude;
        if (scaled_magnitude != 0 &&
            (shift >= 128 ||
             scaled_magnitude > (~uint128_t{0} >> shift))) {
            fixed_detail::ratio_fail("ratio is outside the supported range");
        }
        scaled_magnitude =
            shift >= 128 ? 0 : scaled_magnitude << shift;
        if (scaled_magnitude == 0) negative = false;

        bool representable = false;
        if constexpr (ValueSignedness == Signedness::Signed) {
            const uint128_t limit =
                negative ? fixed_detail::signed_negative_limit(W)
                         : fixed_detail::signed_positive_limit(W);
            representable = scaled_magnitude <= limit;
        } else {
            representable = !negative &&
                            scaled_magnitude <= fixed_detail::unsigned_limit(W);
        }
        if (!representable) {
            fixed_detail::ratio_fail("ratio is not representable");
        }
        return Fixed(fixed_detail::encode<W>(negative, scaled_magnitude));
    }

    friend constexpr bool operator==(const Fixed&, const Fixed&) = default;

private:
    constexpr explicit Fixed(Bits<W> bits) : bits_(bits) {}

    Bits<W> bits_{};
};

template <std::size_t LeftW, int LeftInteger, Signedness LeftSignedness,
          std::size_t RightW, int RightInteger, Signedness RightSignedness>
[[nodiscard]] constexpr auto mul_full(
    const Fixed<LeftW, LeftInteger, LeftSignedness>& left,
    const Fixed<RightW, RightInteger, RightSignedness>& right) {
    static_assert(LeftW <= 128 && RightW <= 128,
                  "mul_full operands are limited to 128 bits");
    static_assert(LeftW + RightW <= 128,
                  "mul_full result is limited to 128 bits");
    constexpr Signedness result_signedness =
        LeftSignedness == Signedness::Signed ||
                RightSignedness == Signedness::Signed
            ? Signedness::Signed
            : Signedness::Unsigned;
    using Result = Fixed<LeftW + RightW, LeftInteger + RightInteger,
                         result_signedness>;

    const auto left_value = fixed_detail::decompose(left);
    const auto right_value = fixed_detail::decompose(right);
    const uint128_t product = left_value.magnitude * right_value.magnitude;
    const bool negative = product != 0 &&
                          (left_value.negative != right_value.negative);
    return Result::from_raw(
        fixed_detail::encode<LeftW + RightW>(negative, product));
}

template <typename Dest, std::size_t SourceW, int SourceInteger,
          Signedness SourceSignedness>
[[nodiscard]] constexpr Dest quantize(
    const Fixed<SourceW, SourceInteger, SourceSignedness>& source,
    Round round = Round::Truncate, Overflow overflow = Overflow::Wrap) {
    static_assert(Dest::width <= 128 && SourceW <= 128,
                  "quantize is limited to 128-bit source and destination values");

    auto value = fixed_detail::decompose(source);
    bool arithmetic_overflow = false;

    if constexpr (SourceW - SourceInteger > Dest::fractional_bits) {
        constexpr std::size_t shift =
            SourceW - SourceInteger - Dest::fractional_bits;
        const uint128_t quotient = shift == 128 ? 0 : value.magnitude >> shift;
        const uint128_t remainder_mask =
            shift == 128 ? ~uint128_t{0} : (uint128_t{1} << shift) - 1;
        const uint128_t remainder = value.magnitude & remainder_mask;
        value.magnitude = quotient;
        if (round == Round::NearestEven) {
            const uint128_t halfway = uint128_t{1} << (shift - 1);
            if (remainder > halfway ||
                (remainder == halfway && (quotient & 1U) != 0)) {
                ++value.magnitude;
            }
        }
    } else if constexpr (SourceW - SourceInteger < Dest::fractional_bits) {
        constexpr std::size_t shift =
            Dest::fractional_bits - (SourceW - SourceInteger);
        if (value.magnitude != 0 &&
            (shift >= 128 || value.magnitude > (~uint128_t{0} >> shift))) {
            arithmetic_overflow = true;
        }
        value.magnitude = shift >= 128 ? 0 : value.magnitude << shift;
    }

    if (value.magnitude == 0) value.negative = false;
    bool representable = !arithmetic_overflow;
    if constexpr (Dest::signedness == Signedness::Signed) {
        const uint128_t limit =
            value.negative
                ? fixed_detail::signed_negative_limit(Dest::width)
                : fixed_detail::signed_positive_limit(Dest::width);
        representable = representable && value.magnitude <= limit;
    } else {
        representable = representable && !value.negative &&
                        value.magnitude <=
                            fixed_detail::unsigned_limit(Dest::width);
    }

    if (representable || overflow == Overflow::Wrap) {
        return Dest::from_raw(
            fixed_detail::encode<Dest::width>(value.negative, value.magnitude));
    }
    if (overflow == Overflow::Trap) fixed_detail::overflow_fail();

    if constexpr (Dest::signedness == Signedness::Signed) {
        const uint128_t saturated =
            value.negative
                ? fixed_detail::signed_negative_limit(Dest::width)
                : fixed_detail::signed_positive_limit(Dest::width);
        return Dest::from_raw(
            fixed_detail::encode<Dest::width>(value.negative, saturated));
    } else {
        const uint128_t saturated =
            value.negative ? 0 : fixed_detail::unsigned_limit(Dest::width);
        return Dest::from_raw(Bits<Dest::width>::from_uint128(saturated));
    }
}

}  // namespace cpptb
