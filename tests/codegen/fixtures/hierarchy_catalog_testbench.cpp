#include "cpptb/cpptb.hpp"
#include "hierarchy_catalog_dut.hpp"

#include <array>
#include <span>
#include <type_traits>
#include <utility>

namespace {

using cpptb::coro::RisingEdge;
using cpptb::coro::Task;
using cpptb::generated::hierarchy_catalog::Dut;
using cpptb::generated::hierarchy_catalog::PacketTValue;
using cpptb::generated::hierarchy_catalog::StateT;
using cpptb::generated::hierarchy_catalog::StateTValue;
using namespace cpptb::coro;

using UFixed4_4 = cpptb::Fixed<8, 4>;

struct PortOnlyDut {
    decltype(Dut::clk) clk;
    decltype(Dut::write_enable) write_enable;
    decltype(Dut::write_data) write_data;
    decltype(Dut::value) value;
    decltype(Dut::event_out) event_out;
};

static_assert(sizeof(Dut) == sizeof(PortOnlyDut));
static_assert(std::is_same_v<
              decltype(std::declval<Dut>().block1.state.get()), StateTValue>);
static_assert(std::is_same_v<
              decltype(std::declval<Dut>()
                           .template cpptb_signal<"block1.storage">()),
              decltype(Dut::block1.storage)>);
static_assert(std::is_same_v<
              decltype(std::declval<Dut>().block1.packet.get()), PacketTValue>);
static_assert(decltype(Dut::block1)::WIDTH == 8);
static_assert(decltype(Dut::block1)::DOUBLE_WIDTH == 16);

Task<void> hierarchy_sequence(Dut dut, cpptb::TestContext& test) {
    dut.clk.set(0);
    dut.write_enable.set(0);
    dut.write_data.set(0);
    test.start_clock(dut.clk, 10_ns);
    co_await RisingEdge{dut.clk};

    dut.block1.storage.deposit(0x12);
    const auto storage_by_path =
        dut.template cpptb_signal<"block1.storage">();
    test.expect_eq("scalar deposit is immediately readable",
                   storage_by_path.get(), 0x12u);
    co_await Delay{1_ps};
    test.expect_eq("scalar deposit reaches output", dut.value.get(), 0x12u);

    dut.block1.storage.force(0x34);
    test.expect_eq("scalar force is immediately readable",
                   dut.block1.storage.get(), 0x34u);
    dut.write_data.set(0x77);
    dut.write_enable.set(1);
    co_await RisingEdge{dut.clk};
    co_await Delay{1_ps};
    test.expect_eq("force overrides RTL assignment", dut.value.get(), 0x34u);
    dut.block1.storage.release();
    co_await RisingEdge{dut.clk};
    co_await Delay{1_ps};
    test.expect_eq("RTL assignment resumes after release", dut.value.get(),
                   0x77u);
    dut.write_enable.set(0);

    test.expect_eq("derived net is readable", dut.block1.inverted.get(),
                   0x88u);
    dut.block1.inverted.force(0x56);
    test.expect_eq("derived net force is immediately readable",
                   dut.block1.inverted.get(), 0x56u);
    dut.block1.inverted.release();

    dut.block1.memory[2].deposit(0xbeef);
    test.expect_eq("non-zero memory index deposit",
                   dut.block1.memory[2].get(), 0xbeefu);
    const std::array<std::uint32_t, 7> memory_values{
        0x1020u, 0x3040u, 0x5060u, 0x7080u,
        0x90a0u, 0xb0c0u, 0xd0e0u};
    std::array<std::uint32_t, 7> memory_readback{};
    dut.block1.memory.deposit(2, std::span<const std::uint32_t>{memory_values});
    dut.block1.memory.get_into(2, std::span<std::uint32_t>{memory_readback});
    for (std::size_t index = 0; index < memory_values.size(); ++index) {
        test.expect_eq("non-zero memory block transfer", memory_readback[index],
                       memory_values[index]);
    }
    dut.block1.memory[2].force(0xcafe);
    test.expect_eq("memory force is immediately readable",
                   dut.block1.memory[2].get(), 0xcafeu);
    dut.block1.memory[2].release();

    dut.block1.matrix[1][3].deposit(0x5a);
    test.expect_eq("multidimensional memory index",
                   dut.block1.matrix[1][3].get(), 0x5au);

    dut.lanes[1].block2.storage.deposit(0x9c);
    test.expect_eq("generated instance hierarchy",
                   dut.lanes[1].block2.storage.get(), 0x9cu);
    test.expect_eq("selected hierarchy parameter",
                   dut.lanes[1].block2.WIDTH, 8);
    const auto selected_fixed =
        UFixed4_4::from_raw(cpptb::Bits<8>::from_uint(0xb4));
    dut.lanes[1].block2.storage.deposit_as(selected_fixed);
    test.expect_eq(
        "selected hierarchy typed view",
        dut.lanes[1].block2.storage.get_as<UFixed4_4>().raw(),
        selected_fixed.raw());
    dut.lanes[1].block2.storage.deposit_logic(
        cpptb::LogicBits<8>::from_uint(0xa5));
    test.expect_eq("selected hierarchy four-state deposit",
                   dut.lanes[1].block2.storage.get_logic(),
                   cpptb::LogicBits<8>::from_uint(0xa5));
    dut.lanes[1].block2.storage.force(0x3d);
    test.expect_eq("selected hierarchy force",
                   dut.lanes[1].block2.storage.get(), 0x3du);
    dut.lanes[1].block2.storage.force_logic(
        cpptb::LogicBits<8>::from_uint(0x5a));
    test.expect_eq("selected hierarchy four-state force",
                   dut.lanes[1].block2.storage.get_logic(),
                   cpptb::LogicBits<8>::from_uint(0x5a));
    dut.lanes[1].block2.storage.release();
    test.expect_eq("selected hierarchy release",
                   dut.lanes[1].block2.storage.get(), 0x5au);

    dut.block1.state.deposit(
        cpptb::generated::hierarchy_catalog::to_value(StateT::Done));
    test.expect("typed enum deposit",
                dut.block1.state.get().is(StateT::Done));
    dut.block1.state.force(
        cpptb::generated::hierarchy_catalog::to_value(StateT::Active));
    test.expect("typed enum force",
                dut.block1.state.get().is(StateT::Active));
    dut.block1.state.release();

    auto packet = PacketTValue{};
    packet.set_tag(cpptb::Bits<4>::from_uint(0xa))
        .set_payload(cpptb::Bits<8>::from_uint(0x5c));
    dut.block1.packet.deposit(packet);
    const auto observed_packet = dut.block1.packet.get();
    test.expect_eq("packed struct tag", observed_packet.tag(),
                   cpptb::Bits<4>::from_uint(0xa));
    test.expect_eq("packed struct payload", observed_packet.payload(),
                   cpptb::Bits<8>::from_uint(0x5c));

    const auto fixed = UFixed4_4::from_raw(cpptb::Bits<8>::from_uint(0xa8));
    dut.block1.storage.deposit_as(fixed);
    test.expect_eq("fixed-point view",
                   dut.block1.storage.get_as<UFixed4_4>().raw(), fixed.raw());

    const auto logic_value = cpptb::LogicBits<4>::from_string("1010");
    dut.four_state_value.deposit_logic(logic_value);
    test.expect_eq("four-state deposit transport",
                   dut.four_state_value.get_logic(), logic_value);
    const auto forced_logic = cpptb::LogicBits<4>::from_string("0101");
    dut.four_state_value.force_logic(forced_logic);
    test.expect_eq("four-state force transport",
                   dut.four_state_value.get_logic(), forced_logic);
    dut.four_state_value.release();

    const auto wide_value = cpptb::Bits<137>::from_hex(
        "1_00000000_00000000_12345678_9abcdef0");
    dut.wide_value.deposit(wide_value);
    test.expect_eq("wide hierarchy deposit", dut.wide_value.get(),
                   wide_value);
    const auto forced_wide = cpptb::Bits<137>::from_hex(
        "0_00000000_00000001_fedcba98_76543210");
    dut.wide_value.force(forced_wide);
    test.expect_eq("wide hierarchy force", dut.wide_value.get(),
                   forced_wide);
    dut.wide_value.release();

    co_await RisingEdge{dut.event_out};
    test.expect_eq("top-level output edge", dut.event_out.get(), 1u);
    co_await RisingEdge{dut.internal_flag};
    test.expect_eq("internal hierarchy edge", dut.internal_flag.get(), 1u);
    co_await FallingEdge{dut.internal_flag};
    test.expect_eq("internal falling edge", dut.internal_flag.get(), 0u);
    co_await Edge{dut.internal_flag};
    test.expect_eq("internal any edge", dut.internal_flag.get(), 1u);
}

CPPTB_REGISTER_TEST(hierarchy_sequence);

}  // namespace
