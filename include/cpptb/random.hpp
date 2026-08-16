// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "cpptb/packed_bits.hpp"

namespace cpptb {

inline constexpr uint64_t kDefaultRandomSeed = 1;
inline constexpr const char* kRandomAlgorithm = "xoshiro256ss-v1";

template <typename Value>
struct Weighted {
    using value_type = Value;

    Value value;
    uint64_t weight;
};

template <typename Value>
Weighted(Value, uint64_t) -> Weighted<Value>;

template <typename Value>
[[nodiscard]] constexpr auto weighted(Value&& value, uint64_t weight) {
    return Weighted<std::remove_cvref_t<Value>>{
        std::forward<Value>(value), weight};
}

namespace random_detail {

constexpr uint64_t mix_seed(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

constexpr uint64_t splitmix_next(uint64_t& state) noexcept {
    state += 0x9e3779b97f4a7c15ULL;
    uint64_t value = state;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

constexpr uint64_t derive_seed(uint64_t master_seed,
                               uint64_t stream_id) noexcept {
    return mix_seed(master_seed ^
                    (stream_id * 0xd2b74407b1ce6e93ULL));
}

template <std::integral Value>
    requires(!std::same_as<std::remove_cv_t<Value>, bool>)
constexpr std::make_unsigned_t<Value> ordered_unsigned(Value value) noexcept {
    using Unsigned = std::make_unsigned_t<Value>;
    if constexpr (std::is_signed_v<Value>) {
        constexpr Unsigned sign =
            Unsigned{1} << (std::numeric_limits<Unsigned>::digits - 1);
        return std::bit_cast<Unsigned>(value) ^ sign;
    } else {
        return value;
    }
}

template <std::integral Value>
    requires(!std::same_as<std::remove_cv_t<Value>, bool>)
constexpr Value from_ordered_unsigned(
    std::make_unsigned_t<Value> value) noexcept {
    using Unsigned = std::make_unsigned_t<Value>;
    if constexpr (std::is_signed_v<Value>) {
        constexpr Unsigned sign =
            Unsigned{1} << (std::numeric_limits<Unsigned>::digits - 1);
        return std::bit_cast<Value>(value ^ sign);
    } else {
        return value;
    }
}

}  // namespace random_detail

class Random {
   public:
    constexpr Random() noexcept { reseed(kDefaultRandomSeed); }

    explicit constexpr Random(uint64_t seed) noexcept {
        reseed(seed);
    }

    constexpr void reseed(uint64_t seed) noexcept {
        seed_ = seed;
        uint64_t state = seed;
        for (auto& word : state_) {
            word = random_detail::splitmix_next(state);
        }
        if ((state_[0] | state_[1] | state_[2] | state_[3]) == 0) {
            state_[0] = 1;
        }
    }

    [[nodiscard]] constexpr uint64_t seed() const noexcept { return seed_; }

    [[nodiscard]] constexpr uint64_t next_u64() noexcept {
        const uint64_t result = std::rotl(state_[1] * 5, 7) * 9;
        const uint64_t shifted = state_[1] << 17;

        state_[2] ^= state_[0];
        state_[3] ^= state_[1];
        state_[1] ^= state_[2];
        state_[0] ^= state_[3];
        state_[2] ^= shifted;
        state_[3] = std::rotl(state_[3], 45);
        return result;
    }

    template <std::integral Value>
        requires(!std::same_as<std::remove_cv_t<Value>, bool> &&
                 sizeof(Value) <= sizeof(uint64_t))
    [[nodiscard]] constexpr Value randint(Value minimum, Value maximum) {
        using Unsigned = std::make_unsigned_t<Value>;
        const Unsigned low = random_detail::ordered_unsigned(minimum);
        const Unsigned high = random_detail::ordered_unsigned(maximum);
        if (low > high) {
            throw std::invalid_argument(
                "cpptb::Random::randint minimum exceeds maximum");
        }

        const Unsigned span = static_cast<Unsigned>(high - low + Unsigned{1});
        const Unsigned offset = static_cast<Unsigned>(uniform_below(span));
        return random_detail::from_ordered_unsigned<Value>(
            static_cast<Unsigned>(low + offset));
    }

    template <std::size_t Width>
    [[nodiscard]] constexpr Bits<Width> randbits() noexcept {
        typename Bits<Width>::word_array words{};
        for (std::size_t index = 0; index < words.size();) {
            const uint64_t value = next_u64();
            words[index++] = static_cast<uint32_t>(value);
            if (index < words.size()) {
                words[index++] = static_cast<uint32_t>(value >> 32);
            }
        }
        return Bits<Width>::from_words(words);
    }

    template <std::ranges::random_access_range Range>
        requires std::ranges::sized_range<Range>
    [[nodiscard]] constexpr std::ranges::range_value_t<Range> choice(
        Range&& values) {
        const auto size = std::ranges::size(values);
        if (size == 0) {
            throw std::invalid_argument(
                "cpptb::Random::choice requires at least one value");
        }
        const auto index = randint<std::size_t>(0, size - 1);
        return *(std::ranges::begin(values) + index);
    }

    template <std::ranges::forward_range Range>
        requires std::ranges::sized_range<Range>
    [[nodiscard]] constexpr auto weighted_choice(Range&& values) {
        using Entry = std::ranges::range_value_t<Range>;
        using Value = typename Entry::value_type;

        uint64_t total_weight = 0;
        for (const auto& entry : values) {
            if (entry.weight >
                std::numeric_limits<uint64_t>::max() - total_weight) {
                throw std::invalid_argument(
                    "cpptb::Random::weighted_choice total weight overflows "
                    "uint64_t");
            }
            total_weight += entry.weight;
        }
        if (total_weight == 0) {
            throw std::invalid_argument(
                "cpptb::Random::weighted_choice requires a positive weight");
        }

        uint64_t selected = randint<uint64_t>(0, total_weight - 1);
        for (const auto& entry : values) {
            if (selected < entry.weight) return Value{entry.value};
            selected -= entry.weight;
        }
        throw std::logic_error(
            "cpptb::Random::weighted_choice selection fell outside weights");
    }

    template <std::ranges::random_access_range Range>
        requires std::ranges::sized_range<Range> &&
                 std::permutable<std::ranges::iterator_t<Range>>
    constexpr void shuffle(Range&& values) {
        const auto size = std::ranges::size(values);
        for (std::size_t remaining = size; remaining > 1; --remaining) {
            const auto selected = randint<std::size_t>(0, remaining - 1);
            std::ranges::iter_swap(std::ranges::begin(values) + (remaining - 1),
                                   std::ranges::begin(values) + selected);
        }
    }

   private:
    template <std::unsigned_integral Unsigned>
        requires(sizeof(Unsigned) <= sizeof(uint64_t))
    [[nodiscard]] constexpr uint64_t uniform_below(Unsigned bound) noexcept {
        if (bound == 0) return next_u64();
        const uint64_t wide_bound = static_cast<uint64_t>(bound);
        const uint64_t threshold = (0 - wide_bound) % wide_bound;
        for (;;) {
            const uint64_t value = next_u64();
            if (value >= threshold) return value % wide_bound;
        }
    }

    std::array<uint64_t, 4> state_{};
    uint64_t seed_ = kDefaultRandomSeed;
};

}  // namespace cpptb
