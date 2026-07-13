#include "cpptb/conformance/framework.hpp"

#include <cstdlib>
#include <string_view>
#include <thread>
#include <utility>

namespace cpptb::conformance {
namespace {

using coro::Edge;
using coro::Delay;
using coro::FallingEdge;
using coro::First;
using coro::Join;
using coro::RisingEdge;
using coro::Signal;
using coro::Task;
using namespace coro;

template <size_t Width, size_t WordCount>
Bits<Width> xor_words(Bits<Width> value,
                      const std::array<uint32_t, WordCount>& mask) {
    static_assert(WordCount == Bits<Width>::word_count);
    for (size_t word = 0; word < WordCount; ++word) {
        value.set_word(word, value.word(word) ^ mask[word]);
    }
    return value;
}

Task<void> packed_signal_contract(ConformanceTb tb) {
    const auto packed65 = Bits<65>::from_words(
        {0x89ab'cdefu, 0x0123'4567u, 0xffff'ffffu});
    tb.dut.packed65_i.set(packed65);
    tb.expect_true("65-bit driven readback is masked snapshot",
                   tb.dut.packed65_i.get() == packed65);

    const auto superseded137 = Bits<137>::from_words(
        {0x1111'2222u, 0x3333'4444u, 0x5555'6666u, 0x7777'8888u,
         0x0000'0011u});
    tb.dut.packed137_i.set(superseded137);
    tb.expect_true("137-bit on-demand set is immediately readable",
                   tb.dut.packed137_i.get() == superseded137);
    const auto packed137 = Bits<137>::from_words(
        {0x0011'2233u, 0x4455'6677u, 0x8899'aabbu, 0xccdd'eeffu,
         0xffff'ffffu});
    tb.dut.packed137_i.set(packed137);
    tb.expect_true("137-bit driven readback is masked snapshot",
                   tb.dut.packed137_i.get() == packed137);

    co_await Delay{1_ps};
    const auto expected65 = xor_words<65>(
        packed65, std::array<uint32_t, 3>{0x89ab'cdefu, 0x0123'4567u, 1u});
    tb.expect_true("65-bit transformed DUT output",
                   tb.dut.packed65_o.get() == expected65);
    const auto expected137 = xor_words<137>(
        packed137,
        std::array<uint32_t, 5>{0xdead'beefu, 0x89ab'cdefu,
                                0x0123'4567u, 0xaa55'aa55u, 0x155u});
    tb.expect_true("137-bit transformed DUT output",
                   tb.dut.packed137_o.get() == expected137);
}

Task<void> unpacked_array_contract(ConformanceTb tb) {
    constexpr std::array<uint32_t, 3> mask = {
        0x89ab'cdefu, 0x0123'4567u, 0x0000'01ffu};

    for (int32_t index = 4; index <= 7; ++index) {
        Bits<73> value;
        value.set_word(0, 0x1020'3040u + static_cast<uint32_t>(index));
        value.set_word(1, 0x5060'7080u + static_cast<uint32_t>(index));
        value.set_word(2, 0x0000'0100u + static_cast<uint32_t>(index));
        tb.dut.array73_i.at(index).set(value);
        tb.expect_true("73-bit array driven readback",
                       tb.dut.array73_i.at(index).get() == value);
    }

    co_await Delay{1_ps};
    for (int32_t index = 4; index <= 7; ++index) {
        Bits<73> value;
        value.set_word(0, 0x1020'3040u + static_cast<uint32_t>(index));
        value.set_word(1, 0x5060'7080u + static_cast<uint32_t>(index));
        value.set_word(2, 0x0000'0100u + static_cast<uint32_t>(index));
        tb.expect_true("73-bit transformed array output",
                       tb.dut.array73_o.at(index).get() ==
                           xor_words<73>(value, mask));
    }
}

Task<void> multidimensional_array_contract(ConformanceTb tb) {
    const auto mask = Bits<65>::from_words(
        {0x89ab'cdefu, 0x0123'4567u, 1u});

    for (int32_t row = 1; row <= 2; ++row) {
        for (int32_t column = -1; column <= 1; ++column) {
            Bits<65> value;
            const uint32_t ordinal =
                static_cast<uint32_t>((row - 1) * 3 + column + 1);
            value.set_word(0, 0x1020'3040u + ordinal);
            value.set_word(1, 0x5060'7080u + ordinal);
            value.set_word(2, ordinal & 1u);
            if (row == 1 && column == -1) {
                const auto superseded = Bits<65>::from_words(
                    {0x1111'2222u, 0x3333'4444u, 1u});
                tb.dut.matrix65_i.at(row).at(column).set(superseded);
                tb.expect_true(
                    "rank-2 on-demand set is immediately readable",
                    tb.dut.matrix65_i.at(row).at(column).get() == superseded);
            }
            tb.dut.matrix65_i.at(row).at(column).set(value);
            tb.expect_true("rank-2 wide array driven readback",
                           tb.dut.matrix65_i.at(row).at(column).get() == value);
        }
    }

    co_await Delay{1_ps};
    for (int32_t row = 1; row <= 2; ++row) {
        for (int32_t column = -1; column <= 1; ++column) {
            Bits<65> value;
            const uint32_t ordinal =
                static_cast<uint32_t>((row - 1) * 3 + column + 1);
            value.set_word(0, 0x1020'3040u + ordinal);
            value.set_word(1, 0x5060'7080u + ordinal);
            value.set_word(2, ordinal & 1u);
            tb.expect_true(
                "rank-2 wide array transformed output",
                tb.dut.matrix65_o.at(row).at(column).get() ==
                    xor_words<65>(value,
                                  std::array<uint32_t, 3>{mask.word(0),
                                                          mask.word(1),
                                                          mask.word(2)}));
        }
    }
}

Task<void> internal_probe_contract(ConformanceTb tb) {
    tb.dut.internal.internal_u64.deposit(0x1111'2222'3333'4444ull);
    tb.dut.internal.internal_u64.deposit(0xfedc'ba98'7654'3210ull);
    tb.expect_true("64-bit internal deposit is immediate",
                   tb.dut.internal.internal_u64.get() ==
                       0xfedc'ba98'7654'3210ull);

    const auto superseded_scalar = Bits<73>::from_words(
        {0x1111'2222u, 0x3333'4444u, 0x0000'0055u});
    const auto scalar = Bits<73>::from_words(
        {0x89ab'cdefu, 0x0123'4567u, 0xffff'ffffu});
    tb.dut.internal.internal_wide.deposit(superseded_scalar);
    tb.dut.internal.internal_wide.deposit(scalar);
    tb.expect_true("wide internal deposit is immediate",
                   tb.dut.internal.internal_wide.get() == scalar);

    tb.dut.internal.internal_memory.at(6).deposit(superseded_scalar);

    for (int32_t index = 7; index >= 4; --index) {
        Bits<73> value;
        value.set_word(0, 0x1020'3040u + static_cast<uint32_t>(index));
        value.set_word(1, 0x5060'7080u + static_cast<uint32_t>(index));
        value.set_word(2, 0xffff'ff00u + static_cast<uint32_t>(index));
        tb.dut.internal.internal_memory.at(index).deposit(value);
    }

    co_await Delay{1_ps};
    tb.expect_true("64-bit internal direct ABI deposit and read",
                   tb.dut.internal.internal_u64.get() ==
                       0xfedc'ba98'7654'3210ull);
    tb.expect_true("internal deposit propagates through combinational logic",
                   tb.dut.internal_comb_fanout.get() ==
                       0xfedc'ba98'7654'3210ull);
    tb.expect_true("same-step internal scalar deposits are last-write-wins",
                   tb.dut.internal.internal_wide.get() == scalar);

    for (int32_t index = 7; index >= 4; --index) {
        Bits<73> expected;
        expected.set_word(0, 0x1020'3040u + static_cast<uint32_t>(index));
        expected.set_word(1, 0x5060'7080u + static_cast<uint32_t>(index));
        expected.set_word(2, 0xffff'ff00u + static_cast<uint32_t>(index));
        tb.expect_true("73-bit descending internal memory deposit and read",
                       tb.dut.internal.internal_memory.at(index).get() ==
                           expected);
    }

    co_await RisingEdge{tb.dut.clock.a};
    co_await Delay{1_ps};
    tb.expect_true("internal deposit propagates through clocked logic",
                   tb.dut.internal_clocked_fanout.get() ==
                       0xfedc'ba98'7654'3210ull);
}

Task<void> force_release_contract(ConformanceTb tb) {
    tb.dut.force_net_source.set(0x12);
    co_await Delay{1_ps};
    tb.expect_eq("force net baseline driver", tb.dut.internal_net_fanout.get(),
                 0x48);

    tb.dut.internal.internal_net.force(0xa5);
    tb.expect_eq("force net immediate readback",
                 tb.dut.internal.internal_net.get(), 0xa5);
    co_await Delay{1_ps};
    tb.expect_eq("force net propagates after explicit delay",
                 tb.dut.internal_net_fanout.get(), 0xa5);

    tb.dut.force_net_source.set(0x34);
    co_await Delay{1_ps};
    tb.expect_eq("force net overrides changing driver",
                 tb.dut.internal_net_fanout.get(), 0xa5);
    tb.dut.internal.internal_net.release();
    co_await Delay{1_ps};
    tb.expect_eq("release net restores resolved driver",
                 tb.dut.internal_net_fanout.get(), 0x6e);

    tb.dut.internal.force_variable_u64.release();
    tb.dut.internal.force_variable_u64.force(0x1111'2222'3333'4444ull);
    tb.dut.internal.force_variable_u64.force(0x0123'4567'89ab'cdefull);
    tb.expect_true("64-bit variable force is immediate",
                   tb.dut.internal.force_variable_u64.get() ==
                       0x0123'4567'89ab'cdefull);
    tb.dut.internal.force_variable_u64.release();
    tb.expect_true("released variable retains forced value",
                   tb.dut.internal.force_variable_u64.get() ==
                       0x0123'4567'89ab'cdefull);

    const auto wide = Bits<73>::from_words(
        {0x89ab'cdefu, 0x0123'4567u, 0xffff'ffffu});
    tb.dut.internal.force_variable_wide.force(wide);
    tb.expect_true("73-bit variable force is masked and immediate",
                   tb.dut.internal.force_variable_wide.get() == wide);
    tb.dut.internal.force_variable_wide.release();
    tb.expect_true("released wide variable retains forced value",
                   tb.dut.internal.force_variable_wide.get() == wide);

    for (const int32_t index : {4, 7}) {
        auto memory = Bits<73>::from_words(
            {0x1020'3040u + static_cast<uint32_t>(index),
             0x5060'7080u + static_cast<uint32_t>(index),
             0xffff'ffffu});
        tb.dut.internal.force_memory.at(index).force(memory);
        tb.expect_true("constant-index memory endpoint force is immediate",
                       tb.dut.internal.force_memory.at(index).get() == memory);
        tb.dut.internal.force_memory.at(index).release();
    }

    tb.dut.internal.force_counter.force(0x55);
    co_await RisingEdge{tb.dut.clock.a};
    co_await RisingEdge{tb.dut.clock.a};
    co_await Delay{1_ps};
    tb.expect_eq("RTL writes do not override force",
                 tb.dut.internal.force_counter.get(), 0x55);
    tb.dut.internal.force_counter.release();
    co_await RisingEdge{tb.dut.clock.a};
    co_await Delay{1_ps};
    tb.expect_eq("RTL write resumes after release",
                 tb.dut.internal.force_counter.get(), 0x56);
}

Task<void> delay_and_settling_contract(ConformanceTb tb) {
    tb.dut.rst_n.set(0);
    tb.dut.derived_gate.set(1);
    tb.dut.stable_signal.set(0);
    tb.dut.drive_value.set(0x10);
    tb.dut.addend.set(0x03);

    co_await Delay{1_ps};
    tb.expect_time("initial delay time", tb.now(), 1_ps);
    tb.expect_eq("initial drive settles during delay", tb.dut.comb_sum.get(),
                 0x13);

    tb.dut.rst_n.set(1);
    tb.dut.drive_value.set(0x20);

    co_await Delay{1_ps};
    tb.expect_time("successive delay time", tb.now(), 2_ps);
    tb.expect_eq("drive value visible after delay",
                 tb.dut.comb_sum.get(), 0x23);

    tb.dut.drive_value.set(0x30);
    tb.expect_eq("driven signal get reflects pending write",
                 tb.dut.drive_value.get(), 0x30);

    co_await RisingEdge{tb.dut.clock.a};
    tb.expect_time("clock A first rising edge time", tb.now(), 2_ns);

    co_await Delay{1_ps};
    tb.expect_time("edge sampling delay", tb.now(), 2_ns + 1_ps);
    tb.expect_eq("edge callback sees settled sequential output",
                 tb.dut.sample.a.get(), 0x30);
    tb.expect_eq("clock A count after first edge", tb.dut.count.a.get(), 1);

    tb.dut.drive_value.set(0x40);
    co_await Delay{1_ps};
    tb.expect_time("edge drive delay", tb.now(), 2_ns + 2_ps);
    tb.expect_eq("edge drive settles during delay", tb.dut.comb_sum.get(),
                 0x43);
    tb.expect_eq("edge drive does not retroactively change sampled value",
                 tb.dut.sample.a.get(), 0x30);
}

Task<void> delay_and_first_contract(ConformanceTb tb) {
    co_await Delay{1_ns};
    tb.expect_time("exact delay deadline", tb.now(), 1_ns);

    const auto timeout_winner =
        co_await First{RisingEdge{tb.dut.stable_signal}, Delay{2_ns}};
    tb.expect_eq("timeout wins First", timeout_winner, 1);
    tb.expect_time("timeout First deadline", tb.now(), 3_ns);

    const auto edge_winner =
        co_await First{RisingEdge{tb.dut.clock.a}, Delay{10_ns}};
    tb.expect_eq("edge wins First", edge_winner, 0);
    tb.expect_time("edge First time", tb.now(), 6_ns);

    co_await Delay{7_ns};
    tb.expect_time("losing First delay is cancelled", tb.now(), 13_ns);

    co_await Delay{1_ps};
    tb.expect_time("picosecond delay", tb.now(), 13_ns + 1_ps);
}

Task<void> edge_contract(ConformanceTb tb) {
    co_await RisingEdge{tb.dut.clock.a};
    tb.expect_time("rising edge", tb.now(), 2_ns);
    co_await FallingEdge{tb.dut.clock.a};
    tb.expect_time("falling edge", tb.now(), 4_ns);
    co_await Edge{tb.dut.clock.a};
    tb.expect_time("any edge rising", tb.now(), 6_ns);
    co_await Edge{tb.dut.clock.a};
    tb.expect_time("any edge falling", tb.now(), 8_ns);
}

Task<void> manual_clock_driver(ConformanceTb tb) {
    tb.dut.clock.manual.set(0);
    co_await Delay{3_ns};
    tb.dut.clock.manual.set(1);
    co_await Delay{1_ns};
    tb.dut.clock.manual.set(0);
    co_await Delay{2_ns};
    tb.dut.clock.manual.set(1);
}

Task<void> manual_clock_observer(ConformanceTb tb) {
    co_await RisingEdge{tb.dut.clock.manual};
    co_await Delay{1_ps};
    tb.expect_time("testbench clock rising time", tb.now(), 3_ns + 1_ps);
    tb.expect_eq("testbench clock samples DUT input", tb.dut.sample.manual.get(),
                 0x40);
    co_await FallingEdge{tb.dut.clock.manual};
    co_await Delay{1_ps};
    tb.expect_time("testbench clock falling time", tb.now(), 4_ns + 1_ps);
    co_await RisingEdge{tb.dut.clock.manual};
    co_await Delay{1_ps};
    tb.expect_time("testbench clock second rising time", tb.now(), 6_ns + 1_ps);
    tb.expect_eq("testbench clock edge count", tb.dut.count.manual.get(), 2);
}

Task<void> derived_clock_observer(ConformanceTb tb) {
    co_await RisingEdge{tb.dut.clock.derived};
    co_await Delay{1_ps};
    tb.expect_time("DUT clock rising time", tb.now(), 2_ns + 1_ps);
    tb.expect_eq("DUT clock samples settled input", tb.dut.sample.derived.get(),
                 0x30);
    co_await FallingEdge{tb.dut.clock.derived};
    co_await Delay{1_ps};
    tb.expect_time("DUT clock falling time", tb.now(), 4_ns + 1_ps);
    co_await RisingEdge{tb.dut.clock.derived};
    co_await Delay{1_ps};
    tb.expect_time("DUT clock second rising time", tb.now(), 6_ns + 1_ps);
    tb.expect_eq("DUT clock edge count", tb.dut.count.derived.get(), 2);
}

Task<void> mark_next_rising(ConformanceTb tb, Signal signal) {
    co_await RisingEdge{signal};
    tb.mark_simultaneous_edge();
}

Task<void> wait_pair(ConformanceTb tb, Signal first, Signal second) {
    co_await Join{mark_next_rising(tb, first), mark_next_rising(tb, second)};
    tb.mark_simultaneous_pair();
}

Task<void> simultaneous_clock_contract(ConformanceTb tb) {
    co_await Delay{5_ns};
    co_await Join{
        wait_pair(tb, tb.dut.clock.a, tb.dut.clock.b),
        wait_pair(tb, tb.dut.clock.manual, tb.dut.clock.derived),
    };
    tb.expect_time("all simultaneous clocks delivered at same simulation time",
                   tb.now(), 6_ns);
    tb.expect_eq("all simultaneous edges resumed", tb.simultaneous_edges(), 4);
    tb.expect_eq("both simultaneous Join pairs completed",
                 tb.simultaneous_pairs(), 2);
}

Task<void> ordered_waiter_first(ConformanceTb tb) {
    co_await RisingEdge{tb.dut.clock.a};
    tb.record_process(1);
}

Task<void> ordered_waiter_second(ConformanceTb tb) {
    co_await RisingEdge{tb.dut.clock.a};
    tb.record_process(2);
}

Task<void> ordered_waiter_verifier(ConformanceTb tb) {
    co_await RisingEdge{tb.dut.clock.a};
    tb.expect_eq("same-trigger waiter count", tb.process_order_count(), 2);
    tb.expect_eq("same-trigger waiter registration order 0", tb.process_order(0),
                 1);
    tb.expect_eq("same-trigger waiter registration order 1", tb.process_order(1),
                 2);
}

Task<void> wait_nested_edge(ConformanceTb tb) {
    co_await RisingEdge{tb.dut.clock.a};
}

Task<void> nested_task_contract(ConformanceTb tb) {
    co_await wait_nested_edge(tb);
    tb.expect_time("nested task continuation time", tb.now(), 2_ns);
}

Task<void> observe_rising_signal(ConformanceTb tb) {
    co_await RisingEdge{tb.dut.event_observed};
    tb.expect_eq("DUT output rising edge value", tb.dut.event_observed.get(), 1);
    tb.expect_time("DUT output rising edge time", tb.now(), 1_ps);
}

Task<void> observe_falling_signal(ConformanceTb tb) {
    co_await FallingEdge{tb.dut.event_observed};
    tb.expect_eq("DUT output falling edge value", tb.dut.event_observed.get(), 0);
    tb.expect_time("DUT output falling edge time", tb.now(), 2_ps);
}

Task<void> observe_any_signal_edge(ConformanceTb tb, SimTime expected_time,
                                   uint32_t expected_value) {
    co_await Edge{tb.dut.event_observed};
    tb.expect_eq("DUT output any-edge value", tb.dut.event_observed.get(),
                 expected_value);
    tb.expect_time("DUT output any-edge time", tb.now(), expected_time);
}

Task<void> signal_observer_contract(ConformanceTb tb) {
    tb.dut.event_drive.set(0);
    co_await Delay{1_ps};

    const auto rising = tb.spawn(observe_rising_signal(tb));
    const auto rising_any =
        tb.spawn(observe_any_signal_edge(tb, 1_ps, 1));
    tb.dut.event_drive.set(1);
    co_await rising;
    co_await rising_any;
    co_await Delay{1_ps};

    const auto falling = tb.spawn(observe_falling_signal(tb));
    const auto falling_any =
        tb.spawn(observe_any_signal_edge(tb, 2_ps, 0));
    tb.dut.event_drive.set(0);
    co_await falling;
    co_await falling_any;
}

Task<void> lifecycle_worker(ConformanceTb tb) {
    co_await Delay{1_ns};
    tb.mark_lifecycle(0x1);
}

Task<void> lifecycle_waiter(ConformanceTb tb, coro::Process process) {
    co_await process;
    tb.mark_lifecycle(0x2);
}

Task<void> cancelled_child(ConformanceTb tb) {
    co_await Delay{20_ns};
    tb.mark_lifecycle(0x8000);
}

Task<void> cancelled_parent(ConformanceTb tb) {
    co_await cancelled_child(tb);
    tb.mark_lifecycle(0x4000);
}

Task<void> lifecycle_join_marker(ConformanceTb tb, SimTime delay,
                                 uint32_t marker) {
    co_await Delay{delay};
    tb.mark_lifecycle(marker);
}

struct ProcessProbe {
    uint32_t markers = 0;
    uint32_t waiter_count = 0;
};

Task<void> probe_worker(ProcessProbe& probe) {
    co_await Delay{1_ns};
    probe.markers |= 0x1;
}

Task<void> probe_waiter(coro::Process process, ProcessProbe& probe,
                        uint32_t marker) {
    co_await process;
    probe.markers |= marker;
    ++probe.waiter_count;
}

Task<void> long_probe_worker(ProcessProbe& probe) {
    co_await Delay{20_ns};
    probe.markers |= 0x80000000;
}

Task<void> process_completion_contract(ConformanceTb tb) {
    ProcessProbe probe;
    const auto worker = tb.spawn(probe_worker(probe));
    const auto first = tb.spawn(probe_waiter(worker, probe, 0x2));
    const auto second = tb.spawn(probe_waiter(worker, probe, 0x4));
    const auto third = tb.spawn(probe_waiter(worker, probe, 0x8));

    co_await third;
    tb.expect_true("multiple completion waiters see worker done", worker.done());
    tb.expect_eq("multiple completion waiters all resume", probe.waiter_count, 3);
    tb.expect_eq("multiple completion waiter markers", probe.markers, 0xf);
    tb.expect_true("first completion waiter done", first.done());
    tb.expect_true("second completion waiter done", second.done());
    tb.expect_true("third completion waiter done", third.done());

    co_await worker;
    probe.markers |= 0x10;
    tb.expect_true("awaiting completed process preserves done status",
                   worker.done());
    tb.expect_eq("completed process late waiter resumes", probe.markers, 0x1f);

    const auto cancelled = tb.spawn(long_probe_worker(probe));
    cancelled.cancel();
    co_await cancelled;
    tb.expect_true("cancelled process is complete", cancelled.done());
    tb.expect_true("cancelled process retains cancelled status",
                   cancelled.cancelled());
    co_await cancelled;
    probe.markers |= 0x20;
    tb.expect_true("awaiting completed cancellation preserves status",
                   cancelled.cancelled());
    tb.expect_eq("cancelled process late waiter resumes", probe.markers, 0x3f);
}

struct CancellationProbe {
    uint32_t markers = 0;
};

Task<void> cancellation_leaf(CancellationProbe& probe, uint32_t marker) {
    co_await Delay{1_ns};
    probe.markers |= marker;
}

Task<void> cancellation_middle(CancellationProbe& probe) {
    co_await cancellation_leaf(probe, 0x1);
    probe.markers |= 0x2;
}

Task<void> cancellation_direct_parent(CancellationProbe& probe) {
    co_await cancellation_middle(probe);
    probe.markers |= 0x4;
}

Task<void> cancellation_join_parent(CancellationProbe& probe) {
    co_await Join{
        cancellation_leaf(probe, 0x10),
        cancellation_leaf(probe, 0x20),
        cancellation_leaf(probe, 0x40),
    };
    probe.markers |= 0x80;
}

Task<void> cancel_then_return(coro::Process* self, CancellationProbe& probe) {
    co_await Delay{1_ns};
    self->cancel();
    probe.markers |= 0x100;
    co_return;
}

Task<void> self_cancel_then_suspend(coro::Process* self,
                                    CancellationProbe& probe) {
    co_await Delay{1_ns};
    self->cancel();
    probe.markers |= 0x200;
    co_await Delay{1_ns};
    probe.markers |= 0x400;
}

Task<void> cancellation_contract(ConformanceTb tb) {
    CancellationProbe probe;

    const auto direct = tb.spawn(cancellation_direct_parent(probe));
    direct.cancel();
    co_await Delay{2_ns};
    tb.expect_true("recursive direct-task cancellation completes", direct.done());
    tb.expect_true("recursive direct-task cancellation status",
                   direct.cancelled());
    tb.expect_eq("recursive direct-task cancellation stops descendants",
                 probe.markers & 0x7, 0);

    const auto joined = tb.spawn(cancellation_join_parent(probe));
    joined.cancel();
    co_await Delay{2_ns};
    tb.expect_true("variadic Join cancellation completes", joined.done());
    tb.expect_true("variadic Join cancellation status", joined.cancelled());
    tb.expect_eq("variadic Join cancellation stops every child",
                 probe.markers & 0xf0, 0);

    coro::Process returning;
    returning = tb.spawn(cancel_then_return(&returning, probe));
    co_await returning;
    tb.expect_true("cancel then co_return process is done", returning.done());
    tb.expect_true("cancel then co_return is normal completion",
                   !returning.cancelled());
    tb.expect_eq("cancel then co_return body completed", probe.markers & 0x100,
                 0x100);

    coro::Process suspending;
    suspending = tb.spawn(self_cancel_then_suspend(&suspending, probe));
    co_await suspending;
    tb.expect_true("self-cancel at suspension is done", suspending.done());
    tb.expect_true("self-cancel at suspension reports cancellation",
                   suspending.cancelled());
    tb.expect_eq("self-cancel prevents resume after true suspension",
                 probe.markers & 0x600, 0x200);
}

struct DelayOrder {
    std::array<uint32_t, 3> values{};
    uint32_t count = 0;
};

Task<void> same_deadline_marker(DelayOrder& order, uint32_t marker) {
    co_await Delay{1_ns};
    if (order.count < order.values.size()) {
        order.values[order.count++] = marker;
    }
}

Task<void> same_deadline_delay_contract(ConformanceTb tb) {
    DelayOrder order;
    const auto first = tb.spawn(same_deadline_marker(order, 1));
    const auto second = tb.spawn(same_deadline_marker(order, 2));
    const auto third = tb.spawn(same_deadline_marker(order, 3));

    co_await third;
    tb.expect_eq("same-deadline Delay callback count", order.count, 3);
    tb.expect_eq("same-deadline Delay FIFO first", order.values[0], 1);
    tb.expect_eq("same-deadline Delay FIFO second", order.values[1], 2);
    tb.expect_eq("same-deadline Delay FIFO third", order.values[2], 3);
    tb.expect_true("same-deadline earlier processes complete",
                   first.done() && second.done());
}

struct TimerRearmOrder {
    std::array<uint32_t, 2> values{};
    std::array<uint64_t, 2> times{};
    uint32_t count = 0;
};

struct TimerDispatchTrace {
    std::array<uint32_t, 8> values{};
    std::array<uint64_t, 8> times{};
    uint32_t count = 0;
};

void record_timer_dispatch(ConformanceTb tb, TimerDispatchTrace& trace,
                           uint32_t marker) {
    if (trace.count >= trace.values.size()) {
        tb.expect_true("timer dispatch trace capacity", false);
        return;
    }
    trace.values[trace.count] = marker;
    trace.times[trace.count] = tb.now().in_femtoseconds();
    ++trace.count;
}

Task<void> r1_timer_rearm(ConformanceTb tb, TimerDispatchTrace& trace) {
    co_await Delay{2_ns};
    record_timer_dispatch(tb, trace, 1);
    co_await Delay{5_ns};
    record_timer_dispatch(tb, trace, 3);
    tb.dut.drive_value.set(0x51);
    co_await Delay{1_ps};
    tb.expect_eq("R1 later rearm output settles", tb.dut.comb_sum.get(), 0x51);
}

Task<void> r1_coincident_clock(ConformanceTb tb, TimerDispatchTrace& trace) {
    co_await RisingEdge{tb.dut.clock.a};
    record_timer_dispatch(tb, trace, 2);
}

Task<void> timer_r1_contract(ConformanceTb tb) {
    TimerDispatchTrace trace;
    const auto timer = tb.spawn(r1_timer_rearm(tb, trace));
    const auto clock = tb.spawn(r1_coincident_clock(tb, trace));

    co_await timer;
    tb.expect_time("R1 later rearm completion time", tb.now(), 7_ns + 1_ps);
    tb.expect_eq("R1 exact callback count", trace.count, 3);
    tb.expect_eq("R1 initial timer precedes coincident clock", trace.values[0], 1);
    tb.expect_eq("R1 coincident clock callback order", trace.values[1], 2);
    tb.expect_eq("R1 later timer callback order", trace.values[2], 3);
    tb.expect_time("R1 initial timer exact time", SimTime{trace.times[0]}, 2_ns);
    tb.expect_time("R1 coincident clock exact time", SimTime{trace.times[1]}, 2_ns);
    tb.expect_time("R1 later timer exact time", SimTime{trace.times[2]}, 7_ns);
    tb.expect_true("R1 coincident clock process completes", clock.done());

    co_await Delay{1_ps};
    tb.expect_time("R1 stale wake observation time", tb.now(), 7_ns + 2_ps);
    tb.expect_eq("R1 callbacks remain exact once", trace.count, 3);
}

Task<void> r2_original_timer(ConformanceTb tb, TimerDispatchTrace& trace) {
    co_await Delay{4_ns};
    record_timer_dispatch(tb, trace, 1);
}

Task<void> r2_equal_timer_from_clock(ConformanceTb tb,
                                     TimerDispatchTrace& trace) {
    co_await RisingEdge{tb.dut.clock.a};
    tb.expect_time("R2 equal timer arm time", tb.now(), 2_ns);
    co_await Delay{2_ns};
    record_timer_dispatch(tb, trace, 2);
    tb.dut.drive_value.set(0x62);
}

Task<void> timer_r2_contract(ConformanceTb tb) {
    TimerDispatchTrace trace;
    const auto original = tb.spawn(r2_original_timer(tb, trace));
    const auto equal = tb.spawn(r2_equal_timer_from_clock(tb, trace));

    co_await equal;
    tb.expect_time("R2 equal-target completion time", tb.now(), 4_ns);
    tb.expect_eq("R2 exact callback count", trace.count, 2);
    tb.expect_eq("R2 same-deadline FIFO original", trace.values[0], 1);
    tb.expect_eq("R2 same-deadline FIFO mid-sleep arm", trace.values[1], 2);
    tb.expect_time("R2 original timer exact time", SimTime{trace.times[0]}, 4_ns);
    tb.expect_time("R2 equal timer exact time", SimTime{trace.times[1]}, 4_ns);
    tb.expect_true("R2 original timer process completes", original.done());

    co_await Delay{1_ps};
    tb.expect_time("R2 post-settle time", tb.now(), 4_ns + 1_ps);
    tb.expect_eq("R2 callbacks remain exact once", trace.count, 2);
    tb.expect_eq("R2 equal timer output settles", tb.dut.comb_sum.get(), 0x62);
}

Task<void> chained_original_timer(ConformanceTb tb,
                                  TimerDispatchTrace& trace) {
    co_await Delay{11_ns};
    record_timer_dispatch(tb, trace, 3);
    tb.dut.drive_value.set(0x73);
    co_await Delay{1_ps};
    tb.expect_eq("chained earlier timer output settles", tb.dut.comb_sum.get(),
                 0x73);
}

Task<void> chained_from_clock_a(ConformanceTb tb, TimerDispatchTrace& trace) {
    co_await RisingEdge{tb.dut.clock.a};
    co_await Delay{6_ns};
    record_timer_dispatch(tb, trace, 2);
}

Task<void> chained_from_clock_b(ConformanceTb tb, TimerDispatchTrace& trace) {
    co_await RisingEdge{tb.dut.clock.b};
    co_await Delay{1_ns};
    record_timer_dispatch(tb, trace, 1);
}

Task<void> chained_earlier_deadlines_contract(ConformanceTb tb) {
    TimerDispatchTrace trace;
    const auto original = tb.spawn(chained_original_timer(tb, trace));
    const auto clock_a = tb.spawn(chained_from_clock_a(tb, trace));
    const auto clock_b = tb.spawn(chained_from_clock_b(tb, trace));

    co_await original;
    tb.expect_time("chained earlier completion time", tb.now(), 11_ns + 1_ps);
    tb.expect_eq("chained earlier exact callback count", trace.count, 3);
    tb.expect_eq("chained earlier first callback order", trace.values[0], 1);
    tb.expect_eq("chained earlier second callback order", trace.values[1], 2);
    tb.expect_eq("chained earlier original callback order", trace.values[2], 3);
    tb.expect_time("chained earliest exact time", SimTime{trace.times[0]}, 7_ns);
    tb.expect_time("chained second exact time", SimTime{trace.times[1]}, 8_ns);
    tb.expect_time("chained original exact time", SimTime{trace.times[2]}, 11_ns);
    tb.expect_true("chained non-owner clock processes complete",
                   clock_a.done() && clock_b.done());

    co_await Delay{1_ps};
    tb.expect_time("chained stale wake observation time", tb.now(),
                   11_ns + 2_ps);
    tb.expect_eq("chained callbacks remain exact once", trace.count, 3);
}

Task<void> idle_rearm_worker(ConformanceTb tb, TimerDispatchTrace& trace) {
    co_await RisingEdge{tb.dut.clock.a};
    tb.expect_time("idle rearm clock time", tb.now(), 2_ns);
    co_await Delay{1_ns};
    record_timer_dispatch(tb, trace, 1);
    tb.dut.drive_value.set(0x84);
    co_await Delay{1_ps};
    tb.expect_eq("idle rearm output settles", tb.dut.comb_sum.get(), 0x84);
}

Task<void> idle_timer_rearm_contract(ConformanceTb tb) {
    TimerDispatchTrace trace;
    const auto worker = tb.spawn(idle_rearm_worker(tb, trace));

    co_await worker;
    tb.expect_time("idle rearm completion time", tb.now(), 3_ns + 1_ps);
    tb.expect_eq("idle rearm exact callback count", trace.count, 1);
    tb.expect_eq("idle rearm callback marker", trace.values[0], 1);
    tb.expect_time("idle rearm exact timer time", SimTime{trace.times[0]}, 3_ns);

    co_await Delay{1_ps};
    tb.expect_time("idle rearm stale wake observation time", tb.now(),
                   3_ns + 2_ps);
    tb.expect_eq("idle rearm callback remains exact once", trace.count, 1);
}

Task<void> timer_rearm_marker(ConformanceTb tb, TimerRearmOrder& order,
                              SimTime delay, uint32_t marker) {
    co_await Delay{delay};
    if (order.count < order.values.size()) {
        order.values[order.count] = marker;
        order.times[order.count] = tb.now().in_femtoseconds();
        ++order.count;
    }
}

Task<void> earlier_deadline_rearm_contract(ConformanceTb tb) {
    TimerRearmOrder order;
    const auto later = tb.spawn(timer_rearm_marker(tb, order, 10_ns, 2));

    co_await RisingEdge{tb.dut.clock.a};
    const auto earlier = tb.spawn(timer_rearm_marker(tb, order, 1_ns, 1));
    co_await earlier;
    tb.expect_time("earlier rearmed timer deadline", tb.now(), 3_ns);
    tb.expect_eq("earlier rearmed timer callback count", order.count, 1);
    tb.expect_eq("earlier rearmed timer callback order", order.values[0], 1);
    tb.expect_time("earlier rearmed timer recorded time",
                   SimTime{order.times[0]}, 3_ns);

    co_await later;
    tb.expect_time("original later timer deadline", tb.now(), 10_ns);
    tb.expect_eq("both rearmed timer callbacks complete", order.count, 2);
    tb.expect_eq("original later timer callback order", order.values[1], 2);
    tb.expect_time("original later timer recorded time",
                   SimTime{order.times[1]}, 10_ns);
}

Task<void> stale_falling_interest_contract(ConformanceTb tb) {
    co_await Delay{14_ns};
    tb.expect_true("no falling-edge interest before timeout probes",
                   !tb.has_falling_edge_waiters());

    const auto falling_winner =
        co_await First{FallingEdge{tb.dut.stable_signal}, Delay{1_ns}};
    tb.expect_eq("FallingEdge timeout wins", falling_winner, 1);
    tb.expect_true("timed-out FallingEdge interest is removed",
                   !tb.has_falling_edge_waiters());

    const auto any_winner =
        co_await First{Edge{tb.dut.stable_signal}, Delay{1_ns}};
    tb.expect_eq("Any-edge timeout wins", any_winner, 1);
    tb.expect_true("timed-out Any-edge interest is removed",
                   !tb.has_falling_edge_waiters());
}

Task<void> process_and_variadic_contract(ConformanceTb tb) {
    const auto worker = tb.spawn(lifecycle_worker(tb));
    const auto waiter = tb.spawn(lifecycle_waiter(tb, worker));
    tb.expect_true("spawn returns valid process", worker.valid());
    tb.expect_true("spawned process initially running", !worker.done());

    co_await waiter;
    tb.expect_true("awaited process completes", worker.done());
    tb.expect_true("completed process is not cancelled", !worker.cancelled());
    tb.expect_true("process waiter completes", waiter.done());
    tb.expect_eq("completion wakes process waiter", tb.lifecycle_markers() & 0x3,
                 0x3);

    const auto cancelled = tb.spawn(cancelled_parent(tb));
    const auto cancel_waiter = tb.spawn(lifecycle_waiter(tb, cancelled));
    cancelled.cancel();
    co_await cancel_waiter;
    tb.expect_true("cancelled process is done", cancelled.done());
    tb.expect_true("cancelled process reports status", cancelled.cancelled());
    tb.expect_true("cancellation wakes process waiter", cancel_waiter.done());
    tb.expect_eq("cancel recursively stops nested child",
                 tb.lifecycle_markers() & 0xc000, 0);

    const auto winner =
        co_await First{Delay{1_ns}, Delay{2_ns}, Delay{3_ns}};
    tb.expect_eq("three-trigger First winner", winner, 0);

    co_await Join{
        lifecycle_join_marker(tb, 1_ns, 0x4),
        lifecycle_join_marker(tb, 2_ns, 0x8),
        lifecycle_join_marker(tb, 3_ns, 0x10),
    };
    tb.expect_eq("three-task Join completion", tb.lifecycle_markers() & 0x1c,
                 0x1c);
}

struct FrameCounter {
    uint32_t* destructions;

    ~FrameCounter() { ++*destructions; }
};

struct MoveOnlyValue {
    MoveOnlyValue() = delete;
    MoveOnlyValue(uint32_t value, uint32_t& destructions)
        : value(value), destructions(&destructions) {}
    MoveOnlyValue(const MoveOnlyValue&) = delete;
    MoveOnlyValue& operator=(const MoveOnlyValue&) = delete;
    MoveOnlyValue(MoveOnlyValue&& other) noexcept
        : value(other.value),
          destructions(std::exchange(other.destructions, nullptr)) {}
    MoveOnlyValue& operator=(MoveOnlyValue&&) = delete;

    ~MoveOnlyValue() {
        if (destructions) ++*destructions;
    }

    uint32_t value;
    uint32_t* destructions;
};

Task<uint32_t> typed_value_leaf(ConformanceTb tb, uint32_t& destructions) {
    FrameCounter counter{&destructions};
    co_await RisingEdge{tb.dut.clock.a};
    co_await Delay{1_ps};
    co_return tb.dut.sample.a.get();
}

Task<uint32_t> typed_value_middle(ConformanceTb tb,
                                  uint32_t& leaf_destructions,
                                  uint32_t& destructions) {
    FrameCounter counter{&destructions};
    const uint32_t value = co_await typed_value_leaf(tb, leaf_destructions);
    tb.expect_eq("typed leaf frame reclaimed before middle continuation",
                 leaf_destructions, 1);
    co_return value + 1;
}

Task<uint32_t> typed_value_outer(ConformanceTb tb,
                                 uint32_t& leaf_destructions,
                                 uint32_t& middle_destructions,
                                 uint32_t& destructions) {
    FrameCounter counter{&destructions};
    const uint32_t value = co_await typed_value_middle(
        tb, leaf_destructions, middle_destructions);
    tb.expect_eq("typed middle frame reclaimed before outer continuation",
                 middle_destructions, 1);
    co_return value + 2;
}

Task<MoveOnlyValue> typed_move_only_value(uint32_t& frame_destructions,
                                          uint32_t& result_destructions) {
    FrameCounter counter{&frame_destructions};
    co_await Delay{1_ps};
    co_return MoveOnlyValue{0x55, result_destructions};
}

Task<void> typed_task_contract(ConformanceTb tb) {
    uint32_t leaf_destructions = 0;
    uint32_t middle_destructions = 0;
    uint32_t outer_destructions = 0;
    const uint32_t result = co_await typed_value_outer(
        tb, leaf_destructions, middle_destructions, outer_destructions);

    tb.expect_eq("nested Task value propagation", result, 0x33);
    tb.expect_eq("typed leaf frame destruction", leaf_destructions, 1);
    tb.expect_eq("typed middle frame destruction", middle_destructions, 1);
    tb.expect_eq("typed outer frame prompt destruction", outer_destructions,
                 1);

    uint32_t move_frame_destructions = 0;
    uint32_t move_result_destructions = 0;
    {
        auto value = co_await typed_move_only_value(
            move_frame_destructions, move_result_destructions);
        tb.expect_eq("move-only non-default Task value", value.value, 0x55);
        tb.expect_eq("move-only typed child frame prompt destruction",
                     move_frame_destructions, 1);
        tb.expect_eq("moved result remains alive after child reclamation",
                     move_result_destructions, 0);
    }
    tb.expect_eq("move-only result destroyed exactly once",
                 move_result_destructions, 1);
}

struct TypedCancellationProbe {
    uint32_t child_resumes = 0;
    uint32_t child_destructions = 0;
    uint32_t result = 0xdeadbeef;
};

Task<uint32_t> cancellable_typed_child(ConformanceTb tb,
                                       TypedCancellationProbe& probe) {
    FrameCounter counter{&probe.child_destructions};
    co_await RisingEdge{tb.dut.clock.a};
    ++probe.child_resumes;
    co_return 0x1234;
}

Task<void> cancellable_typed_parent(ConformanceTb tb,
                                    TypedCancellationProbe& probe) {
    probe.result = co_await cancellable_typed_child(tb, probe);
}

Task<void> typed_cancellation_contract(ConformanceTb tb) {
    TypedCancellationProbe probe;
    const auto process = tb.spawn(cancellable_typed_parent(tb, probe));
    co_await Delay{1_ps};
    process.cancel();

    tb.expect_true("typed cancellation request is cooperative",
                   !process.done());
    co_await Delay{1_ps};
    tb.expect_true("typed parent cancellation completes", process.done());
    tb.expect_true("typed parent cancellation reports status",
                   process.cancelled());
    tb.expect_eq("typed cancellation destroys child frame",
                 probe.child_destructions, 1);
    tb.expect_eq("typed cancellation suppresses child result", probe.result,
                 0xdeadbeef);

    co_await Delay{3_ns};
    tb.expect_eq("cancelled typed child ignores later real edge",
                 probe.child_resumes, 0);
    tb.expect_eq("cancelled typed child frame destroyed once",
                 probe.child_destructions, 1);
}

Task<void> clock_cycles_probe(ConformanceTb tb, Signal clock, uint64_t count,
                              SimTime expected_time, uint32_t& completions,
                              const char* label) {
    co_await clock_cycles(clock, count);
    ++completions;
    tb.expect_time(label, tb.now(), expected_time);
}

Task<void> clock_cycles_contract(ConformanceTb tb) {
    uint32_t completions = 0;
    co_await clock_cycles(tb.dut.clock.a, 0);
    tb.expect_time("ClockCycles zero is immediate", tb.now(), 0_ns);

    const auto generated_one = tb.spawn(clock_cycles_probe(
        tb, tb.dut.clock.a, 1, 2_ns, completions,
        "ClockCycles one generated clock"));
    const auto generated_many = tb.spawn(clock_cycles_probe(
        tb, tb.dut.clock.b, 2, 12_ns, completions,
        "ClockCycles many generated clock"));
    const auto manual_many = tb.spawn(clock_cycles_probe(
        tb, tb.dut.clock.manual, 2, 6_ns, completions,
        "ClockCycles testbench-driven clock"));
    const auto derived_many = tb.spawn(clock_cycles_probe(
        tb, tb.dut.clock.derived, 3, 10_ns, completions,
        "ClockCycles DUT-derived clock"));

    co_await generated_many;
    tb.expect_eq("ClockCycles concurrent completion count", completions, 4);
    tb.expect_true("ClockCycles generated one process done",
                   generated_one.done());
    tb.expect_true("ClockCycles manual and derived processes done",
                   manual_many.done() && derived_many.done());
}

Task<void> with_timeout_contract(ConformanceTb tb) {
    uint32_t resumptions = 0;
    const auto timeout = co_await with_timeout(
        RisingEdge{tb.dut.clock.manual}, 1_ns);
    ++resumptions;
    tb.expect_eq("with_timeout timeout winner",
                 static_cast<uint32_t>(timeout),
                 static_cast<uint32_t>(TimeoutOutcome::TimedOut));
    tb.expect_time("with_timeout timeout deadline", tb.now(), 1_ns);

    const auto edge = co_await with_timeout(RisingEdge{tb.dut.clock.a}, 3_ns);
    ++resumptions;
    tb.expect_eq("with_timeout edge winner", static_cast<uint32_t>(edge),
                 static_cast<uint32_t>(TimeoutOutcome::Triggered));
    tb.expect_time("with_timeout non-tie edge timestamp", tb.now(), 2_ns);

    co_await Delay{3_ns};
    tb.expect_time("with_timeout stale losers passed", tb.now(), 5_ns);
    tb.expect_eq("with_timeout losers never resume operation", resumptions, 2);
}

Task<void> with_timeout_tie_contract(ConformanceTb tb) {
    uint32_t completions = 0;
    const auto outcome =
        co_await with_timeout(FallingEdge{tb.dut.clock.a}, 4_ns);
    ++completions;

    const bool valid_outcome = outcome == TimeoutOutcome::Triggered ||
                               outcome == TimeoutOutcome::TimedOut;
    tb.expect_true("with_timeout simultaneous tie has a valid outcome",
                   valid_outcome);
    tb.expect_time("with_timeout simultaneous tie timestamp", tb.now(), 4_ns);

    co_await Delay{4_ns};
    tb.expect_time("with_timeout simultaneous stale loser stays inactive",
                   tb.now(), 8_ns);
    tb.expect_eq("with_timeout simultaneous tie completes once", completions,
                 1);
}

struct TaskTimeoutProbe {
    uint32_t resumes = 0;
    uint32_t frame_destructions = 0;
};

Task<uint32_t> timeout_value(SimTime delay, uint32_t value,
                             TaskTimeoutProbe& probe) {
    FrameCounter counter{&probe.frame_destructions};
    co_await Delay{delay};
    ++probe.resumes;
    co_return value;
}

Task<void> timeout_void(SimTime delay, TaskTimeoutProbe& probe) {
    FrameCounter counter{&probe.frame_destructions};
    co_await Delay{delay};
    ++probe.resumes;
}

Task<uint32_t> timeout_nested_leaf(SimTime delay, TaskTimeoutProbe& probe) {
    FrameCounter counter{&probe.frame_destructions};
    co_await Delay{delay};
    ++probe.resumes;
    co_return 0x17;
}

Task<uint32_t> timeout_nested_parent(SimTime delay, TaskTimeoutProbe& parent,
                                     TaskTimeoutProbe& leaf) {
    FrameCounter counter{&parent.frame_destructions};
    co_return co_await timeout_nested_leaf(delay, leaf);
}

Task<uint32_t> timeout_event_value(Event& event, TaskTimeoutProbe& probe) {
    FrameCounter counter{&probe.frame_destructions};
    co_await event;
    ++probe.resumes;
    co_return 0x23;
}

Task<uint32_t> timeout_channel_value(Channel<uint32_t>& channel,
                                     TaskTimeoutProbe& probe) {
    FrameCounter counter{&probe.frame_destructions};
    const uint32_t value = co_await channel.get();
    ++probe.resumes;
    co_return value;
}

Task<void> timeout_process_target(uint32_t& resumes) {
    co_await Delay{10_ns};
    ++resumes;
}

Task<uint32_t> timeout_process_value(coro::Process process,
                                     TaskTimeoutProbe& probe) {
    FrameCounter counter{&probe.frame_destructions};
    co_await process;
    ++probe.resumes;
    co_return 0x29;
}

Task<void> cancellable_timeout_process(TaskTimeoutProbe& probe,
                                       uint32_t& continuations) {
    static_cast<void>(
        co_await with_timeout(timeout_value(20_ns, 1, probe), 30_ns));
    ++continuations;
}

Task<void> task_with_timeout_contract(ConformanceTb tb) {
    TaskTimeoutProbe value_probe;
    auto value = co_await with_timeout(timeout_value(1_ps, 0x42, value_probe),
                                       3_ps);
    tb.expect_true("Task<T> timeout completion has value", value.has_value());
    tb.expect_true("Task<T> timeout completion is not timeout",
                   !value.timed_out());
    tb.expect_eq("Task<T> timeout completed value", value.value(), 0x42);
    tb.expect_eq("Task<T> timeout completed child resumes once",
                 value_probe.resumes, 1);
    tb.expect_eq("Task<T> timeout completed frame reclaimed promptly",
                 value_probe.frame_destructions, 1);

    TaskTimeoutProbe void_probe;
    auto void_result =
        co_await with_timeout(timeout_void(1_ps, void_probe), 3_ps);
    tb.expect_true("Task<void> timeout completion state",
                   void_result.completed());
    void_result.value();
    tb.expect_eq("Task<void> timeout child resumes once", void_probe.resumes,
                 1);
    tb.expect_eq("Task<void> timeout frame reclaimed promptly",
                 void_probe.frame_destructions, 1);

    TaskTimeoutProbe void_timeout_probe;
    auto void_timeout = co_await with_timeout(
        timeout_void(10_ns, void_timeout_probe), 2_ps);
    tb.expect_true("Task<void> timeout state", void_timeout.timed_out());
    tb.expect_eq("Task<void> timeout loser never resumes",
                 void_timeout_probe.resumes, 0);
    tb.expect_eq("Task<void> timeout loser waits for cleanup boundary",
                 void_timeout_probe.frame_destructions, 0);
    co_await Delay{1_ps};
    tb.expect_eq("Task<void> timeout loser destroyed once",
                 void_timeout_probe.frame_destructions, 1);

    TaskTimeoutProbe move_probe;
    uint32_t move_result_destructions = 0;
    {
        auto move_result = co_await with_timeout(
            typed_move_only_value(move_probe.frame_destructions,
                                  move_result_destructions),
            3_ps);
        tb.expect_true("Task timeout move-only result completes",
                       move_result.has_value());
        tb.expect_eq("Task timeout move-only non-default value",
                     move_result->value, 0x55);
        tb.expect_eq("Task timeout move-only frame reclaimed promptly",
                     move_probe.frame_destructions, 1);
        tb.expect_eq("Task timeout moved result remains alive",
                     move_result_destructions, 0);
    }
    tb.expect_eq("Task timeout moved result destroyed exactly once",
                 move_result_destructions, 1);

    TaskTimeoutProbe tie_probe;
    auto tie = co_await with_timeout(timeout_value(2_ps, 0x55, tie_probe),
                                     2_ps);
    tb.expect_true("Task timeout same-deadline completed task wins",
                   tie.has_value());
    tb.expect_eq("Task timeout same-deadline value", tie.value(), 0x55);
    tb.expect_eq("Task timeout same-deadline resumes child once",
                 tie_probe.resumes, 1);
    tb.expect_eq("Task timeout same-deadline destroys frame once",
                 tie_probe.frame_destructions, 1);

    TaskTimeoutProbe parent_probe;
    TaskTimeoutProbe leaf_probe;
    auto nested = co_await with_timeout(
        timeout_nested_parent(20_ns, parent_probe, leaf_probe), 2_ps);
    tb.expect_true("nested Task timeout reports timeout", nested.timed_out());
    tb.expect_eq("Task timeout loser destruction waits for boundary",
                 parent_probe.frame_destructions, 0);
    co_await Delay{1_ps};
    tb.expect_eq("nested Task timeout parent destroyed once",
                 parent_probe.frame_destructions, 1);
    tb.expect_eq("nested Task timeout leaf destroyed once",
                 leaf_probe.frame_destructions, 1);
    tb.expect_eq("nested Task timeout leaf never resumes", leaf_probe.resumes,
                 0);

    Event event;
    TaskTimeoutProbe event_probe;
    auto event_result = co_await with_timeout(
        timeout_event_value(event, event_probe), 2_ps);
    tb.expect_true("Event Task timeout reports timeout",
                   event_result.timed_out());
    co_await Delay{1_ps};
    event.set();
    tb.expect_eq("Event Task timeout removes stale waiter",
                 event_probe.resumes, 0);
    tb.expect_eq("Event Task timeout destroys frame once",
                 event_probe.frame_destructions, 1);

    Channel<uint32_t> channel;
    TaskTimeoutProbe channel_probe;
    auto channel_result = co_await with_timeout(
        timeout_channel_value(channel, channel_probe), 2_ps);
    tb.expect_true("Channel Task timeout reports timeout",
                   channel_result.timed_out());
    co_await Delay{1_ps};
    channel.put_nowait(0x66);
    tb.expect_eq("Channel Task timeout preserves item",
                 co_await channel.get(), 0x66);
    tb.expect_eq("Channel Task timeout removes stale consumer",
                 channel_probe.resumes, 0);
    tb.expect_eq("Channel Task timeout destroys frame once",
                 channel_probe.frame_destructions, 1);

    uint32_t process_target_resumes = 0;
    const auto process_target = tb.spawn(
        timeout_process_target(process_target_resumes));
    TaskTimeoutProbe process_wait_probe;
    auto process_wait_result = co_await with_timeout(
        timeout_process_value(process_target, process_wait_probe), 2_ps);
    tb.expect_true("Process wait Task timeout reports timeout",
                   process_wait_result.timed_out());
    co_await Delay{1_ps};
    tb.expect_eq("Process wait Task timeout destroys waiter once",
                 process_wait_probe.frame_destructions, 1);
    tb.expect_eq("Process wait Task timeout waiter never resumes",
                 process_wait_probe.resumes, 0);

    TaskTimeoutProbe process_probe;
    uint32_t process_continuations = 0;
    const auto process = tb.spawn(
        cancellable_timeout_process(process_probe, process_continuations));
    co_await Delay{1_ps};
    process.cancel();
    tb.expect_true("Process Task timeout cancellation is cooperative",
                   !process.done());
    co_await Delay{1_ps};
    tb.expect_true("Process Task timeout cancellation completes",
                   process.cancelled());
    tb.expect_eq("Process Task timeout continuation suppressed",
                 process_continuations, 0);
    tb.expect_eq("Process Task timeout child destroyed once",
                 process_probe.frame_destructions, 1);
    tb.expect_eq("Process Task timeout child never resumes",
                 process_probe.resumes, 0);

    co_await Delay{30_ns};
    tb.expect_eq("Task timeout stale losers never resume",
                 leaf_probe.resumes + event_probe.resumes +
                     channel_probe.resumes + process_wait_probe.resumes +
                     process_probe.resumes,
                 0);
    tb.expect_true("Process wait timeout target completes normally",
                   process_target.done() && !process_target.cancelled());
    tb.expect_eq("Process wait timeout target resumes once",
                 process_target_resumes, 1);
    tb.expect_eq("Task timeout loser frames remain exactly once",
                 parent_probe.frame_destructions + leaf_probe.frame_destructions +
                     event_probe.frame_destructions +
                     channel_probe.frame_destructions +
                     process_probe.frame_destructions,
                 5);
}

struct PredicateProbe {
    std::array<uint64_t, 4> times{};
    std::array<uint32_t, 4> values{};
    uint32_t evaluations = 0;
};

Task<void> predicate_driver(ConformanceTb tb) {
    co_await Delay{3_ns};
    tb.dut.predicate_signal.set(1);
}

Task<void> wait_until_contract(ConformanceTb tb) {
    tb.dut.predicate_signal.set(1);
    PredicateProbe immediate;
    co_await wait_until(
        tb.dut.predicate_signal,
        [&immediate, tb](uint32_t value) {
            const auto index = immediate.evaluations++;
            immediate.times[index] = tb.now().in_femtoseconds();
            immediate.values[index] = value;
            return value == 1;
        },
        tb.dut.clock.a);
    tb.expect_eq("wait_until immediate evaluation count",
                 immediate.evaluations, 1);
    tb.expect_time("wait_until immediate timestamp",
                   SimTime{immediate.times[0]}, 0_ns);
    tb.expect_eq("wait_until immediate observed value", immediate.values[0],
                 1);

    tb.dut.predicate_signal.set(0);
    const auto driver = tb.spawn(predicate_driver(tb));
    PredicateProbe delayed;
    co_await wait_until(
        tb.dut.predicate_signal,
        [&delayed, tb](uint32_t value) {
            const auto index = delayed.evaluations++;
            delayed.times[index] = tb.now().in_femtoseconds();
            delayed.values[index] = value;
            return value == 1;
        },
        tb.dut.clock.a);

    tb.expect_eq("wait_until delayed evaluation count", delayed.evaluations,
                 3);
    tb.expect_time("wait_until delayed initial evaluation",
                   SimTime{delayed.times[0]}, 0_ns);
    tb.expect_time("wait_until delayed first edge evaluation",
                   SimTime{delayed.times[1]}, 2_ns);
    tb.expect_time("wait_until delayed completion evaluation",
                   SimTime{delayed.times[2]}, 6_ns);
    tb.expect_eq("wait_until delayed initial value", delayed.values[0], 0);
    tb.expect_eq("wait_until delayed edge value", delayed.values[1], 0);
    tb.expect_eq("wait_until delayed completion value", delayed.values[2], 1);
    tb.expect_true("wait_until Delay-driven predicate process done",
                   driver.done());
}

struct EventProbe {
    std::array<uint32_t, 8> order{};
    uint32_t count = 0;
};

Task<void> event_waiter(Event& event, EventProbe& probe, uint32_t marker) {
    co_await event;
    probe.order[probe.count++] = marker;
}

Task<void> event_contract(ConformanceTb tb) {
    Event event;
    EventProbe probe;

    event.set();
    co_await event;
    tb.expect_true("Event set-before-wait remains set", event.is_set());
    tb.expect_time("Event set-before-wait is immediate", tb.now(), 0_ns);

    event.clear();
    tb.expect_true("Event clear resets state", !event.is_set());
    const auto waiting = tb.spawn(event_waiter(event, probe, 1));
    co_await Delay{1_ps};
    tb.expect_true("Event wait-before-set remains pending", !waiting.done());
    event.set();
    co_await waiting;
    tb.expect_eq("Event wait-before-set marker", probe.order[0], 1);

    event.clear();
    const auto first = tb.spawn(event_waiter(event, probe, 2));
    const auto second = tb.spawn(event_waiter(event, probe, 3));
    const auto third = tb.spawn(event_waiter(event, probe, 4));
    co_await Delay{1_ps};
    event.set();
    co_await third;
    tb.expect_eq("Event FIFO waiter count", probe.count, 4);
    tb.expect_eq("Event FIFO waiter one", probe.order[1], 2);
    tb.expect_eq("Event FIFO waiter two", probe.order[2], 3);
    tb.expect_eq("Event FIFO waiter three", probe.order[3], 4);
    tb.expect_true("Event all FIFO processes complete",
                   first.done() && second.done());

    event.clear();
    const auto cancelled = tb.spawn(event_waiter(event, probe, 99));
    co_await Delay{1_ps};
    cancelled.cancel();
    tb.expect_true("Event waiter cancellation request is cooperative",
                   !cancelled.done());
    co_await Delay{1_ps};
    tb.expect_true("Event cancelled waiter reports cancellation",
                   cancelled.cancelled());
    event.set();
    tb.expect_eq("Event cancellation cleanup prevents stale resume",
                 probe.count, 4);

    event.clear();
    const auto reused = tb.spawn(event_waiter(event, probe, 5));
    co_await Delay{1_ps};
    event.set();
    co_await reused;
    tb.expect_eq("Event clear and reuse marker", probe.order[4], 5);
    tb.expect_eq("Event clear and reuse count", probe.count, 5);
}

struct ChannelProbe {
    std::array<uint32_t, 12> values{};
    uint32_t count = 0;
};

Task<void> channel_consumer(Channel<uint32_t>& channel, ChannelProbe& probe) {
    probe.values[probe.count++] = co_await channel.get();
}

Task<void> channel_delayed_producer(Channel<uint32_t>& channel,
                                    uint32_t value) {
    co_await Delay{1_ns};
    co_await channel.put(value);
}

Task<void> channel_put_then_cancel(Channel<uint32_t>& channel, uint32_t value,
                                   coro::Process target) {
    co_await channel.put(value);
    target.cancel();
}

Task<void> channel_contract(ConformanceTb tb) {
    Channel<uint32_t> channel;
    ChannelProbe probe;

    co_await channel.put(10);
    tb.expect_eq("Channel put-before-get size", channel.size(), 1);
    tb.expect_eq("Channel put-before-get value", co_await channel.get(), 10);

    co_await channel.put(20);
    co_await channel.put(21);
    tb.expect_eq("Channel FIFO first queued value", co_await channel.get(), 20);
    tb.expect_eq("Channel FIFO second queued value", co_await channel.get(),
                 21);

    const auto waiting = tb.spawn(channel_consumer(channel, probe));
    co_await Delay{1_ps};
    tb.expect_true("Channel get-before-put remains pending", !waiting.done());
    co_await channel.put(30);
    co_await waiting;
    tb.expect_eq("Channel get-before-put value", probe.values[0], 30);

    const auto consumer_one = tb.spawn(channel_consumer(channel, probe));
    const auto consumer_two = tb.spawn(channel_consumer(channel, probe));
    const auto consumer_three = tb.spawn(channel_consumer(channel, probe));
    const auto producer_one =
        tb.spawn(channel_delayed_producer(channel, 40));
    const auto producer_two =
        tb.spawn(channel_delayed_producer(channel, 41));
    const auto producer_three =
        tb.spawn(channel_delayed_producer(channel, 42));
    co_await consumer_three;
    tb.expect_eq("Channel multiple producer/consumer count", probe.count, 4);
    tb.expect_eq("Channel multiple producer FIFO first", probe.values[1], 40);
    tb.expect_eq("Channel multiple producer FIFO second", probe.values[2], 41);
    tb.expect_eq("Channel multiple producer FIFO third", probe.values[3], 42);
    tb.expect_true("Channel multiple consumers all complete",
                   consumer_one.done() && consumer_two.done());
    tb.expect_true("Channel multiple producers all complete",
                   producer_one.done() && producer_two.done() &&
                       producer_three.done());

    Channel<uint32_t> cancellation_channel;
    ChannelProbe cancelled_probe;
    ChannelProbe survivor_probe;
    const auto cancelled =
        tb.spawn(channel_consumer(cancellation_channel, cancelled_probe));
    const auto survivor =
        tb.spawn(channel_consumer(cancellation_channel, survivor_probe));
    const auto putter = tb.spawn(
        channel_put_then_cancel(cancellation_channel, 55, cancelled));
    co_await survivor;
    tb.expect_true("Channel reserved consumer cancellation status",
                   cancelled.cancelled());
    tb.expect_eq("Channel cancelled consumer receives no item",
                 cancelled_probe.count, 0);
    tb.expect_eq("Channel cancellation preserves item count",
                 survivor_probe.count, 1);
    tb.expect_eq("Channel cancellation preserves item value",
                 survivor_probe.values[0], 55);
    tb.expect_true("Channel cancellation producer completes", putter.done());
    tb.expect_true("Channel empty after cancellation handoff",
                   cancellation_channel.empty());
}

Task<void> active_event_waiter(Event& event) {
    co_await event;
}

Task<void> event_active_waiter_violation(ConformanceTb tb) {
    Event event;
    tb.spawn(active_event_waiter(event));
    co_await Delay{1_ps};
}

Task<void> active_channel_waiter(Channel<uint32_t>& channel) {
    static_cast<void>(co_await channel.get());
}

Task<void> channel_active_waiter_violation(ConformanceTb tb) {
    Channel<uint32_t> channel;
    tb.spawn(active_channel_waiter(channel));
    co_await Delay{1_ps};
}

Task<void> subprecision_delay_violation(ConformanceTb) {
    co_await Delay{1_fs};
}

Task<void> output_write_violation(ConformanceTb tb) {
    const Signal dynamic_comb_sum = tb.dut.comb_sum;
    dynamic_comb_sum.set(0xff);
    co_return;
}

Task<void> zero_delay_violation(ConformanceTb) {
    co_await Delay{0_ns};
}

Task<void> invalid_task_timeout_violation(ConformanceTb) {
    static_cast<void>(co_await with_timeout(Task<uint32_t>{}, 1_ps));
}

Task<void> invalid_timeout_value_violation(ConformanceTb) {
    TaskTimeoutProbe probe;
    auto result =
        co_await with_timeout(timeout_value(2_ps, 1, probe), 1_ps);
    static_cast<void>(result.value());
}

Task<void> zero_task_timeout_violation(ConformanceTb) {
    TaskTimeoutProbe probe;
    static_cast<void>(
        co_await with_timeout(timeout_value(1_ps, 1, probe), 0_ps));
}

Task<void> subprecision_task_timeout_violation(ConformanceTb) {
    TaskTimeoutProbe probe;
    static_cast<void>(
        co_await with_timeout(timeout_value(1_ps, 1, probe), 1_fs));
}

Task<void> unobserved_edge_violation(ConformanceTb tb) {
    co_await RisingEdge{tb.dut.comb_sum};
}

Task<void> on_demand_array_bounds_violation(ConformanceTb tb) {
    static_cast<void>(tb.dut.matrix65_i.at(3));
    co_return;
}

Task<void> on_demand_callback_scope_violation(ConformanceTb tb) {
    std::thread outside_callback([dut = tb.dut] {
        static_cast<void>(dut.packed137_i.get());
    });
    outside_callback.join();
    co_return;
}

}  // namespace

void register_user_testbench(ConformanceTb& tb) {
    if (const char* positive_case =
            std::getenv("CPPTB_CONFORMANCE_POSITIVE_CASE")) {
        const std::string_view selected{positive_case};
        if (selected == "timer_r1") {
            tb.sequence(timer_r1_contract);
            return;
        }
        if (selected == "timer_r2") {
            tb.sequence(timer_r2_contract);
            return;
        }
        if (selected == "timer_chained_earlier") {
            tb.sequence(chained_earlier_deadlines_contract);
            return;
        }
        if (selected == "timer_idle_rearm") {
            tb.sequence(idle_timer_rearm_contract);
            return;
        }
    }

    if (const char* negative_case =
            std::getenv("CPPTB_CONFORMANCE_NEGATIVE_CASE")) {
        const std::string_view selected{negative_case};
        if (selected == "event_active_waiter") {
            tb.sequence(event_active_waiter_violation);
            return;
        }
        if (selected == "channel_active_waiter") {
            tb.sequence(channel_active_waiter_violation);
            return;
        }
        if (selected == "invalid_task_timeout") {
            tb.sequence(invalid_task_timeout_violation);
            return;
        }
        if (selected == "invalid_timeout_value") {
            tb.sequence(invalid_timeout_value_violation);
            return;
        }
        if (selected == "zero_task_timeout") {
            tb.sequence(zero_task_timeout_violation);
            return;
        }
        if (selected == "subprecision_task_timeout") {
            tb.sequence(subprecision_task_timeout_violation);
            return;
        }
        if (selected == "unobserved_edge") {
            tb.sequence(unobserved_edge_violation);
            return;
        }
        if (selected == "on_demand_array_bounds") {
            tb.sequence(on_demand_array_bounds_violation);
            return;
        }
        if (selected == "on_demand_callback_scope") {
            tb.sequence(on_demand_callback_scope_violation);
            return;
        }
    }

    tb.sequence(delay_and_settling_contract);
    tb.sequence(packed_signal_contract);
    tb.sequence(unpacked_array_contract);
    tb.sequence(multidimensional_array_contract);
    tb.sequence(internal_probe_contract);
    tb.sequence(force_release_contract);
    tb.sequence(delay_and_first_contract);
    tb.sequence(edge_contract);
    tb.sequence(manual_clock_driver);
    tb.sequence(manual_clock_observer);
    tb.sequence(derived_clock_observer);
    tb.sequence(simultaneous_clock_contract);
    tb.sequence(ordered_waiter_first);
    tb.sequence(ordered_waiter_second);
    tb.sequence(ordered_waiter_verifier);
    tb.sequence(nested_task_contract);
    tb.sequence(signal_observer_contract);
    tb.sequence(process_and_variadic_contract);
    tb.sequence(process_completion_contract);
    tb.sequence(cancellation_contract);
    tb.sequence(same_deadline_delay_contract);
    tb.sequence(earlier_deadline_rearm_contract);
    tb.sequence(stale_falling_interest_contract);
    tb.sequence(typed_task_contract);
    tb.sequence(typed_cancellation_contract);
    tb.sequence(clock_cycles_contract);
    tb.sequence(with_timeout_contract);
    tb.sequence(with_timeout_tie_contract);
    tb.sequence(task_with_timeout_contract);
    tb.sequence(wait_until_contract);
    tb.sequence(event_contract);
    tb.sequence(channel_contract);
}

void register_subprecision_delay_violation(ConformanceTb& tb) {
    tb.sequence(subprecision_delay_violation);
}

void register_output_write_violation(ConformanceTb& tb) {
    tb.sequence(output_write_violation);
}

void register_zero_delay_violation(ConformanceTb& tb) {
    tb.sequence(zero_delay_violation);
}

}  // namespace cpptb::conformance
