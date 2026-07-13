#include <array>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "cpptb/fixed.hpp"
#include "cpptb/packed_bits.hpp"

namespace {

using cpptb::Bits;
using cpptb::Fixed;
using cpptb::Overflow;
using cpptb::Round;
using cpptb::Signedness;

bool expect(const char* label, bool condition) {
    if (condition) return true;
    std::fprintf(stderr, "%s: failed\n", label);
    return false;
}

bool expect_abort(const char* label, void (*trigger)(),
                  const char* expected_message) {
    int stderr_pipe[2];
    if (pipe(stderr_pipe) != 0) {
        std::perror("pipe");
        return false;
    }

    std::fflush(nullptr);
    const pid_t child = fork();
    if (child == 0) {
        close(stderr_pipe[0]);
        if (dup2(stderr_pipe[1], STDERR_FILENO) < 0) _exit(126);
        close(stderr_pipe[1]);
        trigger();
        _exit(0);
    }
    if (child < 0) {
        std::perror("fork");
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        return false;
    }

    close(stderr_pipe[1]);
    std::string stderr_output;
    char buffer[256];
    while (const ssize_t count = read(stderr_pipe[0], buffer, sizeof(buffer))) {
        if (count < 0) {
            std::perror("read");
            close(stderr_pipe[0]);
            return false;
        }
        stderr_output.append(buffer, static_cast<std::size_t>(count));
    }
    close(stderr_pipe[0]);

    int status = 0;
    if (waitpid(child, &status, 0) != child) {
        std::perror("waitpid");
        return false;
    }
    const bool aborted = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
    const bool reported = stderr_output.find(expected_message) != std::string::npos;
    if (aborted && reported) return true;
    std::fprintf(stderr, "%s: status=%d stderr=%s\n", label, status,
                 stderr_output.c_str());
    return false;
}

std::uint32_t next_random(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

template <std::size_t W>
Bits<W> random_bits(std::uint32_t& state) {
    typename Bits<W>::word_array words{};
    for (auto& word : words) word = next_random(state);
    return Bits<W>::from_words(words);
}

template <std::size_t K, std::size_t W>
Bits<K> reference_slice(const Bits<W>& value, std::size_t lsb) {
    Bits<K> result;
    for (std::size_t index = 0; index < K; ++index) {
        result.set_bit(index, value.bit(lsb + index));
    }
    return result;
}

template <std::size_t K, std::size_t W>
Bits<W> reference_set_slice(Bits<W> destination, std::size_t lsb,
                            const Bits<K>& value) {
    for (std::size_t index = 0; index < K; ++index) {
        destination.set_bit(lsb + index, value.bit(index));
    }
    return destination;
}

template <std::size_t W, std::size_t K>
bool check_slice_case(std::size_t lsb, std::uint32_t& state) {
    const auto source = random_bits<W>(state);
    const auto replacement = random_bits<K>(state);
    bool passed = true;
    passed &= expect("word slice matches bit reference",
                     source.template slice<K>(lsb) ==
                         reference_slice<K>(source, lsb));

    auto actual = source;
    actual.template set_slice<K>(lsb, replacement);
    passed &= expect("word set_slice matches bit reference",
                     actual ==
                         reference_set_slice(source, lsb, replacement));
    passed &= expect("word set_slice round trip",
                     actual.template slice<K>(lsb) == replacement);
    return passed;
}

constexpr bool constexpr_slice_contract() {
    auto container = Bits<96>::from_hex("0x01234567_89abcdef_deadbeef");
    const auto replacement = Bits<40>::from_hex("0xab_cde1_2345");
    container.set_slice<40>(28, replacement);
    return container.slice<40>(28) == replacement &&
           container.slice<28>(0) == Bits<28>::from_hex("0xeadbeef") &&
           container.slice<28>(68) == Bits<28>::from_hex("0x0123456");
}

template <std::size_t W>
bool check_width(const char* label) {
    using Value = Bits<W>;
    typename Value::word_array all_ones;
    all_ones.fill(0xffffffffU);
    const Value value = Value::from_words(all_ones);

    bool passed = true;
    constexpr std::size_t remainder = W % 32;
    constexpr std::uint32_t expected_top =
        remainder == 0 ? 0xffffffffU
                       : (std::uint32_t{1} << remainder) - 1;
    passed &= expect(label, value.word(Value::word_count - 1) == expected_top);
    for (std::size_t index = 0; index + 1 < Value::word_count; ++index) {
        passed &= expect(label, value.word(index) == 0xffffffffU);
    }

    Value assigned;
    assigned.set_word(Value::word_count - 1, 0xffffffffU);
    passed &= expect(label,
                     assigned.word(Value::word_count - 1) == expected_top);
    return passed;
}

void trigger_bit_oob() {
    Bits<33> value;
    (void)value.bit(33);
}

void trigger_word_oob() {
    Bits<33> value;
    value.set_word(2, 0);
}

void trigger_slice_oob() {
    Bits<33> value;
    (void)value.slice<2>(32);
}

void trigger_set_slice_oob() {
    Bits<33> value;
    value.set_slice<2>(32, Bits<2>::from_uint(3));
}

void trigger_quantize_trap() {
    using Wide = Fixed<8, 8, Signedness::Signed>;
    using Narrow = Fixed<4, 4, Signedness::Signed>;
    (void)cpptb::quantize<Narrow>(Wide::from_raw(127), Round::Truncate,
                                  Overflow::Trap);
}

}  // namespace

int main() {
    static_assert(Bits<1>::from_hex("1").to_uint() == 1);
    static_assert(Bits<31>::from_hex("0x7fff_ffff").to_uint() ==
                  0x7fffffffU);
    static_assert(Bits<64>::from_hex("0x0123_4567_89ab_cdef").to_uint64() ==
                  0x0123456789abcdefULL);
    static_assert(Fixed<16, 2, Signedness::Signed>::from_ratio(-1, 2)
                      .raw()
                      .to_uint() == 0xe000U);
    static_assert(Fixed<128, 0, Signedness::Unsigned>::from_ratio(0, 1)
                      .raw() == Bits<128>{});
    static_assert(constexpr_slice_contract());

    bool passed = true;
    passed &= check_width<1>("width 1 top mask");
    passed &= check_width<31>("width 31 top mask");
    passed &= check_width<32>("width 32 top mask");
    passed &= check_width<33>("width 33 top mask");
    passed &= check_width<64>("width 64 top mask");
    passed &= check_width<65>("width 65 top mask");
    passed &= check_width<96>("width 96 top mask");
    passed &= check_width<137>("width 137 top mask");

    {
        const auto value = Bits<65>::from_words(
            std::array<std::uint32_t, 3>{0x89abcdefU, 0x01234567U,
                                         0xffffffffU});
        passed &= expect("word order word 0", value.word(0) == 0x89abcdefU);
        passed &= expect("word order word 1", value.word(1) == 0x01234567U);
        passed &= expect("word order masked word 2", value.word(2) == 1U);
        passed &= expect("transport words", value.words()[1] == 0x01234567U);
        passed &= expect("transport data", value.data()[0] == 0x89abcdefU);
    }

    {
        Bits<96> container;
        const auto payload = Bits<40>::from_hex("0xab_cde1_2345");
        container.set_slice<40>(28, payload);
        passed &= expect("cross-word slice round trip",
                         container.slice<40>(28) == payload);
        passed &= expect("cross-word slice low boundary", !container.bit(27));
        passed &= expect("cross-word slice high boundary", !container.bit(68));
    }

    {
        std::uint32_t state = 0x91e10da5U;
        passed &= check_slice_case<32, 32>(0, state);
        passed &= check_slice_case<33, 32>(1, state);
        passed &= check_slice_case<65, 1>(64, state);
        passed &= check_slice_case<65, 32>(33, state);
        passed &= check_slice_case<96, 64>(32, state);
        passed &= check_slice_case<137, 64>(0, state);
        passed &= check_slice_case<137, 64>(1, state);
        passed &= check_slice_case<137, 64>(31, state);
        passed &= check_slice_case<137, 64>(37, state);
        passed &= check_slice_case<137, 64>(73, state);
        passed &= check_slice_case<137, 65>(31, state);
        passed &= check_slice_case<137, 100>(5, state);

        for (std::size_t iteration = 0; iteration < 512; ++iteration) {
            passed &= check_slice_case<137, 64>(
                next_random(state) % (137 - 64 + 1), state);
            passed &= check_slice_case<96, 33>(
                next_random(state) % (96 - 33 + 1), state);
        }
    }

    {
        auto value = Bits<137>::from_hex(
            "0x1_01234567_89abcdef_deadbeef_55aa55aa");
        const auto before = value;
        value.set_slice<137>(0, value);
        passed &= expect("self set_slice preserves value", value == before);
    }

    {
        const auto wide = Bits<137>::from_hex(
            "0x1_00000000_00000000_00000000_00000000_00");
        passed &= expect("137-bit hex top bit", wide.bit(136));
        passed &= expect("137-bit hex top word", wide.word(4) == 0x100U);
        passed &= expect("137-bit hex low word", wide.word(0) == 0U);
    }

    {
        Bits<33> original = Bits<33>::from_uint(7);
        const Bits<33> snapshot = original;
        original.set_bit(32, true);
        original.set_word(0, 9);
        passed &= expect("snapshot retains low word", snapshot.word(0) == 7U);
        passed &= expect("snapshot retains high word", snapshot.word(1) == 0U);
        passed &= expect("value equality distinguishes mutation",
                         snapshot != original);
    }

    {
        using WideSigned = Fixed<137, 137, Signedness::Signed>;
        const auto minus_one = WideSigned::from_raw(-1);
        passed &= expect("wide scalar raw sign extension",
                         minus_one.raw().word(4) == 0x1ffU);
    }

    {
        using U64 = Fixed<64, 64, Signedness::Unsigned>;
        const auto product = cpptb::mul_full(
            U64::from_raw(0xffffffffffffffffULL),
            U64::from_raw(0xffffffffffffffffULL));
        passed &= expect("128-bit product word 0",
                         product.raw().word(0) == 1U);
        passed &= expect("128-bit product word 1",
                         product.raw().word(1) == 0U);
        passed &= expect("128-bit product word 2",
                         product.raw().word(2) == 0xfffffffeU);
        passed &= expect("128-bit product word 3",
                         product.raw().word(3) == 0xffffffffU);

        using Unit = Fixed<1, 1, Signedness::Unsigned>;
        using Fraction = Fixed<128, 0, Signedness::Unsigned>;
        const auto unit = Unit::from_raw(1);
        passed &= expect("128-bit left-rescale wrap",
                         cpptb::quantize<Fraction>(
                             unit, Round::Truncate, Overflow::Wrap)
                                 .raw() == Bits<128>{});
        passed &= expect("128-bit left-rescale saturation",
                         cpptb::quantize<Fraction>(
                             unit, Round::Truncate, Overflow::Saturate)
                                 .raw() ==
                             Bits<128>::from_hex(
                                 "0xffffffffffffffffffffffffffffffff"));
    }

    using Q2_14 = Fixed<16, 2, Signedness::Signed>;
    {
        const auto positive = Q2_14::from_ratio(3, 2);
        const auto negative = Q2_14::from_ratio(-1, 2);
        passed &= expect("exact positive ratio raw",
                         positive.raw().to_uint() == 0x6000U);
        passed &= expect("exact negative ratio raw",
                         negative.raw().to_uint() == 0xe000U);
        passed &= expect("negative scaled representation",
                         negative.scaled() == -8192);

        const auto product = cpptb::mul_full(positive, negative);
        static_assert(decltype(product)::width == 32);
        static_assert(decltype(product)::integer_bits == 4);
        passed &= expect("signed Q2.14 full multiply raw",
                         product.raw().to_uint() == 0xf4000000U);
        passed &= expect("signed Q2.14 full multiply scaled",
                         product.scaled() == -201326592);
        const auto narrowed = cpptb::quantize<Q2_14>(
            product, Round::NearestEven, Overflow::Trap);
        passed &= expect("signed Q2.14 product narrowing",
                         narrowed.raw().to_uint() == 0xd000U);
    }

    {
        using Fine = Fixed<5, 2, Signedness::Signed>;
        using Coarse = Fixed<4, 2, Signedness::Signed>;
        const auto nearest = [](std::uint32_t raw) {
            return cpptb::quantize<Coarse>(Fine::from_raw(raw),
                                           Round::NearestEven,
                                           Overflow::Trap)
                .raw()
                .to_uint();
        };
        passed &= expect("nearest-even positive tie down", nearest(5) == 2U);
        passed &= expect("nearest-even positive tie up", nearest(7) == 4U);
        passed &= expect("nearest-even negative tie up", nearest(27) == 14U);
        passed &= expect("nearest-even negative tie down", nearest(25) == 12U);
        const auto truncated = cpptb::quantize<Coarse>(
            Fine::from_raw(25), Round::TowardZero, Overflow::Trap);
        passed &= expect("negative truncation is toward zero",
                         truncated.raw().to_uint() == 13U);
    }

    {
        using Wide = Fixed<8, 8, Signedness::Signed>;
        using Narrow = Fixed<4, 4, Signedness::Signed>;
        using UnsignedNarrow = Fixed<4, 4, Signedness::Unsigned>;
        const auto high = Wide::from_raw(127);
        const auto low = Wide::from_raw(-9);
        passed &= expect("positive overflow wraps",
                         cpptb::quantize<Narrow>(high, Round::Truncate,
                                                 Overflow::Wrap)
                                 .raw()
                                 .to_uint() == 15U);
        passed &= expect("positive overflow saturates",
                         cpptb::quantize<Narrow>(high, Round::Truncate,
                                                 Overflow::Saturate)
                                 .raw()
                                 .to_uint() == 7U);
        passed &= expect("negative overflow wraps",
                         cpptb::quantize<Narrow>(low, Round::Truncate,
                                                 Overflow::Wrap)
                                 .raw()
                                 .to_uint() == 7U);
        passed &= expect("negative overflow saturates",
                         cpptb::quantize<Narrow>(low, Round::Truncate,
                                                 Overflow::Saturate)
                                 .raw()
                                 .to_uint() == 8U);
        passed &= expect("negative to unsigned saturates to zero",
                         cpptb::quantize<UnsignedNarrow>(
                             Wide::from_raw(-1), Round::Truncate,
                             Overflow::Saturate)
                                 .raw()
                                 .to_uint() == 0U);
    }

    passed &= expect_abort("bit OOB diagnostic", trigger_bit_oob,
                           "bit index 33 is out of bounds");
    passed &= expect_abort("word OOB diagnostic", trigger_word_oob,
                           "set_word index 2 is out of bounds");
    passed &= expect_abort("slice OOB diagnostic", trigger_slice_oob,
                           "Bits slice is out of bounds");
    passed &= expect_abort("set_slice OOB diagnostic", trigger_set_slice_oob,
                           "Bits set_slice is out of bounds");
    passed &= expect_abort("quantize trap diagnostic", trigger_quantize_trap,
                           "quantize overflow");
    return passed ? 0 : 1;
}
