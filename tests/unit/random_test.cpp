#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>

#include "cpptb/random.hpp"

namespace {

bool expect(const char* label, uint64_t actual, uint64_t expected) {
    if (actual == expected) return true;
    std::fprintf(stderr, "%s: actual=%llu expected=%llu\n", label,
                 static_cast<unsigned long long>(actual),
                 static_cast<unsigned long long>(expected));
    return false;
}

}  // namespace

int main() {
    bool passed = true;

    cpptb::Random first{42};
    cpptb::Random replay{42};
    constexpr std::array<uint64_t, 4> expected_sequence{
        0x15780b2e0c2ec716ULL,
        0x6104d9866d113a7eULL,
        0xae17533239e499a1ULL,
        0xecb8ad4703b360a1ULL,
    };
    for (const auto expected : expected_sequence) {
        const auto actual = first.next_u64();
        passed &= expect("stable xoshiro sequence", actual, expected);
        passed &= expect("same seed replays", replay.next_u64(), actual);
    }

    cpptb::Random ranges{7};
    for (uint32_t iteration = 0; iteration < 10'000; ++iteration) {
        const auto signed_value = ranges.randint<int32_t>(-17, 23);
        passed &= signed_value >= -17 && signed_value <= 23;
        const auto unsigned_value = ranges.randint<uint64_t>(10, 19);
        passed &= unsigned_value >= 10 && unsigned_value <= 19;
    }
    static_cast<void>(ranges.randint<int64_t>(
        std::numeric_limits<int64_t>::min(),
        std::numeric_limits<int64_t>::max()));
    static_cast<void>(ranges.randint<uint64_t>(
        0, std::numeric_limits<uint64_t>::max()));

    bool invalid_range_threw = false;
    try {
        static_cast<void>(ranges.randint(9, 3));
    } catch (const std::invalid_argument&) {
        invalid_range_threw = true;
    }
    passed &= expect("invalid range throws", invalid_range_threw, true);

    cpptb::Random selection{99};
    constexpr std::array<uint32_t, 4> choices{3, 5, 8, 13};
    for (uint32_t iteration = 0; iteration < 100; ++iteration) {
        const auto value = selection.choice(choices);
        passed &= value == 3 || value == 5 || value == 8 || value == 13;
    }
    bool empty_choice_threw = false;
    try {
        static_cast<void>(selection.choice(std::array<uint32_t, 0>{}));
    } catch (const std::invalid_argument&) {
        empty_choice_threw = true;
    }
    passed &= expect("empty choice throws", empty_choice_threw, true);

    constexpr std::array weighted_choices{
        cpptb::weighted(3u, 1), cpptb::weighted(5u, 2),
        cpptb::weighted(8u, 3), cpptb::weighted(13u, 4)};
    cpptb::Random weighted_selection{99};
    for (uint32_t iteration = 0; iteration < 100; ++iteration) {
        const auto value =
            weighted_selection.weighted_choice(weighted_choices);
        passed &= value == 3 || value == 5 || value == 8 || value == 13;
    }
    constexpr std::array only_one_positive{
        cpptb::weighted(3u, 0), cpptb::weighted(5u, 7),
        cpptb::weighted(8u, 0)};
    passed &= expect("zero-weight entries are not selected",
                     weighted_selection.weighted_choice(only_one_positive), 5);

    bool zero_weight_threw = false;
    try {
        static_cast<void>(weighted_selection.weighted_choice(std::array{
            cpptb::weighted(3u, 0), cpptb::weighted(5u, 0)}));
    } catch (const std::invalid_argument&) {
        zero_weight_threw = true;
    }
    passed &= expect("all-zero weights throw", zero_weight_threw, true);

    bool weight_overflow_threw = false;
    try {
        static_cast<void>(weighted_selection.weighted_choice(std::array{
            cpptb::weighted(3u, std::numeric_limits<uint64_t>::max()),
            cpptb::weighted(5u, 1)}));
    } catch (const std::invalid_argument&) {
        weight_overflow_threw = true;
    }
    passed &= expect("weight overflow throws", weight_overflow_threw, true);

    cpptb::Random shuffler{123};
    std::array<uint32_t, 8> order{0, 1, 2, 3, 4, 5, 6, 7};
    shuffler.shuffle(order);
    constexpr std::array<uint32_t, 8> expected_order{4, 3, 7, 2, 0, 5, 6, 1};
    passed &= expect("stable shuffle", order == expected_order, true);

    cpptb::Random bit_source{456};
    const auto bits = bit_source.randbits<137>();
    passed &= expect("randbits masks padding", bits.word(4) >> 9, 0);
    cpptb::Random bit_replay{456};
    passed &= expect("randbits replays", bits == bit_replay.randbits<137>(),
                     true);

    const auto root_seed = cpptb::random_detail::derive_seed(11, 1);
    passed &= expect("stream derivation stable", root_seed,
                     cpptb::random_detail::derive_seed(11, 1));
    passed &= expect("stream derivation separates processes",
                     root_seed != cpptb::random_detail::derive_seed(11, 2),
                     true);

    return passed ? 0 : 1;
}
