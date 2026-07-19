# Rich data

This clockless example is inferred directly from its SystemVerilog source. It
shows how generated types handle a 137-bit port, arbitrary slices, Q2.14 fixed
point, unpacked and multidimensional arrays, and nested packed struct/enum
fields.

```sh
make cpp-dpi-rich-data-run
make cpp-dpi-rich-data-sv-run
```

## Wide values and slices

`Bits<N>` stores arbitrary packed widths in least-significant-word-first
order. Slices are explicit and retain their width in the C++ type:

```cpp
auto wide = Bits<137>::from_words(
    {0xc001'd00du, 0xdead'beefu, 0x89ab'cdefu, 0x0123'4567u,
     0x0000'01abu});
wide.set_slice<16>(48, Bits<16>::from_uint(0xbeef));
dut.wide_i.set(wide);

co_await Delay{1_ps};
const auto slice = dut.wide_o.get().slice<16>(48);
test.expect_eq("arbitrary output slice", slice,
               expected_wide.slice<16>(48));
```

## Fixed point

The fixed-point helper separates representation, rounding, and overflow
policy. DUT ports still carry the raw packed value:

```cpp
using Q2_14 = Fixed<16, 2, Signedness::Signed>;
const auto a = Q2_14::from_raw(0x6000);
const auto b = Q2_14::from_raw(0x2000);
dut.fixed_a_i.set(a.raw().to_uint());
dut.fixed_b_i.set(b.raw().to_uint());

const auto expected = quantize<Q2_14>(
    mul_full(a, b), Round::NearestEven, Overflow::Saturate);
co_await Delay{1_ps};
test.expect_eq("Q2.14 multiply", dut.fixed_y_o.get(),
               expected.raw().to_uint());
```

## Arrays and generated views

Each declared unpacked dimension contributes an `[index]` level, preserving
the source bounds including descending and negative ranges:

```cpp
dut.scalar_array_i[2].set(0x1020'3042u);
dut.matrix_i[2][-1].set(
    Bits<65>::from_words({0x1020'3043u, 0x5060'7083u, 1u}));
```

Packed structs and enums receive generated value and view types:

```cpp
auto packet = PacketTValue::from_signal_value(0);
packet.set_opcode(Bits<3>::from_uint(5)).set_mode(ModeT::ModeRun);
packet.view().inner()
    .set_tag(Bits<2>::from_uint(1))
    .set_payload(Bits<3>::from_uint(2));
dut.packet_i.set(packet.signal_value());

co_await Delay{1_ps};
const auto actual = PacketTValue::from_signal_value(dut.packet_o.get());
test.expect("signed enum", actual.mode().is(ModeT::ModeRun));
test.expect_eq("nested payload", actual.inner().payload().to_uint(), 7u);
```

The test batches all independent writes before settling once. Its C++ and
pure-SV forms each perform 17 checks and advance no clock cycles. Dedicated
authoring benchmarks (`wide_echo_137`, `wide_slice`, `fixed_mac`,
`array_multidim`, and `packed_view`) provide longer apples-to-apples
performance measurements for these same constructs.
