#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <type_traits>

#include "cpptb/hierarchy.hpp"

namespace {

struct DiscoveryTransport {};

using Counter = cpptb::hierarchy::Signal<DiscoveryTransport, 7,
                                         "block1.counter", 8, true>;
using Observed = cpptb::hierarchy::Signal<DiscoveryTransport, 8,
                                          "block1.observed", 8, false>;
using Memory = cpptb::hierarchy::Memory<DiscoveryTransport, 9,
                                        "block1.memory", 16, 0, 3, true>;
using TwoState = cpptb::hierarchy::Signal<
    DiscoveryTransport, 10, "block1.two_state", 8, true,
    cpptb::probe::Value<8>, false>;

template <typename T>
concept HasGetLogic = requires(T signal) { signal.get_logic(); };

template <typename T>
concept HasDepositLogic = requires(T signal) {
    signal.deposit_logic(cpptb::LogicBits<T::width>{});
};

template <typename T>
concept HasForceLogic = requires(T signal) {
    signal.force_logic(cpptb::LogicBits<T::width>{});
};

static_assert(std::is_empty_v<Counter>);
static_assert(std::is_empty_v<Observed>);
static_assert(std::is_empty_v<Memory>);
static_assert(!TwoState::four_state);
static_assert(HasGetLogic<Counter>);
static_assert(HasDepositLogic<Counter>);
static_assert(HasForceLogic<Counter>);
static_assert(!HasGetLogic<TwoState>);
static_assert(!HasDepositLogic<TwoState>);
static_assert(!HasForceLogic<TwoState>);

constexpr auto logic_pattern = cpptb::LogicBits<4>::from_string("10xz");
static_assert(logic_pattern.state(3) == cpptb::LogicState::One);
static_assert(logic_pattern.state(2) == cpptb::LogicState::Zero);
static_assert(logic_pattern.state(1) == cpptb::LogicState::X);
static_assert(logic_pattern.state(0) == cpptb::LogicState::Z);
static_assert(logic_pattern.contains_x());
static_assert(logic_pattern.contains_z());
static_assert(!logic_pattern.is_known());

struct DpiLogicWord {
    std::uint32_t aval = 0;
    std::uint32_t bval = 0;
};

constexpr auto dpi_logic_words = logic_pattern.dpi_words<DpiLogicWord>();
static_assert(dpi_logic_words[0].aval == 0xau);
static_assert(dpi_logic_words[0].bval == 0x3u);
static_assert(cpptb::LogicBits<4>::from_dpi_words(dpi_logic_words.data()) ==
              logic_pattern);
static_assert(cpptb::LogicBits<8>::from_uint(0x5a).to_bits().to_uint() ==
              0x5au);

struct Scope {
    [[no_unique_address]] Counter counter;
    [[no_unique_address]] Observed observed;
    [[no_unique_address]] Memory memory;
};

static_assert(sizeof(Scope) == 1);

void compiled_but_never_run(Scope dut) {
    (void)dut.counter.get();
    dut.counter.deposit(0x12);
    dut.counter.force(0x34);
    (void)dut.counter.get_logic();
    dut.counter.deposit_logic(cpptb::LogicBits<8>::from_uint(0x56));
    dut.counter.force_logic(cpptb::LogicBits<8>::from_uint(0x78));
    dut.counter.release();
    (void)dut.memory.at(2).get();
    dut.memory.at(2).deposit(0xbeef);
}

bool contains(std::string_view path, cpptb::hierarchy::Operation operation) {
    const auto plan = cpptb::hierarchy::discovered_access_plan();
    return std::find(plan.begin(), plan.end(),
                     cpptb::hierarchy::Access{path, operation}) != plan.end();
}

}  // namespace

int main() {
    const auto plan = cpptb::hierarchy::discovered_access_plan();
    bool passed = plan.size() == 9;
    passed &= contains("block1.counter", cpptb::hierarchy::Operation::Get);
    passed &= contains("block1.counter", cpptb::hierarchy::Operation::Deposit);
    passed &= contains("block1.counter", cpptb::hierarchy::Operation::Force);
    passed &= contains("block1.counter",
                       cpptb::hierarchy::Operation::GetLogic);
    passed &= contains("block1.counter",
                       cpptb::hierarchy::Operation::DepositLogic);
    passed &= contains("block1.counter",
                       cpptb::hierarchy::Operation::ForceLogic);
    passed &= contains("block1.counter", cpptb::hierarchy::Operation::Release);
    passed &= contains("block1.memory", cpptb::hierarchy::Operation::Get);
    passed &= contains("block1.memory", cpptb::hierarchy::Operation::Deposit);
    passed &= !contains("block1.observed", cpptb::hierarchy::Operation::Get);
    if (!passed) {
        std::fprintf(stderr, "unexpected hierarchy access plan:\n");
        for (const auto access : plan) {
            std::fprintf(stderr, "  %.*s %.*s\n",
                         static_cast<int>(access.path.size()),
                         access.path.data(),
                         static_cast<int>(
                             cpptb::hierarchy::operation_name(access.operation)
                                 .size()),
                         cpptb::hierarchy::operation_name(access.operation)
                             .data());
        }
    }
    return passed ? 0 : 1;
}
