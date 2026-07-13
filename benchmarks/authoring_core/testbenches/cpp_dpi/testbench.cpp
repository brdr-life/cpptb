#include "benchmarks/authoring_core/testbenches/cpp_dpi/framework/authoring_core.hpp"

#include <chrono>
#include <cstdio>

namespace cpptb::benchmarks::authoring_core {
namespace {

using coro::Channel;
using coro::Delay;
using coro::Event;
using coro::FallingEdge;
using coro::RisingEdge;
using coro::Task;
using coro::TimeoutOutcome;
using namespace coro;

struct Context {
    coro::Testbench& scheduler;
    AuthoringCoreDut dut;
    uint32_t iterations;
    BenchResult& result;
};

uint32_t stimulus(uint32_t iteration) {
    return ((iteration + 1u) * 0x1f12'3bb5u) ^ 0xc001'd00du;
}

uint32_t expected_response(uint32_t iteration) {
    return (stimulus(iteration) ^ 0xa5a5'5a5au) + iteration;
}

void check(Context& context, const char* label, uint32_t actual,
           uint32_t expected) {
    ++context.result.checks;
    if (actual == expected) return;
    ++context.result.failures;
    if (context.result.failures <= 8) {
        std::printf("AUTHORING_CORE_MISMATCH mode=cpp_dpi kernel=%s "
                    "label=%s actual=0x%08x expected=0x%08x\n",
                    kernel_name(), label, actual, expected);
    }
}

void check64(Context& context, const char* label, uint64_t actual,
             uint64_t expected) {
    ++context.result.checks;
    if (actual == expected) return;
    ++context.result.failures;
    if (context.result.failures <= 8) {
        std::printf("AUTHORING_CORE_MISMATCH mode=cpp_dpi kernel=%s "
                    "label=%s actual=0x%016llx expected=0x%016llx\n",
                    kernel_name(), label,
                    static_cast<unsigned long long>(actual),
                    static_cast<unsigned long long>(expected));
    }
}

void check137(Context& context, const char* label, const Bits<137>& actual,
              const Bits<137>& expected) {
    ++context.result.checks;
    if (actual == expected) return;
    ++context.result.failures;
    if (context.result.failures <= 8) {
        std::printf("AUTHORING_CORE_MISMATCH mode=cpp_dpi kernel=%s "
                    "label=%s actual_word0=0x%08x expected_word0=0x%08x\n",
                    kernel_name(), label, actual.word(0), expected.word(0));
    }
}

void check65(Context& context, const char* label, const Bits<65>& actual,
             const Bits<65>& expected) {
    ++context.result.checks;
    if (actual == expected) return;
    ++context.result.failures;
    if (context.result.failures <= 8) {
        std::printf("AUTHORING_CORE_MISMATCH mode=cpp_dpi kernel=%s "
                    "label=%s actual_word0=0x%08x expected_word0=0x%08x\n",
                    kernel_name(), label, actual.word(0), expected.word(0));
    }
}

uint64_t wide64_stimulus(uint32_t iteration) {
    return static_cast<uint64_t>(stimulus(iteration * 2u)) |
           (static_cast<uint64_t>(stimulus(iteration * 2u + 1u)) << 32u);
}

Bits<137> wide137_stimulus(uint32_t iteration) {
    Bits<137> value;
    for (uint32_t word = 0; word < 5; ++word) {
        value.set_word(word, stimulus(iteration * 5u + word));
    }
    return value;
}

Bits<137> wide137_response(Bits<137> value) {
    constexpr uint32_t mask_words[] = {
        0xdead'beefu, 0x89ab'cdefu, 0x0123'4567u, 0x5aa5'5aa5u,
        0x0000'01a5u,
    };
    for (uint32_t word = 0; word < 5; ++word) {
        value.set_word(word, value.word(word) ^ mask_words[word]);
    }
    return value;
}

Bits<65> array_multidim_stimulus(uint32_t iteration, int32_t row,
                                 int32_t column) {
    const uint32_t ordinal =
        static_cast<uint32_t>((2 - row) * 3 + column + 1);
    const uint32_t element = iteration * 6u + ordinal;
    Bits<65> value;
    value.set_word(0, stimulus(element * 3u));
    value.set_word(1, stimulus(element * 3u + 1u));
    value.set_word(2, stimulus(element * 3u + 2u) & 1u);
    return value;
}

Bits<65> array_multidim_response(Bits<65> value) {
    constexpr uint32_t mask_words[] = {
        0x89ab'cdefu, 0x0123'4567u, 1u,
    };
    for (uint32_t word = 0; word < 3; ++word) {
        value.set_word(word, value.word(word) ^ mask_words[word]);
    }
    return value;
}

Task<void> wide64_feature(Context& context, uint32_t iteration) {
    ++context.result.features.wide64;
    const uint64_t value = wide64_stimulus(iteration);
    co_await FallingEdge{context.dut.clk};
    context.dut.wide64_i.set(value);
    co_await RisingEdge{context.dut.clk};
    co_await Delay{1_ps};
    check64(context, "wide64", context.dut.wide64_o.get(),
            value ^ 0xd1b5'4a32'd192'ed03ull);
}

Task<void> wide137_feature(Context& context, uint32_t iteration) {
    ++context.result.features.wide_echo_137;
    const auto value = wide137_stimulus(iteration);
    co_await FallingEdge{context.dut.clk};
    context.dut.wide137_i.set(value);
    co_await RisingEdge{context.dut.clk};
    co_await Delay{1_ps};
    check137(context, "wide137", context.dut.wide137_o.get(),
             wide137_response(value));
}

Task<void> wide_slice_feature(Context& context, uint32_t iteration) {
    ++context.result.features.wide_slice;
    auto value = wide137_stimulus(iteration);
    const uint64_t replacement = wide64_stimulus(iteration) ^
                                 0x4f1b'bcdd'812f'6a31ull;
    value.set_slice<64>(37, Bits<64>::from_uint(replacement));
    co_await FallingEdge{context.dut.clk};
    context.dut.wide137_i.set(value);
    co_await RisingEdge{context.dut.clk};
    co_await Delay{1_ps};
    const auto actual = context.dut.wide137_o.get().template slice<64>(37);
    const auto expected = wide137_response(value).template slice<64>(37);
    check64(context, "wide slice", actual.to_uint64(), expected.to_uint64());
}

Task<void> fixed_mac_feature(Context& context, uint32_t iteration) {
    using Q2_14 = Fixed<16, 2, Signedness::Signed>;

    ++context.result.features.fixed_mac;
    const auto a = Q2_14::from_raw(
        static_cast<uint16_t>(stimulus(iteration * 2u)));
    const auto b = Q2_14::from_raw(
        static_cast<uint16_t>(stimulus(iteration * 2u + 1u)));
    const auto product = mul_full(a, b);
    const auto expected = quantize<Q2_14>(
        product, Round::NearestEven, Overflow::Saturate);

    co_await FallingEdge{context.dut.clk};
    context.dut.fixed_a_i.set(a.raw().to_uint());
    context.dut.fixed_b_i.set(b.raw().to_uint());
    co_await RisingEdge{context.dut.clk};
    co_await Delay{1_ps};
    check(context, "fixed mac", context.dut.fixed_y_o.get(),
          expected.raw().to_uint());
}

Task<void> array_index_feature(Context& context, uint32_t iteration) {
    ++context.result.features.array_index;
    co_await FallingEdge{context.dut.clk};
    for (int32_t index = 1; index <= 8; ++index) {
        context.dut.array_i.at(index).set(
            stimulus(iteration * 8u + static_cast<uint32_t>(index - 1)));
    }
    co_await RisingEdge{context.dut.clk};
    co_await Delay{1_ps};
    for (int32_t index = 1; index <= 8; ++index) {
        const uint32_t value =
            stimulus(iteration * 8u + static_cast<uint32_t>(index - 1));
        check(context, "array index", context.dut.array_o.at(index).get(),
              value ^ (0x6d2b'79f5u + static_cast<uint32_t>(index)));
    }
}

Task<void> array_wide_feature(Context& context, uint32_t iteration) {
    ++context.result.features.array_wide;
    co_await FallingEdge{context.dut.clk};
    for (int32_t index = 0; index <= 3; ++index) {
        context.dut.array_wide_i.at(index).set(
            wide64_stimulus(iteration * 4u + static_cast<uint32_t>(index)));
    }
    co_await RisingEdge{context.dut.clk};
    co_await Delay{1_ps};
    for (int32_t index = 0; index <= 3; ++index) {
        const uint64_t value =
            wide64_stimulus(iteration * 4u + static_cast<uint32_t>(index));
        check64(context, "wide array",
                context.dut.array_wide_o.at(index).get(),
                value ^ (0x9e37'79b9'7f4a'7c15ull +
                         static_cast<uint64_t>(index)));
    }
}

Task<void> array_multidim_feature(Context& context, uint32_t iteration) {
    ++context.result.features.array_multidim;
    co_await FallingEdge{context.dut.clk};
    for (int32_t row = 2; row >= 1; --row) {
        for (int32_t column = -1; column <= 1; ++column) {
            context.dut.array_multidim_i.at(row).at(column).set(
                array_multidim_stimulus(iteration, row, column));
        }
    }
    co_await RisingEdge{context.dut.clk};
    co_await Delay{1_ps};
    for (int32_t row = 2; row >= 1; --row) {
        for (int32_t column = -1; column <= 1; ++column) {
            const auto value =
                array_multidim_stimulus(iteration, row, column);
            check65(context, "multidimensional array",
                    context.dut.array_multidim_o.at(row).at(column).get(),
                    array_multidim_response(value));
        }
    }
}

Task<void> mem_rw_feature(Context& context, uint32_t iteration) {
    ++context.result.features.mem_rw;
    const uint32_t address = iteration & 0xffu;
    const uint32_t value = stimulus(iteration) ^ 0x51a7'c3e9u;

    co_await FallingEdge{context.dut.clk};
    context.dut.mem_addr_i.set(address);
    context.dut.mem_wdata_i.set(value);
    context.dut.mem_we_i.set(1);
    co_await RisingEdge{context.dut.clk};
    co_await FallingEdge{context.dut.clk};
    context.dut.mem_we_i.set(0);
    co_await RisingEdge{context.dut.clk};
    co_await Delay{1_ps};
    check(context, "memory read", context.dut.mem_rdata_o.get(), value);
}

Task<void> hier_probe_feature(Context& context, uint32_t iteration,
                              uint32_t& previous_cycle_count) {
    const uint32_t value = stimulus(iteration) ^ 0x3c6e'f372u;

    co_await FallingEdge{context.dut.clk};
    ++context.result.features.hier_probe_reads;
    const uint32_t cycle_count = context.dut.internal.cycle_count.get();
    check(context, "hierarchical cycle count",
          iteration == 0 || cycle_count > previous_cycle_count, 1);
    previous_cycle_count = cycle_count;

    ++context.result.features.hier_probe_deposits;
    context.dut.internal.pending_data.deposit(value);
    co_await Delay{1_ps};

    ++context.result.features.hier_probe_reads;
    check(context, "hierarchical deposit",
          context.dut.internal.pending_data.get(), value);
}

Task<void> mem_backdoor_feature(Context& context, uint32_t iteration) {
    const int32_t address = static_cast<int32_t>(iteration & 0xffu);
    const uint32_t value = stimulus(iteration) ^ 0x8a5c'19d7u;

    co_await FallingEdge{context.dut.clk};
    ++context.result.features.mem_backdoor_deposits;
    context.dut.internal.memory.at(address).deposit(value);
    co_await Delay{1_ps};

    ++context.result.features.mem_backdoor_reads;
    check(context, "memory backdoor read",
          context.dut.internal.memory.at(address).get(), value);

    context.dut.mem_addr_i.set(static_cast<uint32_t>(address));
    context.dut.mem_we_i.set(0);
    co_await RisingEdge{context.dut.clk};
    co_await Delay{1_ps};
    check(context, "memory backdoor frontdoor visibility",
          context.dut.mem_rdata_o.get(), value);
}

uint32_t probe_memory_seed(uint32_t address) {
    return 0x1357'9bdfu ^ (address * 0x9e37'79b9u);
}

uint32_t probe_memory_value(uint32_t iteration) {
    return stimulus(iteration) ^ 0x6a09'e667u;
}

Task<void> seed_probe_memory(Context& context) {
    for (uint32_t address = 0; address < 256; ++address) {
        co_await FallingEdge{context.dut.clk};
        context.dut.mem_addr_i.set(address);
        context.dut.mem_wdata_i.set(probe_memory_seed(address));
        context.dut.mem_we_i.set(1);
        co_await RisingEdge{context.dut.clk};
    }
    co_await FallingEdge{context.dut.clk};
    context.dut.mem_we_i.set(0);
}

Task<void> mem_probe_read_feature(Context& context, uint32_t iteration) {
    const int32_t address = static_cast<int32_t>(iteration & 0xffu);
    const uint32_t expected = probe_memory_seed(
        static_cast<uint32_t>(address));

    co_await FallingEdge{context.dut.clk};
    context.dut.mem_addr_i.set(static_cast<uint32_t>(address));
    ++context.result.features.probe_diag_reads;
    const uint32_t internal = context.dut.internal.memory.at(address).get();

    // The testbench, not deposit/get, chooses its observation boundaries.
    co_await Delay{1_ps};
    check(context, "memory probe read", internal, expected);
    co_await RisingEdge{context.dut.clk};
    co_await Delay{1_ps};
    check(context, "memory probe read frontdoor", context.dut.mem_rdata_o.get(),
          expected);
}

Task<void> mem_probe_deposit_feature(Context& context, uint32_t iteration) {
    const int32_t address = static_cast<int32_t>(iteration & 0xffu);
    const uint32_t value = probe_memory_value(iteration);

    co_await FallingEdge{context.dut.clk};
    context.dut.mem_addr_i.set(static_cast<uint32_t>(address));
    ++context.result.features.probe_diag_deposits;
    context.dut.internal.memory.at(address).deposit(value);

    co_await Delay{1_ps};
    co_await RisingEdge{context.dut.clk};
    co_await Delay{1_ps};
    check(context, "memory probe deposit frontdoor",
          context.dut.mem_rdata_o.get(), value);
}

Task<void> mem_probe_read_deposit_feature(Context& context,
                                          uint32_t iteration) {
    const int32_t address = static_cast<int32_t>(iteration & 0xffu);
    const uint32_t expected_before = iteration < 256
        ? probe_memory_seed(static_cast<uint32_t>(address))
        : probe_memory_value(iteration - 256);
    const uint32_t value = probe_memory_value(iteration);

    co_await FallingEdge{context.dut.clk};
    context.dut.mem_addr_i.set(static_cast<uint32_t>(address));
    ++context.result.features.probe_diag_reads;
    const uint32_t internal = context.dut.internal.memory.at(address).get();
    ++context.result.features.probe_diag_deposits;
    context.dut.internal.memory.at(address).deposit(value);

    co_await Delay{1_ps};
    check(context, "memory probe read before deposit", internal,
          expected_before);
    co_await RisingEdge{context.dut.clk};
    co_await Delay{1_ps};
    check(context, "memory probe read/deposit frontdoor",
          context.dut.mem_rdata_o.get(), value);
}

Task<void> force_release_feature(Context& context, uint32_t iteration) {
    constexpr uint32_t driver_mask = 0x5a5a'a5a5u;
    const uint32_t source = stimulus(iteration);
    const uint32_t forced = source ^ 0xa5a5'5a5au;

    ++context.result.features.force_release;
    context.dut.force_source_i.set(source);
    context.dut.internal.force_target.force(forced);
    co_await Delay{1_ps};
    check(context, "forced internal net fanout",
          context.dut.force_fanout_o.get(), forced);

    context.dut.internal.force_target.release();
    co_await Delay{1_ps};
    check(context, "released internal net driver",
          context.dut.force_fanout_o.get(), source ^ driver_mask);
}

Task<void> packed_view_feature(Context& context, uint32_t iteration) {
    const uint32_t opcode = iteration & 0x7u;
    const uint32_t tag = iteration & 0x3u;
    const uint32_t payload = stimulus(iteration) & 0x7u;

    ++context.result.features.packed_view;
    auto packet = PacketTValue::from_signal_value(0);
    packet.set_opcode(Bits<3>::from_uint(opcode)).set_state(StateT::StateRun);
    packet.view().inner()
        .set_tag(Bits<2>::from_uint(tag))
        .set_payload(Bits<3>::from_uint(payload));
    context.dut.packed_view_i.set(packet.signal_value());
    co_await Delay{1_ps};

    const auto actual = PacketTValue::from_signal_value(
        context.dut.packed_view_o.get());
    check(context, "packed view opcode", actual.opcode().to_uint(),
          opcode ^ 0x3u);
    check(context, "packed view signed enum",
          actual.state().is(StateT::StateRun), 1);
    check(context, "packed view nested tag", actual.inner().tag().to_uint(),
          (tag + 1u) & 0x3u);
    check(context, "packed view nested payload",
          actual.inner().payload().to_uint(), payload ^ 0x5u);
}

Task<uint32_t> authored_value(uint32_t iteration) {
    co_return stimulus(iteration);
}

Task<uint32_t> delayed_authored_value(uint32_t iteration, SimTime delay) {
    co_await Delay{delay};
    co_return stimulus(iteration);
}

Task<void> wait_ready_raw(Context& context) {
    while (context.dut.req_ready.get() == 0) {
        co_await RisingEdge{context.dut.clk};
    }
}

Task<void> transact(Context& context, uint32_t iteration, uint32_t payload,
                    bool ready_already = false) {
    if (!ready_already) co_await wait_ready_raw(context);

    co_await FallingEdge{context.dut.clk};
    context.dut.req_data.set(payload);
    context.dut.req_valid.set(1);

    co_await RisingEdge{context.dut.clk};
    co_await FallingEdge{context.dut.clk};
    context.dut.req_valid.set(0);

    while (true) {
        co_await RisingEdge{context.dut.clk};
        co_await Delay{1_ps};
        if (context.dut.rsp_valid.get() != 0) break;
    }

    const uint32_t response = context.dut.rsp_data.get();
    check(context, "response", response, expected_response(iteration));
    context.result.checksum =
        (context.result.checksum ^ response) * 0x0100'0193u;
    ++context.result.transactions;
}

Task<void> transact_signal_edge(Context& context, uint32_t iteration,
                                uint32_t payload) {
    co_await wait_ready_raw(context);

    co_await FallingEdge{context.dut.clk};
    context.dut.req_data.set(payload);
    context.dut.req_valid.set(1);

    co_await RisingEdge{context.dut.clk};
    co_await FallingEdge{context.dut.clk};
    context.dut.req_valid.set(0);

    co_await RisingEdge{context.dut.rsp_valid};
    ++context.result.features.signal_edges;

    const uint32_t response = context.dut.rsp_data.get();
    check(context, "response", response, expected_response(iteration));
    context.result.checksum =
        (context.result.checksum ^ response) * 0x0100'0193u;
    ++context.result.transactions;
}

Task<bool> timeout_probe(Context& context, uint32_t iteration) {
    ++context.result.features.timeouts;
    const bool expect_timeout = (iteration & 1u) != 0;
    const auto outcome = co_await with_timeout(
        RisingEdge{context.dut.clk}, expect_timeout ? 500_ps : 3_ns);
    const bool timed_out = outcome == TimeoutOutcome::TimedOut;
    if (timed_out) ++context.result.features.timeout_hits;
    check(context, "timeout outcome", timed_out, expect_timeout);
    co_return timed_out;
}

Task<uint32_t> task_timeout_probe(Context& context, uint32_t iteration,
                                  uint32_t fallback) {
    ++context.result.features.task_timeouts;
    const bool expect_timeout = (iteration & 1u) != 0;
    const auto outcome = co_await with_timeout(
        delayed_authored_value(iteration, expect_timeout ? 3_ns : 500_ps),
        expect_timeout ? 500_ps : 3_ns);
    const bool valid_outcome =
        expect_timeout ? outcome.timed_out() : outcome.triggered();
    if (outcome.timed_out()) {
        ++context.result.features.task_timeout_hits;
    }
    check(context, "task timeout outcome", valid_outcome, 1);
    co_return outcome.triggered() ? outcome.value() : fallback;
}

Task<void> event_roundtrip(Context& context, Event& event) {
    ++context.result.features.event_set;
    event.set();
    ++context.result.features.event_wait;
    co_await event;
    check(context, "event sticky state", event.is_set(), 1);
    event.clear();
}

Task<void> wait_for_response_count(Context& context) {
    while (context.dut.response_count.get() != context.iterations) {
        co_await RisingEdge{context.dut.clk};
        co_await Delay{1_ps};
    }
}

uint64_t reported_sim_cycles(Context& context) {
    constexpr uint64_t one_ns_fs = 1'000'000u;
    constexpr uint64_t clock_period_fs = 2'000'000u;
    return (context.scheduler.now().in_femtoseconds() + one_ns_fs) /
           clock_period_fs;
}

void report(Context& context) {
    const auto elapsed = std::chrono::steady_clock::now() - context.result.start;
    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    const auto& feature = context.result.features;
    std::printf(
        "AUTHORING_CORE_RESULT mode=cpp_dpi kernel=%s iterations=%u "
        "transactions=%llu checks=%llu sim_cycles=%llu checksum=%u failures=%u "
        "task_value=%llu clock_cycles=%llu timeouts=%llu timeout_hits=%llu "
        "task_timeouts=%llu task_timeout_hits=%llu "
        "wait_until=%llu event_set=%llu event_wait=%llu channel_send=%llu "
        "channel_receive=%llu wide64=%llu wide_echo_137=%llu "
        "wide_slice=%llu fixed_mac=%llu array_index=%llu "
        "array_wide=%llu array_multidim=%llu mem_rw=%llu "
        "hier_probe_reads=%llu "
        "hier_probe_deposits=%llu mem_backdoor_reads=%llu "
        "mem_backdoor_deposits=%llu probe_diag_reads=%llu "
        "probe_diag_deposits=%llu signal_edges=%llu force_release=%llu "
        "packed_view=%llu "
        "wall_ms=%.3f\n",
        kernel_name(), context.iterations,
        static_cast<unsigned long long>(context.result.transactions),
        static_cast<unsigned long long>(context.result.checks),
        static_cast<unsigned long long>(reported_sim_cycles(context)),
        context.result.checksum, context.result.failures,
        static_cast<unsigned long long>(feature.task_value),
        static_cast<unsigned long long>(feature.clock_cycles),
        static_cast<unsigned long long>(feature.timeouts),
        static_cast<unsigned long long>(feature.timeout_hits),
        static_cast<unsigned long long>(feature.task_timeouts),
        static_cast<unsigned long long>(feature.task_timeout_hits),
        static_cast<unsigned long long>(feature.wait_until),
        static_cast<unsigned long long>(feature.event_set),
        static_cast<unsigned long long>(feature.event_wait),
        static_cast<unsigned long long>(feature.channel_send),
        static_cast<unsigned long long>(feature.channel_receive),
        static_cast<unsigned long long>(feature.wide64),
        static_cast<unsigned long long>(feature.wide_echo_137),
        static_cast<unsigned long long>(feature.wide_slice),
        static_cast<unsigned long long>(feature.fixed_mac),
        static_cast<unsigned long long>(feature.array_index),
        static_cast<unsigned long long>(feature.array_wide),
        static_cast<unsigned long long>(feature.array_multidim),
        static_cast<unsigned long long>(feature.mem_rw),
        static_cast<unsigned long long>(feature.hier_probe_reads),
        static_cast<unsigned long long>(feature.hier_probe_deposits),
        static_cast<unsigned long long>(feature.mem_backdoor_reads),
        static_cast<unsigned long long>(feature.mem_backdoor_deposits),
        static_cast<unsigned long long>(feature.probe_diag_reads),
        static_cast<unsigned long long>(feature.probe_diag_deposits),
        static_cast<unsigned long long>(feature.signal_edges),
        static_cast<unsigned long long>(feature.force_release),
        static_cast<unsigned long long>(feature.packed_view),
        static_cast<double>(elapsed_us) / 1000.0);
}

Task<void> run(Context context) {
    context.dut.rst_n.set(0);
    context.dut.req_valid.set(0);
    context.dut.req_data.set(0);
    context.dut.rsp_ready.set(1);
    context.dut.wide64_i.set(0);
    context.dut.wide137_i.set(Bits<137>{});
    context.dut.force_source_i.set(0);
    context.dut.packed_view_i.set(0);
    context.dut.fixed_a_i.set(0);
    context.dut.fixed_b_i.set(0);
    for (int32_t index = 1; index <= 8; ++index) {
        context.dut.array_i.at(index).set(0);
    }
    for (int32_t index = 0; index <= 3; ++index) {
        context.dut.array_wide_i.at(index).set(0);
    }
    for (int32_t row = 2; row >= 1; --row) {
        for (int32_t column = -1; column <= 1; ++column) {
            context.dut.array_multidim_i.at(row).at(column).set(Bits<65>{});
        }
    }
    context.dut.mem_addr_i.set(0);
    context.dut.mem_wdata_i.set(0);
    context.dut.mem_we_i.set(0);
    for (uint32_t cycle = 0; cycle < 4; ++cycle) {
        co_await RisingEdge{context.dut.clk};
    }
    context.dut.rst_n.set(1);

    Event event;
    Channel<uint32_t> channel;
    uint32_t previous_cycle_count = 0;

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEM_PROBE_READ || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEM_PROBE_DEPOSIT || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEM_PROBE_READ_DEPOSIT
    co_await seed_probe_memory(context);
#endif

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        uint32_t payload = stimulus(iteration);

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_TASK_VALUE || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        payload = co_await authored_value(iteration);
        ++context.result.features.task_value;
        check(context, "task value", payload, stimulus(iteration));
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_CLOCK_CYCLES || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        ++context.result.features.clock_cycles;
        co_await clock_cycles(context.dut.clk, 1);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_TIMEOUT || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        static_cast<void>(co_await timeout_probe(context, iteration));
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_TASK_TIMEOUT || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        payload = co_await task_timeout_probe(context, iteration, payload);
#endif

        bool ready_already = false;
#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_WAIT_UNTIL || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        ++context.result.features.wait_until;
        co_await wait_until(context.dut.req_ready,
                            [](uint32_t value) { return value != 0; },
                            context.dut.clk);
        check(context, "wait_until ready", context.dut.req_ready.get(), 1);
        ready_already = true;
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_EVENT || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        co_await event_roundtrip(context, event);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_CHANNEL || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        ++context.result.features.channel_send;
        channel.put_nowait(payload);
        ++context.result.features.channel_receive;
        const uint32_t received = co_await channel.get();
        check(context, "channel payload", received, payload);
        payload = received;
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_WIDE64 || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        co_await wide64_feature(context, iteration);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_WIDE_ECHO_137 || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        co_await wide137_feature(context, iteration);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_WIDE_SLICE || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        co_await wide_slice_feature(context, iteration);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_FIXED_MAC || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        co_await fixed_mac_feature(context, iteration);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ARRAY_INDEX || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        co_await array_index_feature(context, iteration);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ARRAY_WIDE || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        co_await array_wide_feature(context, iteration);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ARRAY_MULTIDIM
        co_await array_multidim_feature(context, iteration);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEM_RW || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        co_await mem_rw_feature(context, iteration);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_HIER_PROBE || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        co_await hier_probe_feature(context, iteration, previous_cycle_count);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEM_BACKDOOR || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        co_await mem_backdoor_feature(context, iteration);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEM_PROBE_READ
        co_await mem_probe_read_feature(context, iteration);
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEM_PROBE_DEPOSIT
        co_await mem_probe_deposit_feature(context, iteration);
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEM_PROBE_READ_DEPOSIT
        co_await mem_probe_read_deposit_feature(context, iteration);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_FORCE_RELEASE
        co_await force_release_feature(context, iteration);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_PACKED_VIEW
        co_await packed_view_feature(context, iteration);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_SIGNAL_EDGE
        co_await transact_signal_edge(context, iteration, payload);
#else
        co_await transact(context, iteration, payload, ready_already);
#endif
    }

    co_await wait_for_response_count(context);
    check(context, "request count", context.dut.request_count.get(),
          context.iterations);
    check(context, "response count", context.dut.response_count.get(),
          context.iterations);
    report(context);
}

}  // namespace

void register_benchmark(coro::Testbench& scheduler, AuthoringCoreDut dut,
                        uint32_t iterations, BenchResult& result) {
    result = BenchResult{};
    result.start = std::chrono::steady_clock::now();
    scheduler.spawn_detached(run(Context{scheduler, dut, iterations, result}));
}

}  // namespace cpptb::benchmarks::authoring_core
