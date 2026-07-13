#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>

namespace cpptb {

using uint128_t = unsigned __int128;
using int128_t = __int128;

namespace packed_detail {

[[noreturn]] inline void fail(const char* message) {
    std::fprintf(stderr, "cpptb packed value error: %s\n", message);
    std::abort();
}

[[noreturn]] inline void fail_index(const char* operation, std::size_t index,
                                    std::size_t limit) {
    std::fprintf(stderr,
                 "cpptb::Bits %s index %zu is out of bounds (limit %zu)\n",
                 operation, index, limit);
    std::abort();
}

constexpr unsigned hex_digit(char value) {
    if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<unsigned>(value - 'A' + 10);
    }
    fail("invalid hexadecimal digit");
}

}  // namespace packed_detail

template <std::size_t W>
class Bits {
    static_assert(W > 0, "cpptb::Bits width must be greater than zero");

public:
    static constexpr std::size_t width = W;
    static constexpr std::size_t word_count = (W + 31) / 32;
    using word_array = std::array<std::uint32_t, word_count>;

    constexpr Bits() = default;

    static constexpr Bits from_uint(std::uint64_t value) {
        Bits result;
        result.words_[0] = static_cast<std::uint32_t>(value);
        if constexpr (word_count > 1) {
            result.words_[1] = static_cast<std::uint32_t>(value >> 32);
        }
        result.mask_padding();
        return result;
    }

    static constexpr Bits from_uint128(uint128_t value) {
        Bits result;
        constexpr std::size_t copied_words = word_count < 4 ? word_count : 4;
        for (std::size_t index = 0; index < copied_words; ++index) {
            result.words_[index] = static_cast<std::uint32_t>(value);
            value >>= 32;
        }
        result.mask_padding();
        return result;
    }

    static constexpr Bits from_words(word_array words) {
        Bits result;
        result.words_ = words;
        result.mask_padding();
        return result;
    }

    static constexpr Bits from_hex(std::string_view text) {
        Bits result;
        std::size_t begin = 0;
        if (text.size() >= 2 && text[0] == '0' &&
            (text[1] == 'x' || text[1] == 'X')) {
            begin = 2;
        }

        bool saw_digit = false;
        std::size_t nibble = 0;
        for (std::size_t cursor = text.size(); cursor > begin;) {
            const char character = text[--cursor];
            if (character == '_') continue;
            const unsigned digit = packed_detail::hex_digit(character);
            saw_digit = true;
            for (unsigned bit = 0; bit < 4; ++bit) {
                if ((digit & (1U << bit)) == 0) continue;
                const std::size_t position = nibble * 4 + bit;
                if (position >= W) {
                    packed_detail::fail("hexadecimal value does not fit Bits width");
                }
                result.words_[position / 32] |=
                    std::uint32_t{1} << (position % 32);
            }
            ++nibble;
        }
        if (!saw_digit) packed_detail::fail("hexadecimal value has no digits");
        result.mask_padding();
        return result;
    }

    [[nodiscard]] constexpr const word_array& words() const { return words_; }

    [[nodiscard]] constexpr const std::uint32_t* data() const {
        return words_.data();
    }

    [[nodiscard]] static constexpr std::size_t size() { return word_count; }

    [[nodiscard]] constexpr std::uint32_t word(std::size_t index) const {
        if (index >= word_count) {
            packed_detail::fail_index("word", index, word_count);
        }
        return words_[index];
    }

    constexpr void set_word(std::size_t index, std::uint32_t value) {
        if (index >= word_count) {
            packed_detail::fail_index("set_word", index, word_count);
        }
        words_[index] = value;
        mask_padding();
    }

    [[nodiscard]] constexpr bool bit(std::size_t index) const {
        if (index >= W) packed_detail::fail_index("bit", index, W);
        return ((words_[index / 32] >> (index % 32)) & 1U) != 0;
    }

    constexpr void set_bit(std::size_t index, bool value) {
        if (index >= W) packed_detail::fail_index("set_bit", index, W);
        const std::uint32_t mask = std::uint32_t{1} << (index % 32);
        if (value) {
            words_[index / 32] |= mask;
        } else {
            words_[index / 32] &= ~mask;
        }
    }

    template <std::size_t K>
    [[nodiscard]] constexpr Bits<K> slice(std::size_t lsb) const {
        static_assert(K > 0, "cpptb::Bits slice width must be greater than zero");
        if (lsb > W || K > W - lsb) {
            packed_detail::fail("Bits slice is out of bounds");
        }

        typename Bits<K>::word_array result_words{};
        const std::size_t source_word = lsb / 32;
        const std::size_t source_shift = lsb % 32;
        for (std::size_t index = 0; index < Bits<K>::word_count; ++index) {
            const std::size_t low_index = source_word + index;
            std::uint32_t value = words_[low_index] >> source_shift;
            if (source_shift != 0 && low_index + 1 < word_count) {
                value |= words_[low_index + 1] << (32 - source_shift);
            }
            result_words[index] = value;
        }
        return Bits<K>::from_words(result_words);
    }

    template <std::size_t K>
    constexpr void set_slice(std::size_t lsb, const Bits<K>& value) {
        static_assert(K > 0, "cpptb::Bits slice width must be greater than zero");
        if (lsb > W || K > W - lsb) {
            packed_detail::fail("Bits set_slice is out of bounds");
        }

        const typename Bits<K>::word_array source_words = value.words();
        const std::size_t slice_end = lsb + K;
        const std::size_t first_destination_word = lsb / 32;
        const std::size_t last_destination_word = (slice_end - 1) / 32;

        for (std::size_t destination_word = first_destination_word;
             destination_word <= last_destination_word; ++destination_word) {
            const std::size_t destination_begin = destination_word * 32;
            const std::size_t overlap_begin =
                lsb > destination_begin ? lsb : destination_begin;
            const std::size_t destination_end = destination_begin + 32;
            const std::size_t overlap_end =
                slice_end < destination_end ? slice_end : destination_end;
            const std::size_t destination_shift =
                overlap_begin - destination_begin;
            const std::size_t copied_bits = overlap_end - overlap_begin;

            const std::size_t source_offset = overlap_begin - lsb;
            const std::size_t source_word = source_offset / 32;
            const std::size_t source_shift = source_offset % 32;
            std::uint32_t fragment = source_words[source_word] >> source_shift;
            if (source_shift != 0 && source_word + 1 < Bits<K>::word_count) {
                fragment |= source_words[source_word + 1]
                            << (32 - source_shift);
            }

            const std::uint32_t copied_mask = low_mask(copied_bits);
            const std::uint32_t destination_mask =
                copied_mask << destination_shift;
            words_[destination_word] =
                (words_[destination_word] & ~destination_mask) |
                ((fragment & copied_mask) << destination_shift);
        }
    }

    [[nodiscard]] constexpr unsigned to_uint() const
        requires(W <= std::numeric_limits<unsigned>::digits)
    {
        return static_cast<unsigned>(words_[0]);
    }

    [[nodiscard]] constexpr std::uint64_t to_uint64() const requires(W <= 64) {
        std::uint64_t result = words_[0];
        if constexpr (word_count > 1) {
            result |= static_cast<std::uint64_t>(words_[1]) << 32;
        }
        return result;
    }

    [[nodiscard]] constexpr uint128_t to_uint128() const requires(W <= 128) {
        uint128_t result = 0;
        for (std::size_t index = word_count; index > 0; --index) {
            result = (result << 32) | words_[index - 1];
        }
        return result;
    }

    friend constexpr bool operator==(const Bits&, const Bits&) = default;

private:
    static constexpr std::uint32_t low_mask(std::size_t bit_count) {
        return bit_count == 32
                   ? std::uint32_t{0xffffffffU}
                   : (std::uint32_t{1} << bit_count) - 1;
    }

    static constexpr std::uint32_t top_mask() {
        if constexpr ((W % 32) == 0) return std::uint32_t{0xffffffffU};
        return (std::uint32_t{1} << (W % 32)) - 1;
    }

    // Every constructor and mutator preserves zero padding above bit W - 1.
    constexpr void mask_padding() { words_.back() &= top_mask(); }

    word_array words_{};
};

}  // namespace cpptb
