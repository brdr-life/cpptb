#include <array>
#include <cstdint>

#include "cpptb/cpptb.hpp"
#include "examples/rich_data/generated/rich_data_dut.hpp"

namespace cpptb::examples::rich_data {
namespace {

using cpptb::generated::rich_data::Dut;
using cpptb::generated::rich_data::ModeT;
using cpptb::generated::rich_data::PacketTValue;
using coro::Delay;
using coro::Task;
using namespace coro;

template <size_t Width>
Bits<Width> xor_words(Bits<Width> value,
                      const std::array<uint32_t, Bits<Width>::word_count>& mask) {
    for (size_t word = 0; word < Bits<Width>::word_count; ++word) {
        value.set_word(word, value.word(word) ^ mask[word]);
    }
    return value;
}

Task<void> rich_data_sequence(Dut dut, TestContext& test) {
    auto wide = Bits<137>::from_words(
        {0xc001'd00du, 0xdead'beefu, 0x89ab'cdefu, 0x0123'4567u,
         0x0000'01abu});
    wide.set_slice<16>(48, Bits<16>::from_uint(0xbeef));
    dut.wide_i.set(wide);

    using Q2_14 = Fixed<16, 2, Signedness::Signed>;
    const auto fixed_a = Q2_14::from_raw(0x6000);
    const auto fixed_b = Q2_14::from_raw(0x2000);
    dut.fixed_a_i.set(fixed_a.raw().to_uint());
    dut.fixed_b_i.set(fixed_b.raw().to_uint());

    for (int32_t index = 1; index <= 3; ++index) {
        dut.scalar_array_i.at(index).set(
            0x1020'3040u + static_cast<uint32_t>(index));
    }

    for (int32_t row = 2; row >= 1; --row) {
        for (int32_t column = -1; column <= 1; ++column) {
            const uint32_t ordinal =
                static_cast<uint32_t>((row - 1) * 3 + column + 1);
            dut.matrix_i.at(row).at(column).set(Bits<65>::from_words(
                {0x1020'3040u + ordinal, 0x5060'7080u + ordinal,
                 ordinal & 1u}));
        }
    }

    auto packet = PacketTValue::from_signal_value(0);
    packet.set_opcode(Bits<3>::from_uint(5)).set_mode(ModeT::ModeRun);
    packet.view().inner()
        .set_tag(Bits<2>::from_uint(1))
        .set_payload(Bits<3>::from_uint(2));
    dut.packet_i.set(packet.signal_value());

    co_await Delay{1_ps};

    const auto expected_wide = xor_words<137>(
        wide, {0x0f0f'0f0fu, 0x3333'3333u, 0x5555'aaaau,
               0x0123'4567u, 0x0000'0155u});
    test.expect_eq("137-bit value", dut.wide_o.get(), expected_wide);
    test.expect_eq("arbitrary output slice",
                   dut.wide_o.get().slice<16>(48),
                   expected_wide.slice<16>(48));

    const auto fixed_expected = quantize<Q2_14>(
        mul_full(fixed_a, fixed_b), Round::NearestEven, Overflow::Saturate);
    test.expect_eq("Q2.14 multiply", dut.fixed_y_o.get(),
                   fixed_expected.raw().to_uint());

    for (int32_t index = 1; index <= 3; ++index) {
        const uint32_t input =
            0x1020'3040u + static_cast<uint32_t>(index);
        test.expect_eq("unpacked array element",
                       dut.scalar_array_o.at(index).get(),
                       input ^ (0x0101'0101u * static_cast<uint32_t>(index)));
    }

    const std::array<uint32_t, 3> matrix_mask = {
        0x89ab'cdefu, 0x0123'4567u, 1u};
    for (int32_t row = 2; row >= 1; --row) {
        for (int32_t column = -1; column <= 1; ++column) {
            const uint32_t ordinal =
                static_cast<uint32_t>((row - 1) * 3 + column + 1);
            const auto input = Bits<65>::from_words(
                {0x1020'3040u + ordinal, 0x5060'7080u + ordinal,
                 ordinal & 1u});
            test.expect_eq("multidimensional wide array element",
                           dut.matrix_o.at(row).at(column).get(),
                           xor_words<65>(input, matrix_mask));
        }
    }

    const auto actual_packet =
        PacketTValue::from_signal_value(dut.packet_o.get());
    test.expect_eq("packed struct opcode", actual_packet.opcode().to_uint(),
                   6u);
    test.expect("signed enum", actual_packet.mode().is(ModeT::ModeRun));
    test.expect_eq("nested struct tag", actual_packet.inner().tag().to_uint(),
                   2u);
    test.expect_eq("nested struct payload",
                   actual_packet.inner().payload().to_uint(), 7u);

    const auto saturated_a = Q2_14::from_raw(0x7000);
    const auto saturated_b = Q2_14::from_raw(0x7000);
    dut.fixed_a_i.set(saturated_a.raw().to_uint());
    dut.fixed_b_i.set(saturated_b.raw().to_uint());
    co_await Delay{1_ps};
    const auto saturated_expected = quantize<Q2_14>(
        mul_full(saturated_a, saturated_b), Round::NearestEven,
        Overflow::Saturate);
    test.expect_eq("Q2.14 saturation", dut.fixed_y_o.get(),
                   saturated_expected.raw().to_uint());
}

CPPTB_REGISTER_TEST(rich_data_sequence);

}  // namespace
}  // namespace cpptb::examples::rich_data
