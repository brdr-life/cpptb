#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "cpptb/packed_bits.hpp"

namespace cpptb {

enum class LogicState : std::uint8_t {
    Zero,
    One,
    X,
    Z,
};

namespace logic_detail {

[[noreturn]] inline void fail(const char* message) {
    std::fprintf(stderr, "cpptb::LogicBits error: %s\n", message);
    std::abort();
}

}  // namespace logic_detail

template <std::size_t W>
class LogicBits {
    static_assert(W > 0, "cpptb::LogicBits width must be greater than zero");

   public:
    static constexpr std::size_t width = W;
    static constexpr std::size_t word_count = Bits<W>::word_count;
    using plane_type = Bits<W>;

    constexpr LogicBits() = default;

    static constexpr LogicBits from_uint(std::uint64_t value) {
        return from_bits(Bits<W>::from_uint(value));
    }

    static constexpr LogicBits from_bits(Bits<W> value) {
        return LogicBits{value, {}};
    }

    static constexpr LogicBits from_planes(Bits<W> aval, Bits<W> bval) {
        return LogicBits{aval, bval};
    }

    static constexpr LogicBits from_string(std::string_view text) {
        std::size_t digits = 0;
        for (const char character : text) {
            if (character != '_') ++digits;
        }
        if (digits != W) {
            logic_detail::fail("binary text must contain exactly width digits");
        }

        Bits<W> aval;
        Bits<W> bval;
        std::size_t position = 0;
        for (std::size_t cursor = text.size(); cursor > 0;) {
            const char character = text[--cursor];
            if (character == '_') continue;
            switch (character) {
                case '0':
                    break;
                case '1':
                    aval.set_bit(position, true);
                    break;
                case 'x':
                case 'X':
                    aval.set_bit(position, true);
                    bval.set_bit(position, true);
                    break;
                case 'z':
                case 'Z':
                    bval.set_bit(position, true);
                    break;
                default:
                    logic_detail::fail(
                        "binary text accepts only 0, 1, X, Z, and underscore");
            }
            ++position;
        }
        return from_planes(aval, bval);
    }

    template <typename DpiWord>
        requires requires(const DpiWord& word) {
            word.aval;
            word.bval;
        }
    static constexpr LogicBits from_dpi_words(const DpiWord* words) {
        typename Bits<W>::word_array aval{};
        typename Bits<W>::word_array bval{};
        for (std::size_t index = 0; index < word_count; ++index) {
            aval[index] = static_cast<std::uint32_t>(words[index].aval);
            bval[index] = static_cast<std::uint32_t>(words[index].bval);
        }
        return from_planes(Bits<W>::from_words(aval),
                           Bits<W>::from_words(bval));
    }

    template <typename DpiWord>
        requires requires(DpiWord& word) {
            word.aval;
            word.bval;
        }
    [[nodiscard]] constexpr std::array<DpiWord, word_count> dpi_words() const {
        std::array<DpiWord, word_count> result{};
        for (std::size_t index = 0; index < word_count; ++index) {
            result[index].aval = aval_.word(index);
            result[index].bval = bval_.word(index);
        }
        return result;
    }

    [[nodiscard]] constexpr const Bits<W>& aval() const { return aval_; }
    [[nodiscard]] constexpr const Bits<W>& bval() const { return bval_; }
    [[nodiscard]] constexpr const Bits<W>& value_bits() const { return aval_; }
    [[nodiscard]] constexpr const Bits<W>& unknown_bits() const {
        return bval_;
    }

    [[nodiscard]] constexpr LogicState state(std::size_t index) const {
        const bool a = aval_.bit(index);
        const bool b = bval_.bit(index);
        if (!b) return a ? LogicState::One : LogicState::Zero;
        return a ? LogicState::X : LogicState::Z;
    }

    [[nodiscard]] constexpr bool is_known() const {
        for (std::size_t index = 0; index < word_count; ++index) {
            if (bval_.word(index) != 0) return false;
        }
        return true;
    }

    [[nodiscard]] constexpr bool contains_x() const {
        for (std::size_t index = 0; index < W; ++index) {
            if (state(index) == LogicState::X) return true;
        }
        return false;
    }

    [[nodiscard]] constexpr bool contains_z() const {
        for (std::size_t index = 0; index < W; ++index) {
            if (state(index) == LogicState::Z) return true;
        }
        return false;
    }

    [[nodiscard]] constexpr Bits<W> to_bits() const {
        if (!is_known()) {
            logic_detail::fail("cannot convert X/Z value to two-state Bits");
        }
        return aval_;
    }

    friend constexpr bool operator==(const LogicBits&, const LogicBits&) =
        default;

   private:
    constexpr LogicBits(Bits<W> aval, Bits<W> bval)
        : aval_(aval), bval_(bval) {}

    Bits<W> aval_{};
    Bits<W> bval_{};
};

}  // namespace cpptb
