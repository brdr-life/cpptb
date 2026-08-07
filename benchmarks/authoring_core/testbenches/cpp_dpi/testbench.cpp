#include "benchmarks/authoring_core/testbenches/cpp_dpi/framework/authoring_core.hpp"

#include <chrono>
#include <array>
#include <cstdio>
#include <limits>

namespace cpptb::benchmarks::authoring_core {
namespace {

using coro::Queue;
using coro::Delay;
using coro::Event;
using coro::FallingEdge;
using coro::Lock;
using coro::RisingEdge;
using coro::Semaphore;
using coro::Task;
using coro::TimeoutOutcome;
using namespace coro;
using namespace cpptb::vc;

struct Context {
    coro::Testbench& scheduler;
    AuthoringCoreDut dut;
    uint32_t iterations;
    BenchResult& result;
    cpptb::detail::SimLogEndpoint* sim_logs = nullptr;
};

struct CoverageTransaction {
    uint8_t opcode;
    uint16_t length;
};

uint32_t stimulus(uint32_t iteration) {
    return ((iteration + 1u) * 0x1f12'3bb5u) ^ 0xc001'd00du;
}

uint32_t expected_response(uint32_t iteration) {
    return (stimulus(iteration) ^ 0xa5a5'5a5au) + iteration;
}

uint32_t expected_response(uint32_t iteration, uint32_t payload) {
    return (payload ^ 0xa5a5'5a5au) + iteration;
}

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_CONSTRAINED_PACKET
class PacketStimulus final : public Randomized {
   public:
    Rand<uint8_t> opcode{*this, "opcode"};
    Rand<uint16_t> length{*this, "length"};
    Rand<uint16_t> address{*this, "address"};
    Rand<uint8_t> tag{*this, "tag"};

    PacketStimulus() {
        constraint("supported opcode", opcode <= uint8_t{6});
        constraint("packet length",
                   length >= uint16_t{64} && length <= uint16_t{1500});
        constraint("address window",
                   address >= uint16_t{0x1000} &&
                       address <= uint16_t{0x1fff});
        constraint("word-sized packet", length % uint16_t{4} == uint16_t{0});
        constraint("aligned address",
                   address % uint16_t{4} == uint16_t{0});
        constraint("short control packet",
                   opcode != uint8_t{6} || length <= uint16_t{256});
    }

    uint32_t payload() const {
        return (static_cast<uint32_t>(opcode.get()) << 29u) ^
               (static_cast<uint32_t>(length.get()) << 16u) ^
               (static_cast<uint32_t>(address.get()) << 1u) ^ tag.get();
    }
};
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_CONSTRAINT_EXTENSIONS
class ExtensionHeader final : public Randomized {
   public:
    Rand<uint8_t> route{*this, "route"};

    explicit ExtensionHeader(Randomized& parent)
        : Randomized(parent, "header") {
        soft_constraint("default route", route == uint8_t{2});
    }
};

class ExtendedPacketStimulus final : public Randomized {
   public:
    Rand<uint8_t> opcode{*this, "opcode"};
    Rand<uint16_t> length{*this, "length"};
    ExtensionHeader header{*this};
    RandArray<uint8_t, 2> bytes{*this, "bytes"};
    RandBits<65> token{*this, "token"};

    ExtendedPacketStimulus() {
        constraint("selected opcode", inside(opcode, {1, 3, 5}));
        distribution(
            "packet length mix",
            dist(length, weighted(uint16_t{64}, 1),
                 weighted(range(uint16_t{128}, uint16_t{131}), 3)));
        constraint("distinct byte prefix", bytes[0] != bytes[1]);
        constraint("high token bit", token.word(2) == uint32_t{1});
        auto legacy_opcode =
            constraint("disabled legacy opcode", opcode == uint8_t{7});
        legacy_opcode.disable();
    }

    uint32_t payload() const {
        const auto byte_values = bytes.get();
        const auto token_value = token.get();
        return (static_cast<uint32_t>(opcode.get()) << 29u) ^
               (static_cast<uint32_t>(length.get()) << 16u) ^
               (static_cast<uint32_t>(header.route.get()) << 24u) ^
               (static_cast<uint32_t>(byte_values[0]) << 8u) ^
               byte_values[1] ^ token_value.word(0) ^ token_value.word(1) ^
               (token_value.word(2) << 31u);
    }
};
#endif

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

void check128(Context& context, const char* label, const Bits<128>& actual,
              const Bits<128>& expected) {
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
    const uint32_t cycle_count = context.dut.cycle_count.get();
    check(context, "hierarchical cycle count",
          iteration == 0 || cycle_count > previous_cycle_count, 1);
    previous_cycle_count = cycle_count;

    ++context.result.features.hier_probe_deposits;
    context.dut.pending_data.deposit(value);
    co_await Delay{1_ps};

    ++context.result.features.hier_probe_reads;
    check(context, "hierarchical deposit",
          context.dut.pending_data.get(), value);
}

Task<void> hier_data_feature(Context& context, uint32_t iteration) {
    const auto wide = wide137_stimulus(iteration);
    const auto logic = LogicBits<4>::from_uint(stimulus(iteration) & 0xfu);

    co_await FallingEdge{context.dut.clk};
    ++context.result.features.hier_data_deposits;
    context.dut.hierarchy_wide.deposit(wide);
    ++context.result.features.hier_data_reads;
    check137(context, "hierarchy wide data", context.dut.hierarchy_wide.get(),
             wide);

    ++context.result.features.hier_data_deposits;
    context.dut.hierarchy_logic.deposit_logic(logic);
    ++context.result.features.hier_data_reads;
    check(context, "hierarchy four-state data",
          context.dut.hierarchy_logic.get_logic() == logic, 1);
}

Task<void> mem_backdoor_feature(Context& context, uint32_t iteration) {
    const int32_t address = static_cast<int32_t>(iteration & 0xffu);
    const uint32_t value = stimulus(iteration) ^ 0x8a5c'19d7u;

    co_await FallingEdge{context.dut.clk};
    ++context.result.features.mem_backdoor_deposits;
    context.dut.memory.at(address).deposit(value);
    co_await Delay{1_ps};

    ++context.result.features.mem_backdoor_reads;
    check(context, "memory backdoor read",
          context.dut.memory.at(address).get(), value);

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
    const uint32_t internal = context.dut.memory.at(address).get();

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
    context.dut.memory.at(address).deposit(value);

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
    const uint32_t internal = context.dut.memory.at(address).get();
    ++context.result.features.probe_diag_deposits;
    context.dut.memory.at(address).deposit(value);

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
    context.dut.force_target.force(forced);
    co_await Delay{1_ps};
    check(context, "forced internal net fanout",
          context.dut.force_fanout_o.get(), forced);

    context.dut.force_target.release();
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
    check(context, "response", response,
          expected_response(iteration, payload));
    context.result.checksum =
        (context.result.checksum ^ response) * 0x0100'0193u;
    ++context.result.transactions;
}

Task<void> drive_request(Context& context, uint32_t payload) {
    co_await wait_ready_raw(context);

    co_await FallingEdge{context.dut.clk};
    context.dut.req_data.set(payload);
    context.dut.req_valid.set(1);

    co_await RisingEdge{context.dut.clk};
    co_await FallingEdge{context.dut.clk};
    context.dut.req_valid.set(0);
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
    check(context, "response", response,
          expected_response(iteration, payload));
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
    for (std::size_t index = 0;
         index < context.result.failure_records.size() && index < 8; ++index) {
        const auto& failure = context.result.failure_records[index];
        std::printf("AUTHORING_CORE_MISMATCH mode=cpp_dpi kernel=%s "
                    "label=%s actual=%s expected=%s\n",
                    kernel_name(), failure.label.c_str(),
                    failure.actual.c_str(), failure.expected.c_str());
    }
    std::printf(
        "AUTHORING_CORE_RESULT mode=cpp_dpi kernel=%s iterations=%u "
        "transactions=%llu checks=%llu sim_cycles=%llu spawned_processes=%llu "
        "checksum=%u failures=%u "
        "task_value=%llu clock_cycles=%llu timeouts=%llu timeout_hits=%llu "
        "task_timeouts=%llu task_timeout_hits=%llu "
        "wait_until=%llu event_set=%llu event_wait=%llu queue_send=%llu "
        "queue_receive=%llu queue_put=%llu queue_get=%llu "
        "lock_acquire=%llu semaphore_acquire=%llu "
        "wide64=%llu wide_echo_137=%llu "
        "wide_slice=%llu fixed_mac=%llu array_index=%llu "
        "array_wide=%llu array_multidim=%llu mem_rw=%llu "
        "hier_probe_reads=%llu "
        "hier_probe_deposits=%llu mem_backdoor_reads=%llu "
        "mem_backdoor_deposits=%llu probe_diag_reads=%llu "
        "probe_diag_deposits=%llu signal_edges=%llu force_release=%llu "
        "packed_view=%llu hier_data_reads=%llu hier_data_deposits=%llu "
        "timing_phases=%llu test_lifecycle=%llu dynamic_spawn=%llu "
        "analysis_write=%llu analysis_delivery=%llu random_stimulus=%llu "
        "constrained_packet=%llu constraint_extensions=%llu "
        "coverage_sampling=%llu apb_component=%llu memory_model=%llu "
        "memory_model_direct=%llu register_prediction_validity=%llu "
        "register_backdoor=%llu register_hierarchy=%llu register_split=%llu "
        "register_wide=%llu register_enum=%llu "
        "wall_ms=%.3f\n",
        kernel_name(), context.iterations,
        static_cast<unsigned long long>(context.result.transactions),
        static_cast<unsigned long long>(context.result.checks),
        static_cast<unsigned long long>(reported_sim_cycles(context)),
        static_cast<unsigned long long>(context.result.spawned_processes),
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
        static_cast<unsigned long long>(feature.queue_send),
        static_cast<unsigned long long>(feature.queue_receive),
        static_cast<unsigned long long>(feature.queue_put),
        static_cast<unsigned long long>(feature.queue_get),
        static_cast<unsigned long long>(feature.lock_acquire),
        static_cast<unsigned long long>(feature.semaphore_acquire),
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
        static_cast<unsigned long long>(feature.hier_data_reads),
        static_cast<unsigned long long>(feature.hier_data_deposits),
        static_cast<unsigned long long>(feature.timing_phases),
        static_cast<unsigned long long>(feature.test_lifecycle),
        static_cast<unsigned long long>(feature.dynamic_spawn),
        static_cast<unsigned long long>(feature.analysis_write),
        static_cast<unsigned long long>(feature.analysis_delivery),
        static_cast<unsigned long long>(feature.random_stimulus),
        static_cast<unsigned long long>(feature.constrained_packet),
        static_cast<unsigned long long>(feature.constraint_extensions),
        static_cast<unsigned long long>(feature.coverage_sampling),
        static_cast<unsigned long long>(feature.apb_component),
        static_cast<unsigned long long>(feature.memory_model),
        static_cast<unsigned long long>(feature.memory_model_direct),
        static_cast<unsigned long long>(feature.register_prediction_validity),
        static_cast<unsigned long long>(feature.register_backdoor),
        static_cast<unsigned long long>(feature.register_hierarchy),
        static_cast<unsigned long long>(feature.register_split),
        static_cast<unsigned long long>(feature.register_wide),
        static_cast<unsigned long long>(feature.register_enum),
        static_cast<double>(elapsed_us) / 1000.0);
}

Task<void> lifecycle_process(TestContext test, uint32_t iterations) {
    for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
        test.expect_eq("owned process value", stimulus(iteration),
                       stimulus(iteration));
    }
    co_return;
}

Task<void> run_test_lifecycle(Context context) {
    TestContext test{context.scheduler, context.result};
    ++context.result.spawned_processes;
    auto process =
        test.spawn(lifecycle_process(test, context.iterations));
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint32_t value = stimulus(iteration);
        ++context.result.features.test_lifecycle;
        test.expect("stimulus is nonzero", value != 0);
        test.expect_eq("stimulus identity", value, stimulus(iteration));
    }
    co_await process;
    co_await Delay{1_ps};
    report(context);
}

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_STRUCTURED_LOGGING || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_STRUCTURED_LOG_HISTORY
struct StructuredLogSink final : LogSink {
    uint64_t records = 0;
    uint64_t attributed_records = 0;
    uint64_t complete_records = 0;

    void emit(const LogRecord& record) override {
        ++records;
        if (record.process_id != 0 && record.process == "spawned process" &&
            !record.process_source_file.empty() &&
            record.process_source_line != 0) {
            ++attributed_records;
        }
        if (record.level == LogLevel::Info &&
            record.scope == "scoreboard" &&
            record.message == "transaction checkpoint" &&
            record.test_name == kernel_name() &&
            !record.source_file.empty() && record.source_line != 0 &&
            record.sequence == records) {
            ++complete_records;
        }
    }
};

Task<void> structured_logging_process(TestContext test, uint32_t iterations,
                                      uint64_t& disabled_factories) {
    auto log = test.logger("scoreboard");
    for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
        log.debug([&] {
            ++disabled_factories;
            return "transaction " + std::to_string(iteration);
        });
        if ((iteration & 1023u) == 0) {
            log.info("transaction checkpoint");
        }
    }
    co_return;
}

Task<void> run_structured_logging(Context context) {
    StructuredLogSink sink;
    context.result.test_name = "structured_logging";
    TestContext test{
        context.scheduler, context.result, {}, nullptr,
        LoggingOptions{.minimum_level = LogLevel::Info, .sink = &sink}};
    uint64_t disabled_factories = 0;
    ++context.result.spawned_processes;
    auto process = test.spawn(structured_logging_process(
        test, context.iterations, disabled_factories));
    co_await process;

    const uint64_t expected_records =
        (static_cast<uint64_t>(context.iterations) + 1023u) / 1024u;
    check64(context, "structured log records", sink.records,
            expected_records);
    check64(context, "structured log attribution", sink.attributed_records,
            expected_records);
    check64(context, "structured log metadata", sink.complete_records,
            expected_records);
    check64(context, "disabled log factories", disabled_factories, 0);
    co_await Delay{1_ps};
    report(context);
}

Task<void> run_structured_log_history(Context context) {
    StructuredLogSink sink;
    LogHistory history;
    const uint64_t expected_records =
        (static_cast<uint64_t>(context.iterations) + 1023u) / 1024u;
    history.reserve(expected_records);
    context.result.test_name = "structured_log_history";
    TestContext test{
        context.scheduler, context.result, {}, nullptr,
        LoggingOptions{.minimum_level = LogLevel::Info,
                       .sink = &sink,
                       .history = &history}};
    uint64_t disabled_factories = 0;
    ++context.result.spawned_processes;
    auto process = test.spawn(structured_logging_process(
        test, context.iterations, disabled_factories));
    co_await process;

    uint64_t ordered_records = 0;
    uint64_t complete_history_records = 0;
    uint64_t previous_time = 0;
    for (size_t index = 0; index < history.size(); ++index) {
        const auto& record = history[index];
        if (record.sequence == index + 1 &&
            (index == 0 || record.simulation_time_fs >= previous_time)) {
            ++ordered_records;
        }
        previous_time = record.simulation_time_fs;
        if (record.level == LogLevel::Info &&
            record.scope == "scoreboard" &&
            record.message == "transaction checkpoint" &&
            !record.source_file.empty() && record.source_line != 0 &&
            record.process_id != 0 && record.process == "spawned process" &&
            !record.process_source_file.empty() &&
            record.process_source_line != 0) {
            ++complete_history_records;
        }
    }

    check64(context, "structured log output", sink.records, expected_records);
    check64(context, "structured log output metadata", sink.complete_records,
            expected_records);
    check64(context, "structured log history", history.size(),
            expected_records);
    check64(context, "structured log history order", ordered_records,
            expected_records);
    check64(context, "structured log history metadata",
            complete_history_records, expected_records);
    check64(context, "disabled log factories", disabled_factories, 0);
    co_await Delay{1_ps};
    report(context);
}
#endif

Task<void> dynamic_spawn_child(TestContext test, uint32_t value,
                               uint32_t iteration) {
    test.expect_eq("dynamic process value", value, stimulus(iteration));
    co_return;
}

Task<void> dynamic_task_child(TestContext test, uint32_t value,
                              uint32_t iteration) {
    test.expect_eq("dynamic task value", value, stimulus(iteration));
    co_return;
}

Task<void> run_dynamic_task(Context context) {
    TestContext test{context.scheduler, context.result};
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint32_t value = stimulus(iteration);
        ++context.result.features.dynamic_spawn;
        co_await dynamic_task_child(test, value, iteration);
    }
    co_await Delay{1_ps};
    report(context);
}

Task<void> dynamic_scheduler_child(TestContext test, uint32_t value,
                                   uint32_t iteration) {
    test.expect_eq("scheduler process value", value, stimulus(iteration));
    co_return;
}

Task<void> run_dynamic_spawn_scheduler(Context context) {
    TestContext test{context.scheduler, context.result};
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint32_t value = stimulus(iteration);
        ++context.result.features.dynamic_spawn;
        ++context.result.spawned_processes;
        auto child = context.scheduler.spawn(
            dynamic_scheduler_child(test, value, iteration));
        co_await child;
    }
    co_await Delay{1_ps};
    report(context);
}

Task<void> run_dynamic_spawn(Context context) {
    TestContext test{context.scheduler, context.result};
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint32_t value = stimulus(iteration);
        ++context.result.features.dynamic_spawn;
        ++context.result.spawned_processes;
        auto child =
            test.spawn(dynamic_spawn_child(test, value, iteration));
        co_await child;
    }
    co_await Delay{1_ps};
    report(context);
}

Task<void> dynamic_suspending_child(TestContext test, Event& ready,
                                    Event& release, uint32_t value,
                                    uint32_t iteration) {
    ready.set();
    co_await release;
    test.expect_eq("suspending process value", value, stimulus(iteration));
}

Task<void> dynamic_suspending_release(Event& ready, Event& release) {
    co_await ready;
    release.set();
}

Task<void> run_dynamic_spawn_suspending(Context context) {
    TestContext test{context.scheduler, context.result};
    Event ready;
    Event release;
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint32_t value = stimulus(iteration);
        ++context.result.features.dynamic_spawn;
        context.result.spawned_processes += 2;
        auto child = test.spawn(dynamic_suspending_child(
            test, ready, release, value, iteration));
        auto releaser = test.spawn(dynamic_suspending_release(ready, release));
        co_await child;
        co_await releaser;
        ready.clear();
        release.clear();
    }
    co_await Delay{1_ps};
    report(context);
}

Task<void> response_monitor(Context context, Queue<uint32_t>& observed) {
    while (true) {
        co_await RisingEdge{context.dut.rsp_valid};
        co_await Delay{1_ps};
        co_await observed.put(context.dut.rsp_data.get());
        ++context.result.features.queue_put;
    }
}

Task<void> response_edge_watcher(Context context, uint64_t& response_edges) {
    while (true) {
        co_await RisingEdge{context.dut.rsp_valid};
        ++response_edges;
    }
}

Task<void> run_dynamic_monitor(Context context) {
    context.dut.rst_n.set(0);
    context.dut.req_valid.set(0);
    context.dut.req_data.set(0);
    context.dut.rsp_ready.set(1);
    for (uint32_t cycle = 0; cycle < 4; ++cycle) {
        co_await RisingEdge{context.dut.clk};
    }
    context.dut.rst_n.set(1);

    TestContext test{context.scheduler, context.result};
    Queue<uint32_t> observed{8};
    uint64_t response_edges = 0;
    context.result.spawned_processes += 2;
    auto monitor = test.spawn(response_monitor(context, observed));
    auto watcher = test.spawn(response_edge_watcher(context, response_edges));

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        co_await drive_request(context, stimulus(iteration));
        const uint32_t response = co_await observed.get();
        ++context.result.features.queue_get;
        check(context, "monitored response", response,
              expected_response(iteration));
        context.result.checksum =
            (context.result.checksum ^ response) * 0x0100'0193u;
        ++context.result.transactions;
    }

    monitor.cancel();
    watcher.cancel();
    co_await monitor;
    co_await watcher;
    co_await wait_for_response_count(context);
    check64(context, "observed response edges", response_edges,
            context.iterations);
    check(context, "request count", context.dut.request_count.get(),
          context.iterations);
    check(context, "response count", context.dut.response_count.get(),
          context.iterations);
    report(context);
}

Task<void> process_pipeline_driver(Context context,
                                   Queue<uint32_t>& expected_values) {
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        co_await expected_values.put(expected_response(iteration));
        ++context.result.features.queue_put;
        co_await drive_request(context, stimulus(iteration));
    }
}

Task<void> process_pipeline_worker(Context context,
                                   Queue<uint32_t>& observed_values) {
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        co_await RisingEdge{context.dut.rsp_valid};
        co_await Delay{1_ps};
        co_await observed_values.put(context.dut.rsp_data.get());
        ++context.result.features.queue_put;
    }
}

Task<void> process_pipeline_scoreboard(Context context,
                                       Queue<uint32_t>& expected_values,
                                       Queue<uint32_t>& observed_values) {
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint32_t expected = co_await expected_values.get();
        const uint32_t actual = co_await observed_values.get();
        context.result.features.queue_get += 2;
        check(context, "pipeline response", actual, expected);
        context.result.checksum =
            (context.result.checksum ^ actual) * 0x0100'0193u;
        ++context.result.transactions;
    }
}

Task<void> run_process_pipeline(Context context) {
    context.dut.rst_n.set(0);
    context.dut.req_valid.set(0);
    context.dut.req_data.set(0);
    context.dut.rsp_ready.set(1);
    for (uint32_t cycle = 0; cycle < 4; ++cycle) {
        co_await RisingEdge{context.dut.clk};
    }
    context.dut.rst_n.set(1);

    TestContext test{context.scheduler, context.result};
    Queue<uint32_t> expected_values{8};
    Queue<uint32_t> observed_values{8};
    context.result.spawned_processes += 3;

    auto driver = test.spawn(
        process_pipeline_driver(context, expected_values));
    auto worker = test.spawn(
        process_pipeline_worker(context, observed_values));
    auto scoreboard = test.spawn(
        process_pipeline_scoreboard(context, expected_values, observed_values));

    co_await driver;
    co_await worker;
    co_await scoreboard;
    check(context, "expected queue empty", expected_values.empty(), 1);
    check(context, "observed queue empty", observed_values.empty(), 1);
    co_await wait_for_response_count(context);
    check(context, "request count", context.dut.request_count.get(),
          context.iterations);
    check(context, "response count", context.dut.response_count.get(),
          context.iterations);
    report(context);
}

Task<void> analysis_response_monitor(Context context,
                                     AnalysisPort<uint32_t>& observed) {
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        co_await RisingEdge{context.dut.rsp_valid};
        co_await Delay{1_ps};
        observed.write(context.dut.rsp_data.get());
        ++context.result.features.analysis_write;
        context.result.features.analysis_delivery += 2;
        ++context.result.features.queue_put;
    }
}

Task<void> run_analysis_fanout(Context context) {
    context.dut.rst_n.set(0);
    context.dut.req_valid.set(0);
    context.dut.req_data.set(0);
    context.dut.rsp_ready.set(1);
    for (uint32_t cycle = 0; cycle < 4; ++cycle) {
        co_await RisingEdge{context.dut.clk};
    }
    context.dut.rst_n.set(1);

    TestContext test{context.scheduler, context.result};
    AnalysisPort<uint32_t> expected;
    AnalysisPort<uint32_t> observed;
    InOrderScoreboard<uint32_t> scoreboard{test, "analysis response"};
    AnalysisBuffer<uint32_t> buffer{8, AnalysisOverflowPolicy::DropNewest};
    auto expected_connection = expected.connect(scoreboard.expected());
    auto actual_connection = observed.connect(scoreboard.actual());
    auto buffer_connection = observed.connect(buffer);
    ++context.result.spawned_processes;
    auto monitor =
        test.spawn(analysis_response_monitor(context, observed));

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        expected.write(expected_response(iteration));
        ++context.result.features.analysis_write;
        ++context.result.features.analysis_delivery;
        co_await drive_request(context, stimulus(iteration));
        const uint32_t response = co_await buffer.get();
        ++context.result.features.queue_get;
        context.result.checksum =
            (context.result.checksum ^ response) * 0x0100'0193u;
        ++context.result.transactions;
    }

    co_await monitor;
    scoreboard.finalize();
    check64(context, "analysis buffer drops", buffer.dropped(), 0);
    check(context, "analysis buffer empty", buffer.empty(), 1);
    co_await wait_for_response_count(context);
    check(context, "request count", context.dut.request_count.get(),
          context.iterations);
    check(context, "response count", context.dut.response_count.get(),
          context.iterations);
    report(context);
}

Task<void> queue_sync_producer(Context context, Queue<uint32_t>& queue,
                               Semaphore& credits) {
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        co_await credits.acquire();
        ++context.result.features.semaphore_acquire;
        co_await queue.put(stimulus(iteration));
        ++context.result.features.queue_put;
    }
}

Task<void> queue_sync_consumer(Context context, Queue<uint32_t>& queue,
                               Semaphore& credits, Lock& lock) {
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint32_t payload = co_await queue.get();
        ++context.result.features.queue_get;
        co_await lock.acquire();
        ++context.result.features.lock_acquire;
        check(context, "bounded queue payload", payload, stimulus(iteration));
        lock.release();
        credits.release();
        co_await transact(context, iteration, payload);
    }
}

Task<void> release_initial_lock(Lock& lock) {
    lock.release();
    co_return;
}

Task<void> run_queue_sync(Context context) {
    context.dut.rst_n.set(0);
    context.dut.req_valid.set(0);
    context.dut.req_data.set(0);
    context.dut.rsp_ready.set(1);
    for (uint32_t cycle = 0; cycle < 4; ++cycle) {
        co_await RisingEdge{context.dut.clk};
    }
    context.dut.rst_n.set(1);

    Queue<uint32_t> queue{1};
    Semaphore credits{2};
    Lock lock;
    static_cast<void>(lock.try_acquire());
    co_await Join{queue_sync_producer(context, queue, credits),
                  queue_sync_consumer(context, queue, credits, lock),
                  release_initial_lock(lock)};

    co_await wait_for_response_count(context);
    check(context, "request count", context.dut.request_count.get(),
          context.iterations);
    check(context, "response count", context.dut.response_count.get(),
          context.iterations);
    report(context);
}

Task<void> run_force_direct(Context context) {
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint32_t forced = stimulus(iteration) ^ 0xa5a5'5a5au;
        ++context.result.features.force_release;
        context.dut.force_target.force(forced);
        check(context, "direct forced net readback",
              context.dut.force_target.get(), forced);
        context.dut.force_target.release();
    }
    report(context);
    co_return;
}

Task<void> run_timing_phases(Context context) {
    context.dut.rst_n.set(0);
    context.dut.req_valid.set(0);
    context.dut.rsp_ready.set(1);
    context.dut.array_i.at(1).set(0);
    for (uint32_t cycle = 0; cycle < 4; ++cycle) {
        co_await RisingEdge{context.dut.clk};
    }
    context.dut.rst_n.set(1);

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint32_t first = stimulus(iteration);
        const uint32_t second = first ^ 0xa5a5'5a5au;
        ++context.result.features.timing_phases;

#ifdef CPPTB_DEFERRED_WRITES
        // The deferred peer keeps the same await cadence and check count as
        // the immediate kernel and samples where the deferred contract
        // guarantees settlement. A write queued from inside the ReadWrite
        // phase re-arms that phase, so the scheduler drains the queue
        // within the timestep -- cocotb's writes-until-stable loop -- and
        // by ReadOnly both writes have applied in order.
        co_await FallingEdge{context.dut.clk};
        context.dut.array_i.at(1).set(first);
        co_await ReadWrite{};

        context.dut.array_i.at(1).set(second);
        co_await ReadOnly{};
        check(context, "the queue drained and settled by ReadOnly",
              context.dut.array_o.at(1).get(), second ^ 0x6d2b'79f6u);
        check(context, "the drained write reads back from the simulator",
              context.dut.array_i.at(1).get(), second);

        co_await NextTimeStep{};
#else
        co_await FallingEdge{context.dut.clk};
        context.dut.array_i.at(1).set(first);
        co_await ReadWrite{};
        check(context, "ReadWrite settled value",
              context.dut.array_o.at(1).get(), first ^ 0x6d2b'79f6u);

        context.dut.array_i.at(1).set(second);
        co_await ReadOnly{};
        check(context, "ReadOnly settled value",
              context.dut.array_o.at(1).get(), second ^ 0x6d2b'79f6u);

        co_await NextTimeStep{};
#endif
    }

    report(context);
}

template <MemoryMappedMaster Master>
Task<void> run_apb_sequence(
    Context context, Master& master,
    AnalysisPort<typename Master::transaction_type>& expected, Event& done,
    bool count_component = true) {
    using Transaction = typename Master::transaction_type;
    using ByteEnable = typename Master::byte_enable_type;
    const auto all_bytes = Master::all_bytes();

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const auto address = static_cast<typename Master::address_type>(
            (iteration & 15u) * 4u);
        const auto value =
            static_cast<typename Master::data_type>(stimulus(iteration));

        const auto write = co_await master.write(address, value, all_bytes);
        check(context, "APB write status",
              static_cast<uint32_t>(write.status),
              static_cast<uint32_t>(MemoryStatus::Okay));
        expected.write(Transaction{MemoryOperation::Write, address, value,
                                   all_bytes, MemoryStatus::Okay, 0});
        ++context.result.transactions;
        if (count_component) ++context.result.features.apb_component;

        const auto read = co_await master.read(address);
        check(context, "APB read data", read.data, value);
        check(context, "APB read status", static_cast<uint32_t>(read.status),
              static_cast<uint32_t>(MemoryStatus::Okay));
        expected.write(Transaction{MemoryOperation::Read, address, value,
                                   all_bytes, MemoryStatus::Okay, 0});
        ++context.result.transactions;
        if (count_component) ++context.result.features.apb_component;
        context.result.checksum =
            (context.result.checksum ^ read.data) * 0x0100'0193u;
    }
    done.set();
}

Task<void> run_apb_component(Context context) {
    context.dut.rst_n.set(0);
    context.dut.apb_psel_i.set(0);
    context.dut.apb_penable_i.set(0);
    context.dut.apb_pwrite_i.set(0);
    context.dut.apb_paddr_i.set(0);
    context.dut.apb_pwdata_i.set(0);
    context.dut.apb_pstrb_i.set(0);
    for (uint32_t cycle = 0; cycle < 4; ++cycle) {
        co_await RisingEdge{context.dut.clk};
    }
    context.dut.rst_n.set(1);

    const auto bus = ApbBus{
        context.dut.clk,          context.dut.apb_psel_i,
        context.dut.apb_penable_i, context.dut.apb_pwrite_i,
        context.dut.apb_paddr_i,   context.dut.apb_pwdata_i,
        context.dut.apb_prdata_o,  context.dut.apb_pready_o,
        context.dut.apb_pslverr_o, context.dut.apb_pstrb_i};
    ApbMaster master{bus};
    using Transaction = typename decltype(master)::transaction_type;
    TestContext test{context.scheduler, context.result};
    AnalysisPort<Transaction> expected;
    InOrderScoreboard<Transaction> scoreboard{test, "APB transaction"};
    ApbMonitor monitor{test, bus};
    ApbProtocolChecker checker{test, bus};
    Event done;
    auto expected_connection = expected.connect(scoreboard.expected());
    auto actual_connection = monitor.observed().connect(scoreboard.actual());

    co_await Join{
        run_apb_sequence(context, master, expected, done),
        monitor.run(static_cast<std::size_t>(context.iterations) * 2u),
        checker.run_until([&done] { return done.is_set(); })};

    scoreboard.finalize();
    check64(context, "APB scoreboard comparisons", scoreboard.compared(),
            static_cast<uint64_t>(context.iterations) * 2u);
    check64(context, "APB protocol violations", checker.violations(), 0);
    report(context);
}

template <typename Subscriber>
class MeasuredAnalysisSubscriber {
   public:
    MeasuredAnalysisSubscriber(Subscriber& subscriber, uint64_t* publications,
                               uint64_t& deliveries)
        : subscriber_(std::addressof(subscriber)),
          publications_(publications),
          deliveries_(std::addressof(deliveries)) {}

    template <typename Value>
    void write(const Value& value) {
        if (publications_) ++*publications_;
        ++*deliveries_;
        subscriber_->write(value);
    }

   private:
    Subscriber* subscriber_;
    uint64_t* publications_;
    uint64_t* deliveries_;
};

Task<void> run_transaction_recording(Context context) {
    context.dut.rst_n.set(0);
    context.dut.apb_psel_i.set(0);
    context.dut.apb_penable_i.set(0);
    context.dut.apb_pwrite_i.set(0);
    context.dut.apb_paddr_i.set(0);
    context.dut.apb_pwdata_i.set(0);
    context.dut.apb_pstrb_i.set(0);
    for (uint32_t cycle = 0; cycle < 4; ++cycle) {
        co_await RisingEdge{context.dut.clk};
    }
    context.dut.rst_n.set(1);

    const auto bus = ApbBus{
        context.dut.clk,           context.dut.apb_psel_i,
        context.dut.apb_penable_i, context.dut.apb_pwrite_i,
        context.dut.apb_paddr_i,   context.dut.apb_pwdata_i,
        context.dut.apb_prdata_o,  context.dut.apb_pready_o,
        context.dut.apb_pslverr_o, context.dut.apb_pstrb_i};
    ApbMaster master{bus};
    using Transaction = typename decltype(master)::transaction_type;
    TestContext test{context.scheduler, context.result};
    AnalysisPort<Transaction> expected;
    InOrderScoreboard<Transaction> scoreboard{test, "recorded APB transaction"};
    ApbMonitor monitor{test, bus};
    ApbProtocolChecker checker{test, bus};
    TransactionRecorder recorder;
    InMemoryTransactionSink trace;
    auto& stream = recorder.stream<Transaction>("apb0.observed");
    Event done;
    auto expected_connection = expected.connect(scoreboard.expected());
    auto& writes = context.result.features.analysis_write;
    auto& deliveries = context.result.features.analysis_delivery;
    MeasuredAnalysisSubscriber measured_scoreboard{
        scoreboard.actual(), nullptr, deliveries};
    MeasuredAnalysisSubscriber measured_stream{stream, &writes, deliveries};
    MeasuredAnalysisSubscriber measured_trace{trace, &writes, deliveries};
    auto sink_connection = recorder.connect(measured_trace);
    auto actual_connection = monitor.observed().connect(measured_scoreboard);
    auto recording_connection = monitor.observed().connect(measured_stream);

    co_await Join{
        run_apb_sequence(context, master, expected, done, false),
        monitor.run(static_cast<std::size_t>(context.iterations) * 2u),
        checker.run_until([&done] { return done.is_set(); })};

    scoreboard.finalize();
    const auto& records = trace.records();
    const auto* first = records.empty() ? nullptr : &records.front();
    const auto* last = records.empty() ? nullptr : &records.back();
    check64(context, "recorded APB scoreboard comparisons",
            scoreboard.compared(),
            static_cast<uint64_t>(context.iterations) * 2u);
    check64(context, "recorded APB protocol violations", checker.violations(),
            0);
    check64(context, "recorded APB transaction count", records.size(),
            static_cast<uint64_t>(context.iterations) * 2u);
    check64(context, "recorded APB first sequence",
            first ? first->sequence : std::numeric_limits<uint64_t>::max(), 0);
    check64(context, "recorded APB last sequence",
            last ? last->sequence : std::numeric_limits<uint64_t>::max(),
            static_cast<uint64_t>(context.iterations) * 2u - 1u);
    check(context, "recorded APB interval",
          first && first->end_time > first->begin_time,
          true);
    check(context, "recorded APB typed payload",
          first && first->value_json.find("\"operation\":\"write\"") !=
                       std::string::npos,
          true);
    report(context);
}

template <MemoryMappedMaster Master>
Task<void> run_memory_model_sequence(Context context, Master& master,
                                     Event& done) {
    const auto all_bytes = Master::all_bytes();
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const auto address = static_cast<typename Master::address_type>(
            (iteration & 15u) * 4u);
        const auto value =
            static_cast<typename Master::data_type>(stimulus(iteration));

        const auto write = co_await master.write(address, value, all_bytes);
        check(context, "memory-model APB write status",
              static_cast<uint32_t>(write.status),
              static_cast<uint32_t>(MemoryStatus::Okay));
        ++context.result.transactions;
        ++context.result.features.memory_model;

        const auto read = co_await master.read(address);
        check(context, "memory-model APB read data", read.data, value);
        check(context, "memory-model APB read status",
              static_cast<uint32_t>(read.status),
              static_cast<uint32_t>(MemoryStatus::Okay));
        ++context.result.transactions;
        ++context.result.features.memory_model;
        context.result.checksum =
            (context.result.checksum ^ read.data) * 0x0100'0193u;
    }
    done.set();
}

Task<void> run_memory_model(Context context) {
    context.dut.rst_n.set(0);
    context.dut.apb_psel_i.set(0);
    context.dut.apb_penable_i.set(0);
    context.dut.apb_pwrite_i.set(0);
    context.dut.apb_paddr_i.set(0);
    context.dut.apb_pwdata_i.set(0);
    context.dut.apb_pstrb_i.set(0);
    for (uint32_t cycle = 0; cycle < 4; ++cycle) {
        co_await RisingEdge{context.dut.clk};
    }
    context.dut.rst_n.set(1);

    const auto bus = ApbBus{
        context.dut.clk,           context.dut.apb_psel_i,
        context.dut.apb_penable_i, context.dut.apb_pwrite_i,
        context.dut.apb_paddr_i,   context.dut.apb_pwdata_i,
        context.dut.apb_prdata_o,  context.dut.apb_pready_o,
        context.dut.apb_pslverr_o, context.dut.apb_pstrb_i};
    ApbMaster master{bus};
    using Transaction = typename decltype(master)::transaction_type;
    TestContext test{context.scheduler, context.result};
    ApbMonitor monitor{test, bus};
    ApbProtocolChecker checker{test, bus};
    SparseMemory memory;
    memory.add_region(MemoryRegionConfig{
        .name = "register-bank", .base = 0, .size = 64});
    auto predictor = make_memory_predictor<Transaction>(
        test, memory, "memory-model APB transaction");
    Event done;
    auto prediction_connection = monitor.observed().connect(predictor);

    co_await Join{
        run_memory_model_sequence(context, master, done),
        monitor.run(static_cast<std::size_t>(context.iterations) * 2u),
        checker.run_until([&done] { return done.is_set(); })};

    check64(context, "memory-model reads", predictor.reads(),
            context.iterations);
    check64(context, "memory-model writes", predictor.writes(),
            context.iterations);
    check64(context, "memory-model mismatches", predictor.mismatches(), 0);
    check64(context, "memory-model APB protocol violations",
            checker.violations(), 0);
    report(context);
}

Task<void> run_memory_model_direct(Context context) {
    SparseMemory memory;
    memory.add_region(MemoryRegionConfig{
        .name = "register-bank", .base = 0, .size = 64});
    uint64_t reads = 0;
    uint64_t writes = 0;
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint32_t address = (iteration & 15u) * 4u;
        const uint32_t value = stimulus(iteration);
        const auto write = memory.write_word(address, value, uint8_t{0xf});
        check(context, "direct memory-model write status",
              static_cast<uint32_t>(write.status),
              static_cast<uint32_t>(MemoryStatus::Okay));
        ++writes;
        ++context.result.transactions;
        ++context.result.features.memory_model_direct;

        const auto read = memory.read_word<uint32_t>(address);
        check(context, "direct memory-model read data", read.data, value);
        check(context, "direct memory-model read status",
              static_cast<uint32_t>(read.status),
              static_cast<uint32_t>(MemoryStatus::Okay));
        ++reads;
        ++context.result.transactions;
        ++context.result.features.memory_model_direct;
        context.result.checksum =
            (context.result.checksum ^ read.data) * 0x0100'0193u;
    }
    check64(context, "direct memory-model reads", reads, context.iterations);
    check64(context, "direct memory-model writes", writes, context.iterations);
    report(context);
    co_return;
}

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_PREDICTION_VALIDITY
struct RegisterPredictionMaster {
    using address_type = uint32_t;
    using data_type = uint32_t;
    using byte_enable_type = uint8_t;
    using write_request_type =
        MemoryWriteRequest<address_type, data_type, byte_enable_type>;
    using read_request_type = MemoryReadRequest<address_type>;
    using write_response_type = MemoryWriteResponse;
    using read_response_type = MemoryReadResponse<data_type>;

    Task<write_response_type> write(write_request_type) {
        co_return write_response_type{};
    }

    Task<read_response_type> read(read_request_type) {
        co_return read_response_type{};
    }
};

constexpr std::array kRegisterPredictionFields{
    RegisterFieldDescriptor{
        .name = "control",
        .path = "benchmark.control",
        .lsb = 0,
        .width = 8,
        .access = RegisterAccess::ReadWrite,
        .reset_value = 0x5a,
        .reset_mask = 0xff,
    },
    RegisterFieldDescriptor{
        .name = "pending",
        .path = "benchmark.pending",
        .lsb = 8,
        .width = 8,
        .access = RegisterAccess::ReadWrite,
        .write_effect = RegisterWriteEffect::WriteOneClear,
    },
    RegisterFieldDescriptor{
        .name = "sampled",
        .path = "benchmark.sampled",
        .lsb = 16,
        .width = 8,
        .access = RegisterAccess::ReadOnly,
        .read_effect = RegisterReadEffect::Clear,
    },
    RegisterFieldDescriptor{
        .name = "opaque",
        .path = "benchmark.opaque",
        .lsb = 24,
        .width = 8,
        .access = RegisterAccess::ReadOnly,
        .read_effect = RegisterReadEffect::User,
    },
};

constexpr RegisterDescriptor kRegisterPredictionDescriptor{
    .name = "prediction",
    .path = "benchmark.prediction",
    .width = 32,
    .access_width = 32,
    .reset_value = 0x5a,
    .reset_mask = 0xff,
    .fields = kRegisterPredictionFields,
};

Task<void> run_register_prediction_validity(Context context) {
    using Transaction = MemoryTransaction<uint32_t, uint32_t, uint8_t>;
    TestContext test{context.scheduler, context.result};
    RegisterPredictionMaster master;
    RegisterHandle model{test, master, kRegisterPredictionDescriptor};
    auto pending = model.field(kRegisterPredictionFields[1]);
    std::array<RegisterHandle<RegisterPredictionMaster>*, 1> handles{&model};
    RegisterPredictor predictor{
        test,
        std::span<RegisterHandle<RegisterPredictionMaster>* const>{handles}};
    AnalysisPort<Transaction> observed;
    auto prediction_connection = observed.connect(predictor);

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint32_t value = stimulus(iteration);
        model.reset();
        check(context, "register reset validity", model.mirrored_valid_mask(),
              0xff);

        pending.set_desired(value >> 8u);
        check(context, "register field desired validity",
              model.desired_valid_mask(), 0xffff);

        model.predict(value, RegisterPrediction::Direct);
        check(context, "register direct prediction validity",
              model.mirrored_valid_mask(), 0xffff'ffffu);

        observed.write(Transaction{
            .operation = MemoryOperation::Write,
            .address = 0,
            .data = value ^ 0x5a5a'a5a5u,
            .byte_enable = 0xf,
        });
        observed.write(Transaction{
            .operation = MemoryOperation::Read,
            .address = 0,
            .data = value,
            .byte_enable = 0xf,
        });
        check(context, "register read-effect validity",
              model.mirrored_valid_mask(), 0x00ff'ffffu);

        model.reset();
        observed.write(Transaction{
            .operation = MemoryOperation::Write,
            .address = 0,
            .data = value,
            .byte_enable = 0x2,
        });
        check(context, "register partial write validity",
              model.mirrored_valid_mask(), 0xffu | (value & 0xff00u));
        ++context.result.features.register_prediction_validity;
    }

    check64(context, "register validity operations",
            context.result.features.register_prediction_validity,
            context.iterations);
    check64(context, "register validity transactions",
            context.result.transactions, 0);
    check64(context, "register predictor reads", predictor.reads(),
            context.iterations);
    check64(context, "register predictor writes", predictor.writes(),
            static_cast<uint64_t>(context.iterations) * 2u);
    report(context);
    co_return;
}
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_BACKDOOR
struct RegisterBackdoorMaster {
    using address_type = uint32_t;
    using data_type = uint32_t;
    using byte_enable_type = uint8_t;
    using write_request_type =
        MemoryWriteRequest<address_type, data_type, byte_enable_type>;
    using read_request_type = MemoryReadRequest<address_type>;
    using write_response_type = MemoryWriteResponse;
    using read_response_type = MemoryReadResponse<data_type>;

    Task<write_response_type> write(write_request_type) {
        co_return write_response_type{};
    }
    Task<read_response_type> read(read_request_type) {
        co_return read_response_type{};
    }
};

constexpr std::array kRegisterBackdoorSlices{
    RegisterBackdoorSliceDescriptor{
        .path = "pending_data", .register_lsb = 0, .width = 32},
};

constexpr RegisterDescriptor kRegisterBackdoorDescriptor{
    .name = "pending_data",
    .path = "benchmark.pending_data",
    .width = 32,
    .access_width = 32,
    .backdoor_slices = kRegisterBackdoorSlices,
};

class BenchmarkRegisterBackdoor final : public RegisterBackdoor<uint64_t> {
   public:
    explicit BenchmarkRegisterBackdoor(AuthoringCoreDut dut) : dut_(dut) {}

    uint64_t peek(const RegisterDescriptor&, uint64_t) override {
        const auto signal =
            dut_.template cpptb_signal<"pending_data">();
        return static_cast<uint32_t>(
            register_detail::read_hdl_full<32>(signal));
    }

    void poke(const RegisterDescriptor&, uint64_t, uint64_t value) override {
        const auto signal =
            dut_.template cpptb_signal<"pending_data">();
        register_detail::write_hdl_full<32>(signal, value);
    }

   private:
    AuthoringCoreDut dut_;
};

Task<void> run_register_backdoor(Context context) {
    TestContext test{context.scheduler, context.result};
    RegisterBackdoorMaster master;
    BenchmarkRegisterBackdoor backdoor{context.dut};
    RegisterHandle model{
        test, master, kRegisterBackdoorDescriptor, 0, &backdoor};

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint32_t value = stimulus(iteration) ^ 0x6b51'27d9u;
        model.poke(value);
        check(context, "generated register backdoor", model.peek(), value);
        ++context.result.features.register_backdoor;
    }
    check64(context, "generated register backdoor operations",
            context.result.features.register_backdoor, context.iterations);
    check64(context, "generated register backdoor transactions",
            context.result.transactions, 0);
    report(context);
    co_return;
}
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_HIERARCHY
struct RegisterHierarchyMaster {
    using address_type = uint32_t;
    using data_type = uint32_t;
    using byte_enable_type = uint8_t;
    using write_request_type =
        MemoryWriteRequest<address_type, data_type, byte_enable_type>;
    using read_request_type = MemoryReadRequest<address_type>;
    using write_response_type = MemoryWriteResponse;
    using read_response_type = MemoryReadResponse<data_type>;

    Task<write_response_type> write(write_request_type) {
        co_return write_response_type{};
    }
    Task<read_response_type> read(read_request_type) {
        co_return read_response_type{};
    }
};

constexpr std::array<RegisterDescriptor, 4> kRegisterHierarchyDescriptors{{
    {.name = "lane0", .path = "benchmark.lane[0]", .address = 0,
     .width = 32, .access_width = 32},
    {.name = "lane1", .path = "benchmark.lane[1]", .address = 4,
     .width = 32, .access_width = 32},
    {.name = "lane2", .path = "benchmark.lane[2]", .address = 8,
     .width = 32, .access_width = 32},
    {.name = "lane3", .path = "benchmark.lane[3]", .address = 12,
     .width = 32, .access_width = 32},
}};

Task<void> run_register_hierarchy(Context context) {
    TestContext test{context.scheduler, context.result};
    RegisterHierarchyMaster master;
    RegisterHandle lane0{test, master, kRegisterHierarchyDescriptors[0]};
    RegisterHandle lane1{test, master, kRegisterHierarchyDescriptors[1]};
    RegisterHandle lane2{test, master, kRegisterHierarchyDescriptors[2]};
    RegisterHandle lane3{test, master, kRegisterHierarchyDescriptors[3]};
    RegisterViewArray lanes{lane0, lane1, lane2, lane3};

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint32_t value = stimulus(iteration);
        lanes.at<0>().predict(value + 0u);
        lanes.at<1>().predict(value + 1u);
        lanes.at<2>().predict(value + 2u);
        lanes.at<3>().predict(value + 3u);
        check(context, "register hierarchy lane 0", lanes.at<0>().mirrored(),
              value + 0u);
        check(context, "register hierarchy lane 1", lanes.at<1>().mirrored(),
              value + 1u);
        check(context, "register hierarchy lane 2", lanes.at<2>().mirrored(),
              value + 2u);
        check(context, "register hierarchy lane 3", lanes.at<3>().mirrored(),
              value + 3u);
        uint64_t traversal_sum = 0;
        lanes.for_each(
            [&](auto& lane) { traversal_sum += lane.mirrored(); });
        check64(context, "register hierarchy traversal", traversal_sum,
                static_cast<uint64_t>(value) * 4u + 6u);
        ++context.result.features.register_hierarchy;
    }
    check64(context, "register hierarchy operations",
            context.result.features.register_hierarchy, context.iterations);
    check64(context, "register hierarchy transactions",
            context.result.transactions, 0);
    report(context);
    co_return;
}
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_SPLIT
struct RegisterSplitMaster {
    using address_type = uint32_t;
    using data_type = uint32_t;
    using byte_enable_type = uint8_t;
    using write_request_type =
        MemoryWriteRequest<address_type, data_type, byte_enable_type>;
    using read_request_type = MemoryReadRequest<address_type>;
    using write_response_type = MemoryWriteResponse;
    using read_response_type = MemoryReadResponse<data_type>;

    Task<write_response_type> write(write_request_type request) {
        const uint32_t index = request.address / 2u;
        words[index] = static_cast<uint16_t>(request.data);
        ++result.transactions;
        co_return write_response_type{};
    }

    Task<read_response_type> read(read_request_type request) {
        const uint32_t index = request.address / 2u;
        ++result.transactions;
        co_return read_response_type{.data = words[index]};
    }

    BenchResult& result;
    std::array<uint16_t, 2> words{};
};

constexpr RegisterDescriptor kRegisterSplitDescriptor{
    .name = "split",
    .path = "benchmark.split",
    .width = 32,
    .access_width = 16,
};

Task<void> run_register_split(Context context) {
    TestContext test{context.scheduler, context.result};
    RegisterSplitMaster master{context.result};
    RegisterHandle model{test, master, kRegisterSplitDescriptor};

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint32_t value = stimulus(iteration);
        const auto write = co_await model.write(value);
        const auto read = co_await model.read();
        check64(context, "split register read", read.data, value);
        check64(context, "split register mirror", model.mirrored(), value);
        if (!write.okay() || !read.okay()) {
            throw std::runtime_error("split register transport failed");
        }
        ++context.result.features.register_split;
    }
    check64(context, "split register operations",
            context.result.features.register_split, context.iterations);
    check64(context, "split register transactions", context.result.transactions,
            static_cast<uint64_t>(context.iterations) * 4u);
    report(context);
    co_return;
}
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_WIDE
struct RegisterWideMaster {
    using address_type = uint32_t;
    using data_type = uint32_t;
    using byte_enable_type = uint8_t;
    using write_request_type =
        MemoryWriteRequest<address_type, data_type, byte_enable_type>;
    using read_request_type = MemoryReadRequest<address_type>;
    using write_response_type = MemoryWriteResponse;
    using read_response_type = MemoryReadResponse<data_type>;

    Task<write_response_type> write(write_request_type request) {
        words[request.address / 4u] = request.data;
        ++result.transactions;
        co_return write_response_type{};
    }
    Task<read_response_type> read(read_request_type request) {
        ++result.transactions;
        co_return read_response_type{.data = words[request.address / 4u]};
    }

    BenchResult& result;
    std::array<uint32_t, 8> words{};
};

struct RegisterWideBackdoor final : WideRegisterBackdoor {
    void peek_words(const RegisterDescriptor&, uint64_t,
                    std::span<uint32_t> destination) override {
        std::copy(storage.words().begin(), storage.words().end(),
                  destination.begin());
    }
    void poke_words(const RegisterDescriptor&, uint64_t,
                    std::span<const uint32_t> source) override {
        Bits<128>::word_array words{};
        std::copy(source.begin(), source.end(), words.begin());
        storage = Bits<128>::from_words(words);
    }

    Bits<128> storage;
};

struct RegisterWideMemoryBackdoor final : WideRegisterMemoryBackdoor {
    void peek_words(const RegisterMemoryDescriptor&, uint64_t, uint64_t,
                    std::span<uint32_t> destination) override {
        std::copy(storage.words().begin(), storage.words().end(),
                  destination.begin());
    }
    void poke_words(const RegisterMemoryDescriptor&, uint64_t, uint64_t,
                    std::span<const uint32_t> source) override {
        Bits<128>::word_array words{};
        std::copy(source.begin(), source.end(), words.begin());
        storage = Bits<128>::from_words(words);
    }

    Bits<128> storage;
};

constexpr RegisterDescriptor kRegisterWideDescriptor{
    .name = "wide",
    .path = "benchmark.wide",
    .width = 128,
    .access_width = 32,
};

constexpr RegisterMemoryDescriptor kRegisterWideMemoryDescriptor{
    .name = "wide_memory",
    .path = "benchmark.wide_memory",
    .address = 0x10,
    .entries = 1,
    .width = 128,
    .access_width = 32,
    .access = RegisterAccess::ReadWrite,
};

Task<void> run_register_wide(Context context) {
    TestContext test{context.scheduler, context.result};
    RegisterWideMaster master{context.result};
    RegisterWideBackdoor backdoor;
    RegisterWideMemoryBackdoor memory_backdoor;
    WideRegisterHandle<128, RegisterWideMaster> model{
        test, master, kRegisterWideDescriptor, &backdoor};
    WideRegisterMemoryHandle<128, RegisterWideMaster> memory{
        master, kRegisterWideMemoryDescriptor, 0, &memory_backdoor};
    std::array<WideRegisterHandle<128, RegisterWideMaster>*, 1> handles{
        &model};
    WideRegisterPredictor predictor{
        test,
        std::span<WideRegisterHandle<128, RegisterWideMaster>* const>{
            handles}};

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        Bits<128> value;
        for (uint32_t word = 0; word < 4; ++word) {
            value.set_word(word, stimulus(iteration * 4u + word));
        }
        const auto write = co_await model.write(value);
        const auto read = co_await model.read();
        check128(context, "wide register read", read.data, value);
        check128(context, "wide register mirror", model.mirrored(), value);
        model.poke(value);
        check128(context, "wide register backdoor", model.peek(), value);

        const auto memory_write = co_await memory.write(0, value);
        const auto memory_read = co_await memory.read(0);
        check128(context, "wide memory read", memory_read.data, value);
        memory.poke(0, value);
        check128(context, "wide memory backdoor", memory.peek(0), value);

        model.reset();
        for (uint32_t word = 0; word < 4; ++word) {
            predictor.write(MemoryTransaction<uint32_t, uint32_t, uint8_t>{
                .operation = MemoryOperation::Write,
                .address = word * 4u,
                .data = value.word(word),
                .byte_enable = 0xf,
            });
            ++context.result.transactions;
        }
        check128(context, "wide passive prediction", model.mirrored(), value);
        if (!write.okay() || !read.okay() || !memory_write.okay() ||
            !memory_read.okay()) {
            throw std::runtime_error("wide register transport failed");
        }
        ++context.result.features.register_wide;
    }
    check64(context, "wide register operations",
            context.result.features.register_wide, context.iterations);
    check64(context, "wide register transactions", context.result.transactions,
            static_cast<uint64_t>(context.iterations) * 20u);
    report(context);
    co_return;
}
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_ENUM
enum class BenchmarkMode : uint64_t {
    Idle = 0,
    Active = 3,
    Diagnostic = 7,
};

struct RegisterEnumMaster {
    using address_type = uint32_t;
    using data_type = uint32_t;
    using byte_enable_type = uint8_t;
    using write_request_type =
        MemoryWriteRequest<address_type, data_type, byte_enable_type>;
    using read_request_type = MemoryReadRequest<address_type>;
    using write_response_type = MemoryWriteResponse;
    using read_response_type = MemoryReadResponse<data_type>;

    Task<write_response_type> write(write_request_type request) {
        storage = static_cast<uint8_t>(request.data);
        ++result.transactions;
        co_return write_response_type{};
    }
    Task<read_response_type> read(read_request_type) {
        ++result.transactions;
        co_return read_response_type{.data = storage};
    }

    BenchResult& result;
    uint8_t storage = 0;
};

constexpr std::array kRegisterEnumFields{
    RegisterFieldDescriptor{
        .name = "mode",
        .path = "benchmark.enum.mode",
        .lsb = 0,
        .width = 3,
        .access = RegisterAccess::ReadWrite,
    },
};
constexpr RegisterDescriptor kRegisterEnumDescriptor{
    .name = "enum",
    .path = "benchmark.enum",
    .width = 8,
    .access_width = 8,
    .fields = kRegisterEnumFields,
};

Task<void> run_register_enum(Context context) {
    TestContext test{context.scheduler, context.result};
    RegisterEnumMaster master{context.result};
    RegisterHandle model{test, master, kRegisterEnumDescriptor};
    RegisterEnumFieldHandle<BenchmarkMode,
                            RegisterFieldHandle<RegisterEnumMaster>>
        mode{model, kRegisterEnumFields[0]};
    constexpr std::array values{
        BenchmarkMode::Idle, BenchmarkMode::Active, BenchmarkMode::Diagnostic};

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const auto value = values[(iteration + 1u) % values.size()];
        const auto write = co_await mode.write(value);
        const auto read = co_await mode.read();
        check64(context, "enum register read", static_cast<uint64_t>(read.data),
                static_cast<uint64_t>(value));
        check64(context, "enum register mirror",
                static_cast<uint64_t>(mode.mirrored()),
                static_cast<uint64_t>(value));
        if (!write.okay() || !read.okay()) {
            throw std::runtime_error("enum register transport failed");
        }
        ++context.result.features.register_enum;
    }
    check64(context, "enum register operations",
            context.result.features.register_enum, context.iterations);
    check64(context, "enum register transactions", context.result.transactions,
            static_cast<uint64_t>(context.iterations) * 2u);
    report(context);
    co_return;
}
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_SEQUENCES
struct RegisterSequenceMaster {
    using address_type = uint32_t;
    using data_type = uint32_t;
    using byte_enable_type = uint8_t;
    using write_request_type =
        MemoryWriteRequest<address_type, data_type, byte_enable_type>;
    using read_request_type = MemoryReadRequest<address_type>;
    using write_response_type = MemoryWriteResponse;
    using read_response_type = MemoryReadResponse<data_type>;

    Task<write_response_type> write(write_request_type request) {
        storage = request.data;
        ++result->transactions;
        co_return write_response_type{};
    }

    Task<read_response_type> read(read_request_type) {
        ++result->transactions;
        co_return read_response_type{.data = storage};
    }

    BenchResult* result = nullptr;
    uint32_t storage = 0;
};

constexpr std::array kRegisterSequenceFields{
    RegisterFieldDescriptor{
        .name = "value",
        .path = "benchmark.sequence.value",
        .width = 8,
        .access = RegisterAccess::ReadWrite,
        .reset_value = 0x5a,
        .reset_mask = 0xff,
    },
};

constexpr RegisterDescriptor kRegisterSequenceDescriptor{
    .name = "sequence",
    .path = "benchmark.sequence",
    .width = 32,
    .access_width = 32,
    .reset_value = 0x5a,
    .reset_mask = 0xff,
    .fields = kRegisterSequenceFields,
};

class RegisterSequenceBackdoor final : public RegisterBackdoor<uint64_t> {
   public:
    explicit RegisterSequenceBackdoor(RegisterSequenceMaster& master)
        : master_(&master) {}

    uint64_t peek(const RegisterDescriptor&, uint64_t) override {
        return master_->storage;
    }

    void poke(const RegisterDescriptor&, uint64_t, uint64_t value) override {
        master_->storage = static_cast<uint32_t>(value);
    }

   private:
    RegisterSequenceMaster* master_;
};

class RegisterSequenceModel {
   public:
    RegisterSequenceModel(TestContext test, RegisterSequenceMaster& master,
                          RegisterSequenceBackdoor& backdoor)
        : sequence(std::move(test), master, kRegisterSequenceDescriptor,
                   &backdoor) {}

    template <typename Function>
    Task<void> for_each_register_async(Function& function) {
        co_await function(sequence);
    }

    RegisterHandle<RegisterSequenceMaster> sequence;
};

Task<void> run_register_sequences(Context context) {
    TestContext test{context.scheduler, context.result};
    RegisterSequenceMaster master{.result = &context.result};
    RegisterSequenceBackdoor backdoor{master};
    RegisterSequenceModel model{test, master, backdoor};
    uint64_t operations = 0;

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        master.storage = 0x5a;
        model.sequence.reset();

        const auto reset_frontdoor =
            co_await register_reset_check(test, model);
        const auto reset_backdoor = co_await register_reset_check(
            test, model, {.path = AccessPath::Backdoor});
        const auto access = co_await register_access_check(test, model);
        const auto bash_frontdoor =
            co_await register_bit_bash(test, model);
        const auto bash_backdoor = co_await register_bit_bash(
            test, model, {.path = AccessPath::Backdoor});

        check64(context, "reset frontdoor register count",
                reset_frontdoor.registers_tested, 1);
        check64(context, "reset backdoor read count",
                reset_backdoor.backdoor_reads, 1);
        check64(context, "access register count", access.registers_tested, 1);
        check64(context, "frontdoor bit-bash count",
                bash_frontdoor.bits_tested, 8);
        check64(context, "backdoor bit-bash count",
                bash_backdoor.bits_tested, 8);

        context.result.checksum =
            (context.result.checksum ^
             ((bash_frontdoor.bits_tested << 16u) ^ master.storage ^
              iteration)) *
            0x0100'0193u;
        ++operations;
    }

    check64(context, "register sequence operations", operations,
            context.iterations);
    check64(context, "register sequence transactions",
            context.result.transactions,
            static_cast<uint64_t>(context.iterations) * 21u);
    report(context);
    co_return;
}
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_COVERAGE
constexpr std::array kRegisterCoverageFields{
    RegisterFieldDescriptor{
        .name = "command",
        .path = "benchmark.coverage.control.command",
        .lsb = 0,
        .width = 8,
        .access = RegisterAccess::ReadWrite,
    },
    RegisterFieldDescriptor{
        .name = "status",
        .path = "benchmark.coverage.control.status",
        .lsb = 8,
        .width = 8,
        .access = RegisterAccess::ReadOnly,
    },
};

constexpr std::array kRegisterCoverageRegisters{
    RegisterDescriptor{
        .name = "control",
        .path = "benchmark.coverage.control",
        .width = 16,
        .access_width = 16,
        .fields = kRegisterCoverageFields,
    },
};

constexpr std::array kRegisterCoverageMemories{
    RegisterMemoryDescriptor{
        .name = "memory",
        .path = "benchmark.coverage.memory",
        .address = 0x100,
        .entries = 4,
        .width = 32,
        .access_width = 32,
        .access = RegisterAccess::ReadWrite,
    },
};

constexpr RegisterBlockDescriptor kRegisterCoverageBlock{
    .name = "coverage",
    .registers = kRegisterCoverageRegisters,
    .memories = kRegisterCoverageMemories,
};

Task<void> run_register_coverage(Context context) {
    using Transaction = MemoryTransaction<uint32_t, uint32_t, uint8_t>;
    RegisterAccessCoverage coverage{kRegisterCoverageBlock};
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint32_t index = iteration & 3u;
        coverage.write(Transaction{
            .operation = MemoryOperation::Write,
            .address = 0,
            .data = stimulus(iteration),
            .byte_enable = 0x1,
        });
        coverage.write(Transaction{
            .operation = MemoryOperation::Read,
            .address = 0,
        });
        coverage.write(Transaction{
            .operation = MemoryOperation::Write,
            .address = 0x100u + index * 4u,
            .data = stimulus(iteration),
            .byte_enable = 0xf,
        });
        coverage.write(Transaction{
            .operation = MemoryOperation::Read,
            .address = 0x100u + index * 4u,
        });
        coverage.write(Transaction{
            .operation = MemoryOperation::Read,
            .address = 0x1000,
        });
        coverage.write(Transaction{
            .operation = MemoryOperation::Read,
            .address = 0,
            .status = MemoryStatus::SlaveError,
        });
        coverage.sample_register(kRegisterCoverageRegisters[0],
                                 MemoryOperation::Write);
        coverage.sample_register(kRegisterCoverageRegisters[0],
                                 MemoryOperation::Read);
        coverage.sample_memory(kRegisterCoverageMemories[0], index,
                               MemoryOperation::Write);
        coverage.sample_memory(kRegisterCoverageMemories[0], index,
                               MemoryOperation::Read);
        context.result.transactions += 10;
        ++context.result.features.coverage_sampling;
    }

    const auto snapshot = coverage.snapshot();
    const auto* control = snapshot.find("benchmark.coverage.control");
    const auto* command = snapshot.find("benchmark.coverage.control.command");
    const auto* status = snapshot.find("benchmark.coverage.control.status");
    const auto* memory = snapshot.find("benchmark.coverage.memory");
    if (!control || !command || !status || !memory) {
        throw std::runtime_error("register coverage snapshot is incomplete");
    }
    const uint64_t iterations = context.iterations;
    check64(context, "register coverage samples", snapshot.samples,
            iterations * 8u);
    check64(context, "register coverage failed", snapshot.failed, iterations);
    check64(context, "register coverage unmapped", snapshot.unmapped,
            iterations);
    check64(context, "register coverage frontdoor aggregate",
            control->frontdoor_reads + control->frontdoor_writes,
            iterations * 2u);
    check64(context, "register coverage backdoor aggregate",
            control->backdoor_reads + control->backdoor_writes,
            iterations * 2u);
    check64(context, "register coverage command writes",
            command->frontdoor_writes, iterations);
    check64(context, "register coverage status reads",
            status->frontdoor_reads, iterations);
    check64(context, "register coverage memory frontdoor",
            memory->frontdoor_reads + memory->frontdoor_writes,
            iterations * 2u);
    check64(context, "register coverage memory backdoor",
            memory->backdoor_reads + memory->backdoor_writes,
            iterations * 2u);
    check64(context, "register coverage unique memory indices",
            memory->unique_read_indices + memory->unique_written_indices,
            std::min<uint64_t>(iterations, 4u) * 2u);
    check64(context, "register coverage iterations",
            context.result.features.coverage_sampling, iterations);
    check64(context, "register coverage transactions",
            context.result.transactions, iterations * 10u);
    constexpr uint32_t kFnvPrime = 0x0100'0193u;
    const std::array<uint64_t, 10> coverage_values{
        snapshot.samples,
        snapshot.failed,
        snapshot.unmapped,
        control->frontdoor_reads + control->frontdoor_writes,
        control->backdoor_reads + control->backdoor_writes,
        command->frontdoor_writes,
        status->frontdoor_reads,
        memory->frontdoor_reads + memory->frontdoor_writes,
        memory->backdoor_reads + memory->backdoor_writes,
        memory->unique_read_indices + memory->unique_written_indices,
    };
    for (const uint64_t value : coverage_values) {
        context.result.checksum =
            (context.result.checksum ^ static_cast<uint32_t>(value)) *
            kFnvPrime;
    }
    report(context);
    co_return;
}
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_MAPS
struct RegisterMapMaster {
    using address_type = uint32_t;
    using data_type = uint32_t;
    using byte_enable_type = uint8_t;
    using write_request_type =
        MemoryWriteRequest<address_type, data_type, byte_enable_type>;
    using read_request_type = MemoryReadRequest<address_type>;
    using write_response_type = MemoryWriteResponse;
    using read_response_type = MemoryReadResponse<data_type>;

    Task<write_response_type> write(write_request_type request) {
        words.at(request.address / 4u) = request.data;
        ++result.transactions;
        co_return write_response_type{};
    }

    Task<read_response_type> read(read_request_type request) {
        ++result.transactions;
        co_return read_response_type{.data = words.at(request.address / 4u)};
    }

    BenchResult& result;
    std::array<uint32_t, 16 * 1024> words{};
};

struct BenchmarkRegisterFrontdoor final
    : RegisterFrontdoor<RegisterMapMaster> {
    explicit BenchmarkRegisterFrontdoor(BenchResult& selected_result)
        : result(selected_result) {}

    Task<write_response_type> write(
        RegisterMapMaster&, const RegisterDescriptor&,
        write_request_type request) override {
        storage = request.data;
        last_address = request.address;
        ++result.transactions;
        co_return write_response_type{};
    }

    Task<read_response_type> read(
        RegisterMapMaster&, const RegisterDescriptor&,
        read_request_type request) override {
        last_address = request.address;
        ++result.transactions;
        co_return read_response_type{.data = storage};
    }

    BenchResult& result;
    uint32_t storage = 0;
    uint32_t last_address = 0;
};

constexpr RegisterDescriptor kRegisterMapDescriptor{
    .name = "control",
    .path = "benchmark.maps.control",
    .address = 0x20,
    .width = 32,
    .access_width = 32,
};

constexpr RegisterMemoryDescriptor kRegisterMapMemoryDescriptor{
    .name = "buffer",
    .path = "benchmark.maps.buffer",
    .address = 0x100,
    .entries = 4,
    .width = 32,
    .access_width = 32,
    .access = RegisterAccess::ReadWrite,
};

Task<void> run_register_maps(Context context) {
    TestContext test{context.scheduler, context.result};
    RegisterMapMaster master{context.result};
    BenchmarkRegisterFrontdoor custom{context.result};
    RegisterHandle model{test, master, kRegisterMapDescriptor};
    RegisterMemoryHandle memory{master, kRegisterMapMemoryDescriptor};

    RegisterAddressMap primary{"primary", master, 0x1000};
    RegisterAddressMap alias{"alias", master, 0x8000};
    alias.route(kRegisterMapDescriptor, 0x40)
        .route(kRegisterMapMemoryDescriptor, 0x200);
    RegisterAddressMap indirect{"indirect", master, 0x9000};
    indirect.route(kRegisterMapDescriptor, 0x60, &custom);

    uint64_t operations = 0;
    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint32_t value = stimulus(iteration);

        static_cast<void>(co_await model.write(value, primary));
        const auto primary_read = co_await model.read(primary);
        check(context, "primary map read", primary_read.data, value);
        check64(context, "primary map address",
                primary.effective_address(kRegisterMapDescriptor), 0x1020);

        const uint32_t alias_value = value ^ 0x5a5a'a5a5u;
        static_cast<void>(co_await model.write(alias_value, alias));
        const auto alias_read = co_await model.read(alias);
        check(context, "alias map read", alias_read.data, alias_value);
        check64(context, "alias map address",
                alias.effective_address(kRegisterMapDescriptor), 0x8040);

        const uint32_t indirect_value = value + 0x1020'3040u;
        static_cast<void>(co_await model.write(indirect_value, indirect));
        const auto indirect_read = co_await model.read(indirect);
        check(context, "custom frontdoor read", indirect_read.data,
              indirect_value);
        check64(context, "custom frontdoor address", custom.last_address,
                0x9060);

        const uint32_t index = iteration & 1u;
        const std::array<uint32_t, 2> values{value + 1u, value + 2u};
        std::array<uint32_t, 2> readback{};
        static_cast<void>(co_await memory.write(index, values, alias));
        static_cast<void>(co_await memory.read_into(index, readback, alias));
        check(context, "mapped memory first element", readback[0], values[0]);
        check(context, "mapped memory second element", readback[1],
              values[1]);
        const std::array<uint32_t, 5> observed_values{
            static_cast<uint32_t>(primary_read.data),
            static_cast<uint32_t>(alias_read.data),
            static_cast<uint32_t>(indirect_read.data), readback[0],
            readback[1]};
        for (const uint32_t observed : observed_values) {
            context.result.checksum =
                (context.result.checksum ^ observed) * 0x0100'0193u;
        }
        ++operations;
    }

    check64(context, "register map operations", operations,
            context.iterations);
    check64(context, "register map transactions", context.result.transactions,
            static_cast<uint64_t>(context.iterations) * 10u);
    report(context);
    co_return;
}
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_USER_EFFECTS
struct RegisterUserEffectMaster {
    using address_type = uint32_t;
    using data_type = uint32_t;
    using byte_enable_type = uint8_t;
    using write_request_type =
        MemoryWriteRequest<address_type, data_type, byte_enable_type>;
    using read_request_type = MemoryReadRequest<address_type>;
    using write_response_type = MemoryWriteResponse;
    using read_response_type = MemoryReadResponse<data_type>;

    Task<write_response_type> write(write_request_type request) {
        storage ^= request.data & 0xffu;
        ++result.transactions;
        co_return write_response_type{};
    }

    Task<read_response_type> read(read_request_type) {
        const uint32_t sampled = storage;
        storage = (~storage) & 0xffu;
        ++result.transactions;
        co_return read_response_type{.data = sampled};
    }

    BenchResult& result;
    uint32_t storage = 0;
};

struct BenchmarkUserEffectPolicy final : RegisterUserEffectPolicy {
    bool encode_write(
        const RegisterUserEffectBitContext& context) override {
        return context.previous_valid ? context.previous != context.value
                                      : context.value;
    }

    RegisterUserEffectBitResult predict_write(
        const RegisterUserEffectBitContext& context) override {
        return {.value = context.previous != context.value,
                .valid = context.previous_valid};
    }

    RegisterUserEffectBitResult predict_read(
        const RegisterUserEffectBitContext& context) override {
        return {.value = !context.value, .valid = true};
    }

    uint64_t encode_write_field(
        const RegisterUserEffectFieldContext& context) override {
        return context.previous ^ context.value;
    }

    RegisterUserEffectFieldResult predict_write_field(
        const RegisterUserEffectFieldContext& context) override {
        return {.value = context.previous ^ context.value,
                .valid_mask = context.previous_valid_mask};
    }

    RegisterUserEffectFieldResult predict_read_field(
        const RegisterUserEffectFieldContext& context) override {
        return {.value = ~context.value,
                .valid_mask = register_mask(
                    context.field_descriptor.width)};
    }
};

constexpr std::array kBenchmarkUserEffectFields{
    RegisterFieldDescriptor{
        .name = "custom",
        .path = "benchmark.user_effects.custom",
        .lsb = 0,
        .width = 8,
        .access = RegisterAccess::ReadWrite,
        .read_effect = RegisterReadEffect::User,
        .write_effect = RegisterWriteEffect::User,
    },
};

constexpr RegisterDescriptor kBenchmarkUserEffectDescriptor{
    .name = "user_effects",
    .path = "benchmark.user_effects",
    .width = 32,
    .access_width = 32,
    .fields = kBenchmarkUserEffectFields,
};

Task<void> run_register_user_effects(Context context) {
    TestContext test{context.scheduler, context.result};
    RegisterUserEffectMaster master{context.result};
    BenchmarkUserEffectPolicy policy;
    RegisterHandle model{test, master, kBenchmarkUserEffectDescriptor,
                         nullptr, &policy};
    uint64_t operations = 0;

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint32_t initial = stimulus(iteration * 2u) & 0xffu;
        const uint32_t desired = stimulus(iteration * 2u + 1u) & 0xffu;
        master.storage = initial;
        model.predict(initial, RegisterPrediction::Direct);
        model.set_desired(desired);
        const auto write = co_await model.update();
        check(context, "user effect write mirror", model.mirrored(), desired);
        check(context, "user effect write DUT", master.storage, desired);

        const auto read = co_await model.read();
        check(context, "user effect read mirror", model.mirrored(),
              master.storage);
        check(context, "user effect validity",
              static_cast<uint32_t>(model.mirrored_valid_mask() & 0xffu),
              0xffu);
        context.result.checksum =
            (context.result.checksum ^ desired) * 0x0100'0193u;
        context.result.checksum =
            (context.result.checksum ^ (master.storage & 0xffu)) *
            0x0100'0193u;
        if (!write.okay() || !read.okay()) {
            throw std::runtime_error("register user-effect transport failed");
        }
        ++operations;
    }

    check64(context, "register user-effect operations", operations,
            context.iterations);
    check64(context, "register user-effect transactions",
            context.result.transactions,
            static_cast<uint64_t>(context.iterations) * 2u);
    report(context);
    co_return;
}
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_MEMORY
struct RegisterMemoryMaster {
    using address_type = uint32_t;
    using data_type = uint32_t;
    using byte_enable_type = uint8_t;
    using write_request_type =
        MemoryWriteRequest<address_type, data_type, byte_enable_type>;
    using read_request_type = MemoryReadRequest<address_type>;
    using write_response_type = MemoryWriteResponse;
    using read_response_type = MemoryReadResponse<data_type>;

    Task<write_response_type> write(write_request_type) {
        co_return write_response_type{};
    }
    Task<read_response_type> read(read_request_type) {
        co_return read_response_type{};
    }
};

constexpr RegisterMemoryDescriptor kRegisterMemoryDescriptor{
    .name = "memory",
    .path = "benchmark.memory",
    .address = 0x100,
    .entries = 256,
    .width = 32,
    .access_width = 32,
    .access = RegisterAccess::ReadWrite,
};

class BenchmarkMemoryBackdoor final
    : public RegisterMemoryBackdoor<uint32_t> {
   public:
    explicit BenchmarkMemoryBackdoor(AuthoringCoreDut dut) : dut_(dut) {}

    uint32_t peek(const RegisterMemoryDescriptor&, uint64_t index,
                  uint64_t) override {
        return static_cast<uint32_t>(
            dut_.memory.at(static_cast<int32_t>(index)).get());
    }

    void poke(const RegisterMemoryDescriptor&, uint64_t index, uint64_t,
              uint32_t value) override {
        dut_.memory.at(static_cast<int32_t>(index)).deposit(value);
    }

    void peek_into(const RegisterMemoryDescriptor&, uint64_t first_index,
                   uint64_t, std::span<uint32_t> values) override {
        dut_.memory.get_into(static_cast<int32_t>(first_index), values);
    }

    void poke(const RegisterMemoryDescriptor&, uint64_t first_index,
              uint64_t, std::span<const uint32_t> values) override {
        dut_.memory.deposit(static_cast<int32_t>(first_index), values);
    }

   private:
    AuthoringCoreDut dut_;
};

Task<void> run_register_memory(Context context) {
    RegisterMemoryMaster master;
    BenchmarkMemoryBackdoor backdoor{context.dut};
    RegisterMemoryHandle memory{
        master, kRegisterMemoryDescriptor, 0x4000, &backdoor};
    uint64_t operations = 0;

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        const uint64_t first_index = (iteration & 63u) * 4u;
        std::array<uint32_t, 4> values{};
        std::array<uint32_t, 4> readback{};
        for (uint32_t word = 0; word < values.size(); ++word) {
            values[word] = stimulus(iteration * 4u + word) ^ 0x3c6e'f372u;
        }
        typename decltype(memory)::write_response_type write;
        typename decltype(memory)::read_response_type read;
        if (iteration % 3u == 0) {
            auto window = memory.slice(first_index, values.size());
            write = co_await window.write(values, AccessPath::Backdoor);
            read = co_await window.read(readback, AccessPath::Backdoor);
        } else if (iteration % 3u == 1) {
            const uint64_t byte_offset = first_index * memory.element_bytes();
            write = co_await memory.write_offset(
                byte_offset, values, AccessPath::Backdoor);
            read = co_await memory.read_offset(
                byte_offset, readback, AccessPath::Backdoor);
        } else {
            const uint64_t absolute_address =
                memory.base_address() + first_index * memory.element_bytes();
            write = co_await memory.write_absolute(
                absolute_address, values, AccessPath::Backdoor);
            read = co_await memory.read_absolute(
                absolute_address, readback, AccessPath::Backdoor);
        }
        check64(context, "register memory write count",
                write.transfers_completed, values.size());
        check64(context, "register memory read count",
                read.transfers_completed, readback.size());
        for (uint32_t word = 0; word < values.size(); ++word) {
            check(context, "register memory readback", readback[word],
                  values[word]);
            context.result.checksum =
                (context.result.checksum ^ readback[word]) * 0x0100'0193u;
        }
        ++operations;
    }
    check64(context, "register memory operations", operations,
            context.iterations);
    check64(context, "register memory transactions",
            context.result.transactions, 0);
    report(context);
    co_return;
}
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MIXED_LOGGING
struct MixedLogSink final : LogSink {
    uint64_t records = 0;
    uint64_t cpp_records = 0;
    uint64_t sv_records = 0;
    uint64_t complete_records = 0;

    void emit(const LogRecord& record) override {
        ++records;
        if (record.origin == LogOrigin::Cpp) ++cpp_records;
        if (record.origin == LogOrigin::SystemVerilog) ++sv_records;
        const bool cpp_complete =
            record.origin == LogOrigin::Cpp &&
            record.message == "C++ checkpoint" &&
            record.scope == "scoreboard" && record.hierarchy.empty();
        const bool sv_complete =
            record.origin == LogOrigin::SystemVerilog &&
            record.message == "SV checkpoint" &&
            record.scope == "rtl.request" && !record.hierarchy.empty() &&
            record.process_id == 0;
        if ((cpp_complete || sv_complete) &&
            record.test_name == "mixed_logging" &&
            !record.source_file.empty() && record.source_line != 0 &&
            record.sequence == records) {
            ++complete_records;
        }
    }
};
#endif

Task<void> run(Context context) {
#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MIXED_LOGGING
    MixedLogSink mixed_log_sink;
    LogHistory mixed_log_history;
    const uint64_t expected_language_records =
        (static_cast<uint64_t>(context.iterations) + 1023u) / 1024u;
    mixed_log_history.reserve(2u * expected_language_records);
    context.result.test_name = "mixed_logging";
    TestContext mixed_log_test{
        context.scheduler, context.result, {}, nullptr,
        LoggingOptions{.minimum_level = LogLevel::Info,
                       .sink = &mixed_log_sink,
                       .history = &mixed_log_history}};
    mixed_log_test.bind_sim_logs(*context.sim_logs);
    auto mixed_log = mixed_log_test.logger("scoreboard");
    uint64_t disabled_mixed_log_factories = 0;
#endif
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
    context.dut.apb_psel_i.set(0);
    context.dut.apb_penable_i.set(0);
    context.dut.apb_pwrite_i.set(0);
    context.dut.apb_paddr_i.set(0);
    context.dut.apb_pwdata_i.set(0);
    context.dut.apb_pstrb_i.set(0);
    for (uint32_t cycle = 0; cycle < 4; ++cycle) {
        co_await RisingEdge{context.dut.clk};
    }
    context.dut.rst_n.set(1);

    Event event;
    Queue<uint32_t> queue;
    uint32_t previous_cycle_count = 0;

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_RANDOM_STIMULUS
    TestContext random_test{context.scheduler, context.result};
    auto& random = random_test.random();
    constexpr std::array random_masks{
        weighted(0x0000'0000u, 1), weighted(0x0101'0101u, 2),
        weighted(0x1357'9bdfu, 3), weighted(0xa5a5'5a5au, 4)};
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_CONSTRAINED_PACKET
    TestContext constrained_test{context.scheduler, context.result};
    PacketStimulus packet;
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_CONSTRAINT_EXTENSIONS
    TestContext extension_test{context.scheduler, context.result};
    ExtendedPacketStimulus extended_packet;
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_COVERAGE_SAMPLING
    Covergroup<CoverageTransaction> functional_coverage{"transactions"};
    auto& opcode_coverage = functional_coverage.coverpoint(
        "opcode", &CoverageTransaction::opcode);
    opcode_coverage.bin("read", uint8_t{0})
        .bin("write", uint8_t{1})
        .bin("atomic", uint8_t{2})
        .illegal_bin("reserved", uint8_t{3})
        .transition_bin("read_to_write", uint8_t{0}, uint8_t{1});
    auto& length_coverage = functional_coverage.coverpoint(
        "length", &CoverageTransaction::length);
    length_coverage.ignore_bin("empty", uint16_t{0})
        .bin("short", uint16_t{1}, uint16_t{63})
        .bin("medium", uint16_t{64}, uint16_t{511})
        .bin("long", uint16_t{512}, uint16_t{1500})
        .illegal_bin("oversize", uint16_t{1501}, uint16_t{1599});
    functional_coverage.cross("opcode_x_length", opcode_coverage,
                              length_coverage);
    uint64_t expected_coverage_cross_hits = 0;
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEM_PROBE_READ || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEM_PROBE_DEPOSIT || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEM_PROBE_READ_DEPOSIT
    co_await seed_probe_memory(context);
#endif

    for (uint32_t iteration = 0; iteration < context.iterations; ++iteration) {
        uint32_t payload = stimulus(iteration);

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MIXED_LOGGING
        mixed_log.debug([&] {
            ++disabled_mixed_log_factories;
            return "C++ transaction " + std::to_string(iteration);
        });
        if ((iteration & 1023u) == 0) mixed_log.info("C++ checkpoint");
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_RANDOM_STIMULUS
        payload = random.randint<uint32_t>(
            0, std::numeric_limits<uint32_t>::max());
        payload ^= random.weighted_choice(random_masks);
        const auto wide_random = random.randbits<65>();
        payload ^= wide_random.word(0) ^ wide_random.word(1);
        if (wide_random.word(2) != 0) payload ^= 0x8000'0000u;
        std::array<uint32_t, 4> order{0, 1, 2, 3};
        random.shuffle(order);
        payload ^= order[0] | (order[1] << 4) | (order[2] << 8) |
                   (order[3] << 12);
        ++context.result.features.random_stimulus;
#endif


#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_CONSTRAINED_PACKET
        constrained_test.randomize(packet);
        payload = packet.payload();
        ++context.result.features.constrained_packet;
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_CONSTRAINT_EXTENSIONS
        extension_test.randomize(extended_packet);
        payload = extended_packet.payload();
        ++context.result.features.constraint_extensions;
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_COVERAGE_SAMPLING
        const auto coverage_opcode = static_cast<uint8_t>(iteration & 3u);
        const auto coverage_length =
            static_cast<uint16_t>((iteration * 37u) % 1600u);
        static_cast<void>(functional_coverage.sample(
            CoverageTransaction{coverage_opcode, coverage_length}));
        if (coverage_opcode != 3u && coverage_length != 0u &&
            coverage_length <= 1500u) {
            ++expected_coverage_cross_hits;
        }
        ++context.result.features.coverage_sampling;
#endif

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

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_QUEUE || \
    AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ALL
        ++context.result.features.queue_send;
        queue.put_nowait(payload);
        ++context.result.features.queue_receive;
        const uint32_t received = co_await queue.get();
        check(context, "queue payload", received, payload);
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

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_HIER_DATA
        co_await hier_data_feature(context, iteration);
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

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MIXED_LOGGING
    uint64_t ordered_mixed_records = 0;
    uint64_t previous_mixed_time = 0;
    for (size_t index = 0; index < mixed_log_history.size(); ++index) {
        const auto& record = mixed_log_history[index];
        if (record.sequence == index + 1 &&
            (index == 0 ||
             record.simulation_time_fs >= previous_mixed_time)) {
            ++ordered_mixed_records;
        }
        previous_mixed_time = record.simulation_time_fs;
    }
    check64(context, "mixed log output", mixed_log_sink.records,
            2u * expected_language_records);
    check64(context, "mixed log history", mixed_log_history.size(),
            2u * expected_language_records);
    check64(context, "mixed log order", ordered_mixed_records,
            2u * expected_language_records);
    check64(context, "mixed C++ records", mixed_log_sink.cpp_records,
            expected_language_records);
    check64(context, "mixed SV records", mixed_log_sink.sv_records,
            expected_language_records);
    check64(context, "mixed log metadata", mixed_log_sink.complete_records,
            2u * expected_language_records);
    check64(context, "disabled mixed log factories",
            disabled_mixed_log_factories, 0);
#endif

#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_COVERAGE_SAMPLING
    const auto coverage = functional_coverage.snapshot();
    uint64_t opcode_accounted = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        opcode_accounted += coverage.points[0].bins[index].hits;
    }
    uint64_t length_accounted = 0;
    for (std::size_t index = 0; index < 5; ++index) {
        length_accounted += coverage.points[1].bins[index].hits;
    }
    uint64_t cross_hits = 0;
    for (const auto& bin : coverage.crosses[0].bins) cross_hits += bin.hits;
    check64(context, "coverage samples", coverage.samples,
            context.iterations);
    check64(context, "coverage opcode accounting", opcode_accounted,
            context.iterations);
    check64(context, "coverage length accounting", length_accounted,
            context.iterations);
    check64(context, "coverage transition hits",
            coverage.points[0].bins[4].hits,
            (static_cast<uint64_t>(context.iterations) + 2u) / 4u);
    check64(context, "coverage cross hits", cross_hits,
            expected_coverage_cross_hits);
#endif

    co_await wait_for_response_count(context);
    check(context, "request count", context.dut.request_count.get(),
          context.iterations);
    check(context, "response count", context.dut.response_count.get(),
          context.iterations);
    report(context);
}

}  // namespace

void register_benchmark(coro::Testbench& scheduler, AuthoringCoreDut dut,
                        uint32_t iterations, BenchResult& result,
                        coro::ClockRegistrar clocks,
                        cpptb::detail::SimLogEndpoint& sim_logs) {
    result = BenchResult{};
    result.start = std::chrono::steady_clock::now();
    dut.clk.set(0);
    clocks.start(dut.clk, 2_ns);
#if AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_FORCE_DIRECT
    scheduler.spawn_detached(
        run_force_direct(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_TIMING_PHASES
    scheduler.spawn_detached(
        run_timing_phases(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_QUEUE_SYNC
    scheduler.spawn_detached(
        run_queue_sync(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_TEST_LIFECYCLE
    scheduler.spawn_detached(
        run_test_lifecycle(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_DYNAMIC_SPAWN
    scheduler.spawn_detached(
        run_dynamic_spawn(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_DYNAMIC_TASK
    scheduler.spawn_detached(
        run_dynamic_task(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_DYNAMIC_SPAWN_SCHEDULER
    scheduler.spawn_detached(run_dynamic_spawn_scheduler(
        Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_DYNAMIC_SPAWN_SUSPENDING
    scheduler.spawn_detached(run_dynamic_spawn_suspending(
        Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_DYNAMIC_MONITOR
    scheduler.spawn_detached(
        run_dynamic_monitor(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_PROCESS_PIPELINE
    scheduler.spawn_detached(
        run_process_pipeline(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_ANALYSIS_FANOUT
    scheduler.spawn_detached(
        run_analysis_fanout(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_APB_COMPONENT
    scheduler.spawn_detached(
        run_apb_component(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_TRANSACTION_RECORDING
    scheduler.spawn_detached(
        run_transaction_recording(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEMORY_MODEL
    scheduler.spawn_detached(
        run_memory_model(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MEMORY_MODEL_DIRECT
    scheduler.spawn_detached(
        run_memory_model_direct(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_PREDICTION_VALIDITY
    scheduler.spawn_detached(run_register_prediction_validity(
        Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_BACKDOOR
    scheduler.spawn_detached(
        run_register_backdoor(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_HIERARCHY
    scheduler.spawn_detached(
        run_register_hierarchy(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_SPLIT
    scheduler.spawn_detached(
        run_register_split(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_WIDE
    scheduler.spawn_detached(
        run_register_wide(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_ENUM
    scheduler.spawn_detached(
        run_register_enum(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_MEMORY
    scheduler.spawn_detached(
        run_register_memory(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_SEQUENCES
    scheduler.spawn_detached(
        run_register_sequences(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_COVERAGE
    scheduler.spawn_detached(
        run_register_coverage(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_MAPS
    scheduler.spawn_detached(
        run_register_maps(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_REGISTER_USER_EFFECTS
    scheduler.spawn_detached(run_register_user_effects(
        Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_STRUCTURED_LOGGING
    scheduler.spawn_detached(
        run_structured_logging(Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_STRUCTURED_LOG_HISTORY
    scheduler.spawn_detached(run_structured_log_history(
        Context{scheduler, dut, iterations, result}));
#elif AUTHORING_CORE_KERNEL == AUTHORING_CORE_KERNEL_MIXED_LOGGING
    scheduler.spawn_detached(
        run(Context{scheduler, dut, iterations, result, &sim_logs}));
#else
    scheduler.spawn_detached(run(Context{scheduler, dut, iterations, result}));
#endif
}

}  // namespace cpptb::benchmarks::authoring_core
