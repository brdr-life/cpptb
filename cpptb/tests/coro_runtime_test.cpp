#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "cpptb/coro_runtime.hpp"
#include "cpptb/dpi_static_binding.hpp"
#include "cpptb/probe.hpp"

namespace {

using namespace cpptb::coro;

template <typename SignalType>
concept WritablePackedSignal = requires(
    SignalType signal, typename SignalType::value_type value) {
    signal.set(value);
};

static_assert(WritablePackedSignal<DrivenSignal<65>>);
static_assert(!WritablePackedSignal<ObservedSignal<65>>);
static_assert(WritablePackedSignal<decltype(
              std::declval<DrivenArray<65, 7, 4>>().at(4))>);
static_assert(!WritablePackedSignal<decltype(
              std::declval<ObservedArray<65, 7, 4>>().at(4))>);
using RankTwoDriven =
    DrivenFixedArray<65, ArrayDimension<2, 1>, ArrayDimension<-1, 1>>;
using RankThreeObserved =
    ObservedFixedArray<8, ArrayDimension<0, 1>, ArrayDimension<4, 3>,
                       ArrayDimension<-2, -1>>;
static_assert(WritablePackedSignal<decltype(
              std::declval<RankTwoDriven>().at(1).at(-1))>);
static_assert(!WritablePackedSignal<decltype(
              std::declval<RankThreeObserved>().at(0).at(3).at(-2))>);
static_assert(RankTwoDriven::rank == 2);
static_assert(RankTwoDriven::word_count == 18);
static_assert(FixedArraySpec<8, false, ArrayDimension<0, 1>,
                             ArrayDimension<4, 3>,
                             ArrayDimension<-2, -1>>::word_count == 8);

template <bool Writable, bool Driven>
concept ValidStaticPackedSpec = requires {
    typename cpptb::dpi::StaticPackedSignalSpec<1, Writable, Driven, 0, 0>;
};

std::array<uint32_t, 4> static_on_demand_words{};

void static_on_demand_get(uint32_t offset, uint32_t* words,
                          uint32_t word_count) {
    for (uint32_t word = 0; word < word_count; ++word) {
        words[word] = static_on_demand_words.at(offset + word);
    }
}

void static_on_demand_set(uint32_t offset, const uint32_t* words,
                          uint32_t word_count) {
    for (uint32_t word = 0; word < word_count; ++word) {
        static_on_demand_words.at(offset + word) = words[word];
    }
}

template <bool Writable, bool Driven,
          cpptb::dpi::OnDemandSetWordsFn SetWords>
concept ValidStaticOnDemandSpec = requires {
    typename cpptb::dpi::StaticOnDemandSignalSpec<
        1, Writable, Driven, 0, static_on_demand_get, SetWords>;
};

using StaticPackedScalar =
    cpptb::dpi::StaticPackedSignal<1, true, true, 3, 1>;
using StaticObservedPacked =
    cpptb::dpi::StaticPackedSignal<64, false, false, 4, 0>;
using StaticOnDemandScalar =
    cpptb::dpi::StaticOnDemandSignal<1, true, true, 6>;
using StaticOnDemandMatrix = cpptb::dpi::StaticOnDemandFixedArray<
    137, false, false, 7, 0, ArrayDimension<2, 1>,
    ArrayDimension<-1, 1>>;

static_assert(ValidStaticPackedSpec<true, true>);
static_assert(ValidStaticPackedSpec<false, false>);
static_assert(!ValidStaticPackedSpec<true, false>);
static_assert(!ValidStaticPackedSpec<false, true>);
static_assert(ValidStaticOnDemandSpec<true, true, static_on_demand_set>);
static_assert(ValidStaticOnDemandSpec<false, false, nullptr>);
static_assert(!ValidStaticOnDemandSpec<true, true, nullptr>);
static_assert(!ValidStaticOnDemandSpec<false, false, static_on_demand_set>);
static_assert(WritablePackedSignal<StaticPackedScalar>);
static_assert(!WritablePackedSignal<StaticObservedPacked>);
static_assert(WritablePackedSignal<StaticOnDemandScalar>);
static_assert(!WritablePackedSignal<decltype(
              std::declval<StaticOnDemandMatrix>().at(1).at(-1))>);
static_assert(std::convertible_to<StaticPackedScalar, Signal>);
static_assert(std::convertible_to<StaticOnDemandScalar, Signal>);
static_assert(StaticPackedScalar::transport_offset == 1);
static_assert(StaticOnDemandMatrix::base_id == 7);

constexpr std::array<uint32_t, 3> kStaticObservedWordIds{4, 8, 9};
constexpr std::array<uint32_t, 3> kStaticDrivenWordIds{0, 1, 2};
constexpr std::array<cpptb::dpi::StaticPackedBindingSpan, 3>
    kValidStaticSpans{{{4, 1, 0, false},
                       {8, 2, 1, false},
                       {0, 3, 0, true}}};
constexpr std::array<cpptb::dpi::StaticPackedBindingSpan, 3>
    kStaleStaticSpans{{{4, 1, 1, false},
                       {8, 2, 0, false},
                       {0, 3, 0, true}}};
static_assert(cpptb::dpi::validate_static_packed_binding_spans(
    kValidStaticSpans, kStaticObservedWordIds, kStaticDrivenWordIds));
static_assert(!cpptb::dpi::validate_static_packed_binding_spans(
    kStaleStaticSpans, kStaticObservedWordIds, kStaticDrivenWordIds));

template <typename ProbeType>
concept WritableProbe = requires(
    ProbeType value, typename ProbeType::value_type data) {
    value.deposit(data);
};

template <typename ProbeType>
concept EdgeWaitableProbe = requires(ProbeType value) { RisingEdge{value}; };

template <typename ProbeType>
concept ForceableProbe = requires(
    ProbeType value, typename ProbeType::value_type data) {
    value.force(data);
    value.release();
};

static_assert(WritableProbe<cpptb::probe::Probe<32, true>>);
static_assert(!WritableProbe<cpptb::probe::Probe<32, false>>);
static_assert(ForceableProbe<cpptb::probe::Probe<32, false, true>>);
static_assert(!ForceableProbe<cpptb::probe::Probe<32, true, false>>);
static_assert(!EdgeWaitableProbe<cpptb::probe::Probe<32, true>>);

std::array<uint32_t, 12> probe_words{};
uint32_t narrow_probe_value = 0;
uint32_t narrow_force_value = 0;
uint32_t narrow_release_count = 0;

uint32_t narrow_probe_get(int32_t) { return narrow_probe_value; }

void narrow_probe_deposit(int32_t, uint32_t value) {
    narrow_probe_value = value;
}

void narrow_probe_force(int32_t, uint32_t value) {
    narrow_force_value = value;
}

void narrow_probe_release(int32_t) { ++narrow_release_count; }

cpptb::Bits<73> probe_get(int32_t index) {
    const size_t offset = static_cast<size_t>(index - 4) * 3;
    cpptb::Bits<73>::word_array words{};
    for (size_t word = 0; word < 3; ++word) {
        words.at(word) = probe_words.at(offset + word);
    }
    return cpptb::Bits<73>::from_words(words);
}

void probe_deposit(int32_t index, cpptb::Bits<73> value) {
    const size_t offset = static_cast<size_t>(index - 4) * 3;
    for (size_t word = 0; word < 3; ++word) {
        probe_words.at(offset + word) = value.word(word);
    }
}

struct ArrayTransport {
    std::array<uint32_t, 32> words{};
};

uint32_t array_get(void* context, uint32_t id) {
    return static_cast<ArrayTransport*>(context)->words.at(id);
}

void array_set(void* context, uint32_t id, uint32_t value) {
    static_cast<ArrayTransport*>(context)->words.at(id) = value;
}

void array_get_words(void* context, uint32_t id, uint32_t* words,
                     uint32_t word_count) {
    auto& source = static_cast<ArrayTransport*>(context)->words;
    for (uint32_t word = 0; word < word_count; ++word) {
        words[word] = source.at(id + word);
    }
}

void array_set_words(void* context, uint32_t id, const uint32_t* words,
                     uint32_t word_count) {
    auto& destination = static_cast<ArrayTransport*>(context)->words;
    for (uint32_t word = 0; word < word_count; ++word) {
        destination.at(id + word) = words[word];
    }
}

struct Results {
    uint32_t rising = 0;
    uint32_t falling = 0;
    uint32_t changed = 0;
    uint32_t delayed = 0;
    uint32_t first_winner = 99;
    uint32_t variadic_first_winner = 99;
    uint32_t precise_delays = 0;
    uint32_t joined = 0;
    uint32_t variadic_joined = 0;
    uint32_t process_completed = 0;
    uint32_t process_waiters = 0;
    uint32_t cancelled_child_resumes = 0;
    uint32_t self_cancel_markers = 0;
    uint32_t normal_self_cancel = 0;
    uint32_t cancel_await_markers = 0;
    uint32_t timeout_winners = 0;
    uint32_t edge_winners = 0;
};

struct DestructionCounter {
    uint32_t* value;

    ~DestructionCounter() { ++*value; }
};

Task<void> wait_for_edges(Signal signal, Results& results) {
    co_await RisingEdge{signal};
    ++results.rising;
    co_await FallingEdge{signal};
    ++results.falling;
}

Task<void> wait_for_change(Signal signal, Results& results) {
    co_await Edge{signal};
    ++results.changed;
}

#ifdef CPPTB_CORO_WAIT_PATH_DIAGNOSTICS
Task<void> fast_path_rearm(Signal signal, uint32_t& stage) {
    co_await RisingEdge{signal};
    stage = 1;
    co_await FallingEdge{signal};
    stage = 2;
}

Task<void> fast_path_any(Signal signal, uint32_t& wakes) {
    co_await Edge{signal};
    ++wakes;
}

Task<void> fast_path_cancelled_falling(Signal signal, uint32_t& wakes) {
    co_await FallingEdge{signal};
    ++wakes;
}

Task<void> generic_first_three_edges(Signal rising_signal,
                                     Signal falling_signal,
                                     Signal any_signal, uint32_t& winner,
                                     uint32_t& completions) {
    winner = static_cast<uint32_t>(
        co_await First{RisingEdge{rising_signal}, FallingEdge{falling_signal},
                       Edge{any_signal}});
    ++completions;
}

Task<void> generic_first_path(Signal signal, uint32_t& winner) {
    winner = static_cast<uint32_t>(
        co_await First{RisingEdge{signal}, Delay{10_ns}});
}

Task<void> generic_timeout_path(Signal signal, TimeoutOutcome& outcome) {
    outcome = co_await with_timeout(RisingEdge{signal}, 2_ns);
}
#endif

Task<void> wait_for_delay(Testbench& tb, Results& results) {
    co_await Delay{7_ns};
    if (tb.now() == 7_ns) ++results.delayed;
}

Task<void> wait_for_first(Signal signal, Results& results) {
    results.first_winner =
        static_cast<uint32_t>(co_await First{RisingEdge{signal}, Delay{11_ns}});
}

Task<void> wait_for_precise_delays(Testbench& tb, Results& results) {
    co_await Delay{1_ps};
    if (tb.now() == 1_ps) ++results.precise_delays;
    co_await Delay{999_fs};
    if (tb.now() == 1'999_fs) ++results.precise_delays;
}

Task<void> join_child(Signal signal, uint32_t& value) {
    co_await RisingEdge{signal};
    ++value;
}

Task<void> wait_for_join(Signal first_signal, Signal second_signal, Results& results) {
    co_await Join{join_child(first_signal, results.joined),
                  join_child(second_signal, results.joined)};
    results.joined += 10;
}

Task<void> delay_marker(SimTime delay, uint32_t marker, uint32_t& value) {
    co_await Delay{delay};
    value |= marker;
}

Task<void> wait_for_variadic_join(Results& results) {
    co_await Join{
        delay_marker(1_ns, 0x1, results.variadic_joined),
        delay_marker(2_ns, 0x2, results.variadic_joined),
        delay_marker(3_ns, 0x4, results.variadic_joined),
    };
    results.variadic_joined |= 0x8;
}

Task<void> wait_for_variadic_first(Results& results) {
    results.variadic_first_winner = static_cast<uint32_t>(
        co_await First{Delay{9_ns}, Delay{4_ns}, Delay{6_ns}});
}

Task<void> process_worker(Results& results) {
    co_await Delay{3_ns};
    ++results.process_completed;
}

Task<void> wait_for_process(Process process, Results& results) {
    co_await process;
    ++results.process_waiters;
}

Task<void> cancellable_child(Results& results) {
    co_await Delay{20_ns};
    ++results.cancelled_child_resumes;
}

Task<void> cancellable_parent(Results& results) {
    co_await cancellable_child(results);
    ++results.cancelled_child_resumes;
}

Task<void> cancellable_join_parent(Results& results) {
    co_await Join{
        cancellable_child(results),
        cancellable_child(results),
        cancellable_child(results),
    };
    ++results.cancelled_child_resumes;
}

Task<void> self_cancelling_process(Process* self, Results& results) {
    co_await Delay{1_ns};
    self->cancel();
    if (!self->cancelled() && !self->done()) {
        results.self_cancel_markers |= 0x1;
    }
    co_await Delay{1_ns};
    results.self_cancel_markers |= 0x2;
}

Task<void> self_cancel_then_return(Process* self, Results& results) {
    co_await Delay{1_ns};
    self->cancel();
    if (!self->cancelled() && !self->done()) {
        results.normal_self_cancel |= 0x1;
    }
    co_return;
}

Task<void> cancel_and_await_process(Process target, Results& results) {
    target.cancel();
    if (!target.done() && !target.cancelled()) {
        results.cancel_await_markers |= 0x1;
    }
    co_await target;
    if (target.done() && target.cancelled()) {
        results.cancel_await_markers |= 0x2;
    }
}

Task<void> immediate_root(uint32_t& runs, uint32_t& destructions) {
    DestructionCounter counter{&destructions};
    ++runs;
    co_return;
}

Task<void> ordered_timer(uint32_t id, std::vector<uint32_t>& order) {
    co_await Delay{5_ns};
    order.push_back(id);
}

Task<void> first_falling_or_timeout(Signal signal, Results& results) {
    const size_t winner =
        co_await First{FallingEdge{signal}, Edge{signal}, Delay{1_ns}};
    if (winner == 2) {
        ++results.timeout_winners;
    } else {
        ++results.edge_winners;
    }
}

Task<void> cancellable_edge_wait(Signal signal) {
    co_await First{FallingEdge{signal}, Edge{signal}, Delay{100_ns}};
}

Task<void> wait_for_static_first(Signal clock, Signal observer,
                                 uint32_t& winner) {
    winner = static_cast<uint32_t>(
        co_await First{RisingEdge{clock}, FallingEdge{clock}, Edge{observer},
                       Delay{5_ns}});
}

Task<void> wait_for_dynamic_edge_first(Signal rising, Signal falling,
                                       Signal changed, uint32_t& winner,
                                       uint32_t& resumes) {
    winner = static_cast<uint32_t>(
        co_await First{RisingEdge{rising}, FallingEdge{falling}, Edge{changed}});
    ++resumes;
}

Task<void> cancellation_racer(Process* target) {
    co_await Delay{4_ns};
    target->cancel();
}

Task<void> delayed_increment(uint32_t& value, SimTime delay = 4_ns) {
    co_await Delay{delay};
    ++value;
}

#ifdef CPPTB_CORO_FRAME_POOL_DIAGNOSTICS
__attribute__((noinline)) uint32_t sum_large_frame(
    const std::array<uint8_t, 4'096>& bytes) {
    uint32_t sum = 0;
    for (const uint8_t value : bytes) sum += value;
    return sum;
}

Task<void> oversized_frame(uint32_t& checksum) {
    std::array<uint8_t, 4'096> bytes{};
    for (size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<uint8_t>(index);
    }
    co_await Delay{1_ns};
    checksum = sum_large_frame(bytes);
}
#endif

struct SpawnOnDestroy {
    Testbench* tb;
    uint32_t* spawned_runs;
    uint32_t* spawned_destructions;
    uint32_t* destructor_runs;

    ~SpawnOnDestroy() {
        ++*destructor_runs;
        tb->spawn_detached(
            immediate_root(*spawned_runs, *spawned_destructions));
    }
};

Task<void> spawn_from_frame_destructor(Testbench& tb, uint32_t& spawned_runs,
                                 uint32_t& spawned_destructions,
                                 uint32_t& destructor_runs) {
    SpawnOnDestroy callback{&tb, &spawned_runs, &spawned_destructions,
                            &destructor_runs};
    co_return;
}

struct CancelOnDestroy {
    Process target;
    uint32_t* destructor_runs;

    ~CancelOnDestroy() {
        ++*destructor_runs;
        target.cancel();
    }
};

Task<void> cancel_from_frame_destructor(Process target, uint32_t& destructor_runs) {
    CancelOnDestroy callback{std::move(target), &destructor_runs};
    co_return;
}

Task<void> suspend_with_shutdown_cancel(Process target, Signal signal,
                                  uint32_t& destructor_runs) {
    CancelOnDestroy callback{std::move(target), &destructor_runs};
    co_await RisingEdge{signal};
}

struct FrameToken {
    uint32_t* destructions;
    bool active = true;

    explicit FrameToken(uint32_t& value) : destructions(&value) {}
    FrameToken(const FrameToken&) = delete;
    FrameToken& operator=(const FrameToken&) = delete;
    FrameToken(FrameToken&& other) noexcept
        : destructions(other.destructions),
          active(std::exchange(other.active, false)) {}

    ~FrameToken() {
        if (active) ++*destructions;
    }
};

Task<void> rejected_shutdown_root(FrameToken, uint32_t& runs) {
    ++runs;
    co_return;
}

struct ShutdownSpawnOnDestroy {
    Testbench* tb;
    uint32_t* attempted;
    uint32_t* spawned_runs;
    uint32_t* spawned_destructions;

    ~ShutdownSpawnOnDestroy() {
        ++*attempted;
        tb->spawn_detached(rejected_shutdown_root(
            FrameToken{*spawned_destructions}, *spawned_runs));
    }
};

Task<void> suspend_with_shutdown_callback(Testbench& tb, Signal signal,
                                    uint32_t& attempted,
                                    uint32_t& spawned_runs,
                                    uint32_t& spawned_destructions) {
    ShutdownSpawnOnDestroy callback{&tb, &attempted, &spawned_runs,
                                    &spawned_destructions};
    co_await RisingEdge{signal};
}

Task<void> stress_worker(Signal signal, uint32_t& resumes) {
    for (uint32_t iteration = 0; iteration < 32; ++iteration) {
        co_await RisingEdge{signal};
        ++resumes;
    }
}

Task<void> invalid_process_wait() {
    Process process;
    co_await process;
}

Task<void> zero_delay_wait() {
    co_await Delay{0_ns};
}

Task<void> subprecision_delay_wait() {
    co_await Delay{1_fs};
}

Task<uint32_t> typed_primitive(SimTime delay = 1_ns) {
    co_await Delay{delay};
    co_return 42;
}

Task<std::unique_ptr<uint32_t>> typed_move_only() {
    co_return std::make_unique<uint32_t>(73);
}

Task<uint32_t> nested_typed_value(uint32_t& child_destructions) {
    DestructionCounter counter{&child_destructions};
    co_return co_await typed_primitive();
}

Task<void> collect_typed_results(uint32_t& primitive, uint32_t& move_only,
                                 uint32_t& nested,
                                 uint32_t& child_destructions,
                                 uint32_t& prompt_reclaims) {
    primitive = co_await typed_primitive();
    auto value = co_await typed_move_only();
    move_only = *value;
    nested = co_await nested_typed_value(child_destructions);
    if (child_destructions == 1) ++prompt_reclaims;
}

Task<uint32_t> cancellable_typed_value(uint32_t& resumes,
                                       uint32_t& destructions) {
    DestructionCounter counter{&destructions};
    co_await Delay{20_ns};
    ++resumes;
    co_return 99;
}

Task<void> await_cancellable_typed(uint32_t& resumes,
                                   uint32_t& destructions,
                                   uint32_t& result) {
    result = co_await cancellable_typed_value(resumes, destructions);
}

Task<uint32_t> throwing_typed_value() {
    throw 1;
    co_return 0;
}

Task<void> await_throwing_typed_value() {
    static_cast<void>(co_await throwing_typed_value());
}

Task<void> count_clock_cycles(Signal clock, uint64_t cycles,
                              uint32_t& completions) {
    co_await clock_cycles(clock, cycles);
    ++completions;
}

template <EdgeTrigger Trigger>
Task<void> collect_timeout(Trigger trigger, SimTime timeout, uint32_t& outcome,
                           uint32_t& resumes) {
    outcome = static_cast<uint32_t>(co_await with_timeout(trigger, timeout));
    ++resumes;
}

struct TimeoutProbe {
    uint32_t resumes = 0;
    uint32_t frame_destructions = 0;
};

#ifdef CPPTB_CORO_WAIT_PATH_DIAGNOSTICS
Task<uint32_t> timed_fast_path_falling(Signal signal, TimeoutProbe& probe) {
    DestructionCounter counter{&probe.frame_destructions};
    co_await FallingEdge{signal};
    ++probe.resumes;
    co_return 47;
}
#endif

struct NonDefaultMoveOnly {
    explicit NonDefaultMoveOnly(uint32_t value) : value(value) {}
    NonDefaultMoveOnly(const NonDefaultMoveOnly&) = delete;
    NonDefaultMoveOnly& operator=(const NonDefaultMoveOnly&) = delete;
    NonDefaultMoveOnly(NonDefaultMoveOnly&&) noexcept = default;
    NonDefaultMoveOnly& operator=(NonDefaultMoveOnly&&) noexcept = default;

    uint32_t value;
};

Task<uint32_t> timed_value(SimTime delay, uint32_t value,
                           TimeoutProbe& probe) {
    DestructionCounter counter{&probe.frame_destructions};
    co_await Delay{delay};
    ++probe.resumes;
    co_return value;
}

Task<void> timed_void(SimTime delay, TimeoutProbe& probe) {
    DestructionCounter counter{&probe.frame_destructions};
    co_await Delay{delay};
    ++probe.resumes;
}

Task<NonDefaultMoveOnly> timed_move_only(TimeoutProbe& probe) {
    DestructionCounter counter{&probe.frame_destructions};
    co_return NonDefaultMoveOnly{91};
}

Task<uint32_t> timed_nested_leaf(SimTime delay, TimeoutProbe& probe) {
    DestructionCounter counter{&probe.frame_destructions};
    co_await Delay{delay};
    ++probe.resumes;
    co_return 17;
}

Task<uint32_t> timed_nested_parent(SimTime delay, TimeoutProbe& parent,
                                   TimeoutProbe& leaf) {
    DestructionCounter counter{&parent.frame_destructions};
    co_return co_await timed_nested_leaf(delay, leaf);
}

Task<uint32_t> timed_event_value(Event& event, TimeoutProbe& probe) {
    DestructionCounter counter{&probe.frame_destructions};
    co_await event;
    ++probe.resumes;
    co_return 23;
}

Task<uint32_t> timed_channel_value(Channel<uint32_t>& channel,
                                   TimeoutProbe& probe) {
    DestructionCounter counter{&probe.frame_destructions};
    const uint32_t value = co_await channel.get();
    ++probe.resumes;
    co_return value;
}

Task<uint32_t> timed_process_value(Process process, TimeoutProbe& probe) {
    DestructionCounter counter{&probe.frame_destructions};
    co_await process;
    ++probe.resumes;
    co_return 29;
}

Task<void> collect_timed_value(Task<uint32_t> task, SimTime timeout,
                               uint32_t& completed, uint32_t& timed_out,
                               uint32_t& value, uint32_t& continuations) {
    auto result = co_await with_timeout(std::move(task), timeout);
    completed = result.completed() ? 1 : 0;
    timed_out = result.timed_out() ? 1 : 0;
    if (result) value = result.value();
    ++continuations;
}

Task<void> collect_timed_void(Task<void> task, SimTime timeout,
                              uint32_t& completed, uint32_t& timed_out,
                              uint32_t& continuations) {
    auto result = co_await with_timeout(std::move(task), timeout);
    completed = result.has_value() ? 1 : 0;
    timed_out = result.timed_out() ? 1 : 0;
    if (result) result.value();
    ++continuations;
}

Task<void> collect_timed_move_only(TimeoutProbe& probe, uint32_t& value,
                                   uint32_t& continuations) {
    auto result = co_await with_timeout(timed_move_only(probe), 1_ns);
    if (result) value = std::move(result).value().value;
    ++continuations;
}

Task<void> invalid_timed_task() {
    static_cast<void>(co_await with_timeout(Task<uint32_t>{}, 1_ns));
}

Task<void> invalid_timed_value_access() {
    TimeoutProbe probe;
    auto result = co_await with_timeout(timed_value(2_ns, 1, probe), 1_ns);
    static_cast<void>(result.value());
}

Task<void> zero_task_timeout() {
    TimeoutProbe probe;
    static_cast<void>(
        co_await with_timeout(timed_value(1_ns, 1, probe), 0_ns));
}

Task<void> subprecision_task_timeout() {
    TimeoutProbe probe;
    static_cast<void>(
        co_await with_timeout(timed_value(1_ps, 1, probe), 1_fs));
}

struct SignalValues {
    std::array<uint32_t, 4> values{};
};

uint32_t get_signal_value(void* context, uint32_t id) {
    return static_cast<SignalValues*>(context)->values[id];
}

Task<void> collect_wait_until(Signal signal, Signal clock, uint32_t target,
                              uint32_t& completions) {
    co_await wait_until(signal,
                        [target](uint32_t value) { return value >= target; },
                        clock);
    ++completions;
}

Task<void> wait_event(Event& event, uint32_t id,
                      std::vector<uint32_t>& order) {
    co_await event;
    order.push_back(id);
}

Task<void> wait_event_count(Event& event, uint32_t& count) {
    co_await event.wait();
    ++count;
}

Task<void> channel_get(Channel<uint32_t>& channel,
                       std::vector<uint32_t>& values) {
    values.push_back(co_await channel.get());
}

Task<void> channel_get_move_only(
    Channel<std::unique_ptr<uint32_t>>& channel, uint32_t& value) {
    auto item = co_await channel.get();
    value = *item;
}

Task<void> channel_put(Channel<uint32_t>& channel, uint32_t value,
                       SimTime delay = 1_ns) {
    co_await Delay{delay};
    co_await channel.put(value);
}

Task<void> put_then_cancel(Channel<uint32_t>& channel, uint32_t value,
                           Process target) {
    co_await channel.put(value);
    target.cancel();
}

struct ChannelMoveProbe {
    uint32_t value = 0;
    uint32_t completions = 0;
    bool source_inactive = false;
    bool destination_active = false;
    bool destination_inactive_after_resume = false;
};

Task<void> channel_get_moved_awaiter(Channel<uint32_t>& channel,
                                     ChannelMoveProbe& probe) {
    std::optional<Channel<uint32_t>::GetAwaiter> moved;
    {
        Channel<uint32_t>::GetAwaiter source{channel};
        moved.emplace(std::move(source));
        probe.source_inactive = !source.active;
        probe.destination_active = moved->active;
    }

    probe.value = co_await *moved;
    probe.destination_inactive_after_resume = !moved->active;
    ++probe.completions;
}

Task<void> channel_tagged_get(Channel<uint32_t>& channel, uint32_t id,
                              std::vector<uint32_t>& values,
                              std::vector<uint32_t>& order,
                              std::array<uint32_t, 3>& completions) {
    values.push_back(co_await channel.get());
    order.push_back(id);
    ++completions[id];
}

Task<void> channel_reentrant_get(Channel<uint32_t>& channel,
                                 Process* cancel_reserved,
                                 std::vector<uint32_t>& values,
                                 std::vector<uint32_t>& order,
                                 std::array<uint32_t, 3>& completions) {
    const uint32_t value = co_await channel.get();
    values.push_back(value);
    order.push_back(0);
    ++completions[0];

    channel.put_nowait(value + 1);
    cancel_reserved->cancel();
}

void trigger_event_cross_scheduler() {
    Event event;
    Testbench first;
    Testbench second;
    std::vector<uint32_t> order;
    first.spawn_detached(wait_event(event, 1, order));
    second.spawn_detached(wait_event(event, 2, order));
}

void trigger_event_destroyed_with_waiter() {
    Testbench tb;
    std::vector<uint32_t> order;
    Event event;
    tb.spawn_detached(wait_event(event, 1, order));
}

void trigger_channel_cross_scheduler() {
    Channel<uint32_t> channel;
    Testbench first;
    Testbench second;
    std::vector<uint32_t> values;
    first.spawn_detached(channel_get(channel, values));
    second.spawn_detached(channel_get(channel, values));
}

void trigger_channel_destroyed_with_waiter() {
    Testbench tb;
    std::vector<uint32_t> values;
    Channel<uint32_t> channel;
    tb.spawn_detached(channel_get(channel, values));
}

void trigger_typed_task_exception() {
    Testbench tb;
    tb.spawn_detached(await_throwing_typed_value());
}

void trigger_invalid_process_wait() {
    Testbench tb;
    tb.spawn_detached(invalid_process_wait());
}

void trigger_zero_delay_wait() {
    Testbench tb;
    tb.spawn_detached(zero_delay_wait());
}

void trigger_subprecision_delay_wait() {
    Testbench tb{1_ps};
    tb.spawn_detached(subprecision_delay_wait());
}

void trigger_late_static_edge_source_configuration() {
    Testbench tb;
    uint32_t value = 0;
    tb.spawn_detached(delayed_increment(value));
    tb.configure_static_edge_source(37);
}

void trigger_late_static_source_after_edge_wait() {
    Testbench tb;
    Results results;
    const Signal signal{nullptr, 38, "late_static_after_edge"};
    tb.spawn_detached(wait_for_change(signal, results));
    tb.configure_static_edge_source(signal.id);
}

void trigger_invalid_timed_task() {
    Testbench tb;
    tb.spawn_detached(invalid_timed_task());
}

void trigger_invalid_timed_value_access() {
    Testbench tb;
    tb.spawn_detached(invalid_timed_value_access());
    tb.set_time(1);
}

void trigger_zero_task_timeout() {
    Testbench tb;
    tb.spawn_detached(zero_task_timeout());
}

void trigger_subprecision_task_timeout() {
    Testbench tb{1_ps};
    tb.spawn_detached(subprecision_task_timeout());
}

void trigger_unpacked_array_oob() {
    ArrayTransport transport;
    const DrivenArray<32, 4, 7> array{
        3, "array_i", &transport, array_get, array_set, array_get_words,
        array_set_words};
    (void)array.at(3);
}

void trigger_multidimensional_array_oob() {
    ArrayTransport transport;
    const RankTwoDriven array{
        0, "matrix_i", &transport, array_get, array_set, array_get_words,
        array_set_words};
    (void)array.at(2).at(3);
}

void trigger_memory_probe_oob() {
    const cpptb::probe::MemoryProbe<73, 7, 4, true> memory{
        "memory", probe_get, probe_deposit};
    (void)memory.at(3);
}

void trigger_probe_outside_callback() {
    const cpptb::probe::Probe<73, false> value{
        4, "state", probe_get, nullptr};
    (void)value.get();
}

bool expect(const char* label, uint32_t actual, uint32_t expected) {
    if (actual == expected) return true;
    std::fprintf(stderr, "%s: got %u expected %u\n", label, actual, expected);
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
        stderr_output.append(buffer, static_cast<size_t>(count));
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

    std::fprintf(stderr,
                 "%s: status=%d, expected SIGABRT and message '%s'; stderr: %s\n",
                 label, status, expected_message, stderr_output.c_str());
    return false;
}

}  // namespace

int main() {
    static_assert(sizeof(CoroutineState) <= 32,
                  "hot coroutine state must remain compact");
    static_assert(sizeof(TaskPromise<void>) == sizeof(TaskPromiseBase),
                  "Task<void> must not carry typed result storage");
    static_assert(!std::default_initializable<NonDefaultMoveOnly>);
    static_assert(std::move_constructible<NonDefaultMoveOnly>);
    static_assert(1_fs == SimTime{1});
    static_assert(1_ps == SimTime{1'000});
    static_assert(1_ns == SimTime{1'000'000});
    static_assert(1_us == SimTime{1'000'000'000});
    static_assert(1_ms == SimTime{1'000'000'000'000});

    bool passed = true;
    {
        std::array<uint32_t, 8> inputs{};
        std::array<uint32_t, 8> outputs{};
        std::array<bool, 8> configured_clock{};
        std::array<bool, 8> local_edge_capable{};
        bool outputs_dirty = false;
        bool local_edge_delivery_enabled = false;
        cpptb::dpi::StaticBindingContext context{
            .inputs = inputs.data(),
            .outputs = outputs.data(),
            .current_inputs = nullptr,
            .configured_clock = configured_clock.data(),
            .local_edge_capable = local_edge_capable.data(),
            .outputs_dirty = &outputs_dirty,
            .local_edge_delivery_enabled = &local_edge_delivery_enabled,
        };

        const StaticPackedScalar packed{&context, "packed_1bit"};
        packed.set(2);
        passed &= expect("static packed masks zero-equivalent narrow write",
                         outputs.at(3), 0);
        passed &= expect("static packed masked no-op remains clean",
                         outputs_dirty ? 1 : 0, 0);
        packed.set(3);
        passed &= expect("static packed masks narrow write", outputs.at(3), 1);
        passed &= expect("static packed normalized change is dirty",
                         outputs_dirty ? 1 : 0, 1);

        static_on_demand_words.fill(0);
        outputs_dirty = false;
        const StaticOnDemandScalar on_demand{
            &context, "on_demand_1bit", static_on_demand_get,
            static_on_demand_set};
        on_demand.set(2);
        passed &= expect("static on-demand masks zero-equivalent narrow write",
                         static_on_demand_words.at(0), 0);
        passed &= expect("static on-demand direct write does not dirty outputs",
                         outputs_dirty ? 1 : 0, 0);
        on_demand.set(3);
        passed &= expect("static on-demand masks narrow write",
                         static_on_demand_words.at(0), 1);
        passed &= expect("static on-demand remains outside packed dirty state",
                         outputs_dirty ? 1 : 0, 0);
    }
    {
        narrow_probe_value = 0xffff'ffffu;
        const cpptb::probe::Probe<7, true> value{
            0, "narrow", narrow_probe_get, narrow_probe_deposit};
        cpptb::probe::detail::DpiCallbackScope callback_scope;
        passed &= expect("narrow probe masks get", value.get(), 0x7fu);
        value.deposit(0x1a5u);
        passed &= expect("narrow probe masks deposit", narrow_probe_value,
                         0x25u);
    }
    {
        narrow_force_value = 0;
        narrow_release_count = 0;
        const cpptb::probe::Probe<7, false, true> value{
            0, "forced", narrow_probe_get, nullptr, narrow_probe_force,
            narrow_probe_release};
        cpptb::probe::detail::DpiCallbackScope callback_scope;
        value.force(0x1a5u);
        passed &= expect("narrow probe masks force", narrow_force_value,
                         0x25u);
        value.release();
        passed &= expect("probe release callback", narrow_release_count, 1);
    }
    {
        static_assert(cpptb::probe::MemoryProbe<73, 7, 4, true>::left() == 7);
        static_assert(cpptb::probe::MemoryProbe<73, 7, 4, true>::right() == 4);
        static_assert(cpptb::probe::MemoryProbe<73, 7, 4, true>::size() == 4);
        probe_words.fill(0);
        const cpptb::probe::MemoryProbe<73, 7, 4, true> memory{
            "memory", probe_get, probe_deposit};
        cpptb::probe::detail::DpiCallbackScope callback_scope;
        const auto value = cpptb::Bits<73>::from_words(
            {0x89ab'cdefu, 0x0123'4567u, 0xffff'ffffu});
        memory.at(6).deposit(value);
        passed &= expect("memory probe low word", probe_words[6],
                         0x89ab'cdefu);
        passed &= expect("memory probe middle word", probe_words[7],
                         0x0123'4567u);
        passed &= expect("memory probe masks high word", probe_words[8],
                         0x0000'01ffu);
        passed &= expect("memory probe get low word",
                         memory.at(6).get().word(0), 0x89ab'cdefu);
    }
    {
        static_assert(DrivenArray<32, 4, 7>::left() == 4);
        static_assert(DrivenArray<32, 4, 7>::right() == 7);
        static_assert(DrivenArray<32, 4, 7>::size() == 4);
        static_assert(ObservedArray<64, 3, 0>::low() == 0);
        static_assert(ObservedArray<64, 3, 0>::high() == 3);

        ArrayTransport transport;
        const DrivenArray<32, 4, 7> narrow{
            3, "narrow_i", &transport, array_get, array_set, array_get_words,
            array_set_words};
        narrow.at(4).set(0x1234'5678u);
        narrow.at(7).set(0x89ab'cdefu);
        passed &= expect("ascending array low index id", transport.words[3],
                         0x1234'5678u);
        passed &= expect("ascending array high index id", transport.words[6],
                         0x89ab'cdefu);
        passed &= expect("array scalar get", narrow.at(7).get(),
                         0x89ab'cdefu);

        const DrivenArray<64, 3, 0> wide{
            8, "wide_i", &transport, array_get, array_set, array_get_words,
            array_set_words};
        wide.at(0).set(0x0123'4567'89ab'cdefull);
        wide.at(3).set(0xfedc'ba98'7654'3210ull);
        passed &= expect("descending array numeric low word", transport.words[8],
                         0x89ab'cdefu);
        passed &= expect("descending array numeric low high word",
                         transport.words[9], 0x0123'4567u);
        passed &= expect("descending array numeric high word",
                         transport.words[14], 0x7654'3210u);
        passed &= expect("descending array numeric high high word",
                         transport.words[15], 0xfedc'ba98u);
        passed &= expect("array packed get low",
                         static_cast<uint32_t>(wide.at(3).get()),
                         0x7654'3210u);

        const RankTwoDriven matrix = reshape_fixed_array(
            FixedArraySpec<65, true, ArrayDimension<2, 1>,
                           ArrayDimension<-1, 1>>{},
            UnpackedArray<288, 2, 1, true>{
                0, "matrix_i", &transport, array_get, array_set,
                array_get_words, array_set_words});
        matrix.at(1).at(-1).set(
            cpptb::Bits<65>::from_uint(0x0123'4567'89ab'cdefull));
        matrix.at(2).at(1).set(
            cpptb::Bits<65>::from_uint(0xfedc'ba98'7654'3210ull));
        passed &= expect("rank-2 first element low word", transport.words[0],
                         0x89ab'cdefu);
        passed &= expect("rank-2 outer stride", transport.words[15],
                         0x7654'3210u);
        passed &= expect("rank-2 inner index get",
                         matrix.at(2).at(1).get().word(0), 0x7654'3210u);

        const DrivenFixedArray<8, ArrayDimension<0, 1>,
                               ArrayDimension<4, 3>,
                               ArrayDimension<-2, -1>> cube{
            20, "cube_i", &transport, array_get, array_set, array_get_words,
            array_set_words};
        cube.at(1).at(4).at(-1).set(0x5au);
        passed &= expect("rank-3 row-major offset", transport.words[27],
                         0x5au);
    }
    {
        Testbench tb;
        uint32_t primitive = 0;
        uint32_t move_only = 0;
        uint32_t nested = 0;
        uint32_t child_destructions = 0;
        uint32_t prompt_reclaims = 0;
        tb.spawn_detached(collect_typed_results(
            primitive, move_only, nested, child_destructions,
            prompt_reclaims));
        tb.set_time(1);
        tb.set_time(2);
        passed &= expect("typed primitive result", primitive, 42);
        passed &= expect("typed move-only result", move_only, 73);
        passed &= expect("nested typed result", nested, 42);
        passed &= expect("nested child frame destroyed", child_destructions, 1);
        passed &= expect("typed child reclaimed before parent continues",
                         prompt_reclaims, 1);
        passed &= expect("typed results done", tb.done() ? 1 : 0, 1);
    }

    {
        Testbench tb;
        uint32_t resumes = 0;
        uint32_t destructions = 0;
        uint32_t result = 0;
        const auto process =
            tb.spawn(await_cancellable_typed(resumes, destructions, result));
        process.cancel();
        tb.set_time(20);
        passed &= expect("typed task cancellation status",
                         process.cancelled() ? 1 : 0, 1);
        passed &= expect("typed task cancellation prevents resume", resumes, 0);
        passed &= expect("typed task cancellation leaves result", result, 0);
        passed &= expect("typed task cancellation destroys child",
                         destructions, 1);
    }

    {
        Testbench tb;
        const Signal clock{nullptr, 30, "clock_cycles"};
        uint32_t zero = 0;
        uint32_t one = 0;
        uint32_t many = 0;
        tb.spawn_detached(count_clock_cycles(clock, 0, zero));
        tb.spawn_detached(count_clock_cycles(clock, 1, one));
        tb.spawn_detached(count_clock_cycles(clock, 3, many));
        passed &= expect("ClockCycles zero", zero, 1);
        tb.notify_edge(clock.id, EdgeKind::Rising);
        passed &= expect("ClockCycles one", one, 1);
        passed &= expect("ClockCycles many pending", many, 0);
        tb.notify_edge(clock.id, EdgeKind::Rising);
        tb.notify_edge(clock.id, EdgeKind::Rising);
        passed &= expect("ClockCycles many", many, 1);
    }

    {
        Testbench tb;
        const Signal winner_signal{nullptr, 31, "timeout_winner"};
        const Signal timeout_signal{nullptr, 32, "timeout_loser"};
        uint32_t winner = 99;
        uint32_t timed_out = 99;
        uint32_t winner_resumes = 0;
        uint32_t timeout_resumes = 0;
        tb.spawn_detached(collect_timeout(FallingEdge{winner_signal}, 5_ns,
                                          winner, winner_resumes));
        tb.spawn_detached(collect_timeout(FallingEdge{timeout_signal}, 3_ns,
                                          timed_out, timeout_resumes));
        tb.notify_edge(winner_signal.id, EdgeKind::Falling);
        tb.set_time(3);
        passed &= expect("with_timeout FallingEdge trigger winner", winner,
                         static_cast<uint32_t>(TimeoutOutcome::Triggered));
        passed &= expect("with_timeout FallingEdge timeout winner", timed_out,
                         static_cast<uint32_t>(TimeoutOutcome::TimedOut));
        tb.notify_edge(timeout_signal.id, EdgeKind::Falling);
        tb.set_time(5);
        passed &= expect("with_timeout stale timer never resumes",
                         winner_resumes, 1);
        passed &= expect("with_timeout stale FallingEdge never resumes",
                         timeout_resumes, 1);
        passed &= expect("with_timeout FallingEdge interest cleaned",
                         tb.has_falling_edge_waiters() ? 1 : 0, 0);
        passed &= expect("with_timeout FallingEdge races done",
                         tb.done() ? 1 : 0, 1);
    }

    {
        const Signal signal{nullptr, 33, "edge_then_time_tie"};
        Testbench tb;
        uint32_t outcome = 99;
        uint32_t completions = 0;
        tb.spawn_detached(
            collect_timeout(FallingEdge{signal}, 5_ns, outcome, completions));

        tb.notify_edge(signal.id, EdgeKind::Falling);
        tb.set_time(5);
        passed &= expect("with_timeout tie edge-then-time chooses trigger",
                         outcome,
                         static_cast<uint32_t>(TimeoutOutcome::Triggered));
        passed &= expect("with_timeout tie edge-then-time completes once",
                         completions, 1);
        passed &= expect("with_timeout tie edge-then-time has no stale wait",
                         tb.has_falling_edge_waiters() ? 1 : 0, 0);
        passed &= expect("with_timeout tie edge-then-time done",
                         tb.done() ? 1 : 0, 1);
    }

    {
        const Signal signal{nullptr, 34, "time_then_edge_tie"};
        Testbench tb;
        uint32_t outcome = 99;
        uint32_t completions = 0;
        tb.spawn_detached(
            collect_timeout(FallingEdge{signal}, 5_ns, outcome, completions));

        tb.set_time(5);
        tb.notify_edge(signal.id, EdgeKind::Falling);
        passed &= expect("with_timeout tie time-then-edge chooses timeout",
                         outcome,
                         static_cast<uint32_t>(TimeoutOutcome::TimedOut));
        passed &= expect("with_timeout tie time-then-edge completes once",
                         completions, 1);
        passed &= expect("with_timeout tie time-then-edge has no stale wait",
                         tb.has_falling_edge_waiters() ? 1 : 0, 0);
        passed &= expect("with_timeout tie time-then-edge done",
                         tb.done() ? 1 : 0, 1);
    }

    {
        Testbench tb;
        TimeoutProbe value_probe;
        TimeoutProbe void_probe;
        TimeoutProbe move_probe;
        uint32_t value_completed = 0;
        uint32_t value_timed_out = 0;
        uint32_t value = 0;
        uint32_t value_continuations = 0;
        uint32_t void_completed = 0;
        uint32_t void_timed_out = 0;
        uint32_t void_continuations = 0;
        uint32_t move_value = 0;
        uint32_t move_continuations = 0;

        tb.spawn_detached(collect_timed_value(
            timed_value(2_ns, 42, value_probe), 5_ns, value_completed,
            value_timed_out, value, value_continuations));
        tb.spawn_detached(collect_timed_void(
            timed_void(1_ns, void_probe), 5_ns, void_completed,
            void_timed_out, void_continuations));
        tb.spawn_detached(collect_timed_move_only(
            move_probe, move_value, move_continuations));
        tb.set_time(1);
        tb.set_time(2);
        tb.set_time(5);

        passed &= expect("Task<T> timeout completion state", value_completed,
                         1);
        passed &= expect("Task<T> timeout not timed out", value_timed_out, 0);
        passed &= expect("Task<T> timeout value", value, 42);
        passed &= expect("Task<T> timeout continuation exactly once",
                         value_continuations, 1);
        passed &= expect("Task<T> timeout child frame exactly once",
                         value_probe.frame_destructions, 1);
        passed &= expect("Task<void> timeout completion state", void_completed,
                         1);
        passed &= expect("Task<void> timeout not timed out", void_timed_out, 0);
        passed &= expect("Task<void> timeout continuation exactly once",
                         void_continuations, 1);
        passed &= expect("Task<void> timeout frame exactly once",
                         void_probe.frame_destructions, 1);
        passed &= expect("Task timeout move-only non-default value", move_value,
                         91);
        passed &= expect("Task timeout move-only continuation exactly once",
                         move_continuations, 1);
        passed &= expect("Task timeout move-only frame exactly once",
                         move_probe.frame_destructions, 1);
        passed &= expect("completed Task timeout stale deadlines inactive",
                         tb.done() ? 1 : 0, 1);
    }

    {
        Testbench tb;
        TimeoutProbe probe;
        uint32_t completed = 0;
        uint32_t timed_out = 0;
        uint32_t value = 0;
        uint32_t continuations = 0;
        tb.spawn_detached(collect_timed_value(
            timed_value(5_ns, 55, probe), 5_ns, completed, timed_out, value,
            continuations));
        tb.set_time(5);
        tb.set_time(10);

        passed &= expect("Task timeout same-deadline task wins", completed, 1);
        passed &= expect("Task timeout same-deadline carries value", value, 55);
        passed &= expect("Task timeout same-deadline not timed out", timed_out,
                         0);
        passed &= expect("Task timeout same-deadline resumes exactly once",
                         continuations, 1);
        passed &= expect("Task timeout same-deadline frame exactly once",
                         probe.frame_destructions, 1);
        passed &= expect("Task timeout same-deadline stale timer inactive",
                         tb.done() ? 1 : 0, 1);
    }

    {
        Testbench tb;
        TimeoutProbe probe;
        TimeoutProbe void_probe;
        uint32_t completed = 0;
        uint32_t timed_out = 0;
        uint32_t value = 7;
        uint32_t continuations = 0;
        uint32_t void_completed = 1;
        uint32_t void_timed_out = 0;
        uint32_t void_continuations = 0;
        tb.spawn_detached(collect_timed_value(
            timed_value(10_ns, 99, probe), 3_ns, completed, timed_out, value,
            continuations));
        tb.spawn_detached(collect_timed_void(
            timed_void(10_ns, void_probe), 3_ns, void_completed,
            void_timed_out, void_continuations));
        tb.set_time(3);
        tb.set_time(10);

        passed &= expect("Task<T> timeout loser has no value", completed, 0);
        passed &= expect("Task<T> timeout state", timed_out, 1);
        passed &= expect("Task<T> timeout leaves destination unchanged", value,
                         7);
        passed &= expect("Task<T> timeout parent resumes exactly once",
                         continuations, 1);
        passed &= expect("Task<T> timeout suppresses loser resume",
                         probe.resumes, 0);
        passed &= expect("Task<T> timeout destroys loser frame exactly once",
                         probe.frame_destructions, 1);
        passed &= expect("Task<T> timeout stale child deadline inactive",
                         tb.done() ? 1 : 0, 1);
        passed &= expect("Task<void> timeout has no completion value",
                         void_completed, 0);
        passed &= expect("Task<void> timeout state", void_timed_out, 1);
        passed &= expect("Task<void> timeout parent resumes exactly once",
                         void_continuations, 1);
        passed &= expect("Task<void> timeout suppresses loser resume",
                         void_probe.resumes, 0);
        passed &= expect("Task<void> timeout destroys loser frame once",
                         void_probe.frame_destructions, 1);
    }

    {
        Testbench tb;
        TimeoutProbe parent_probe;
        TimeoutProbe leaf_probe;
        uint32_t completed = 0;
        uint32_t timed_out = 0;
        uint32_t value = 0;
        uint32_t continuations = 0;
        tb.spawn_detached(collect_timed_value(
            timed_nested_parent(20_ns, parent_probe, leaf_probe), 2_ns,
            completed, timed_out, value, continuations));
        tb.set_time(2);
        tb.set_time(20);

        passed &= expect("nested Task timeout reports timeout", timed_out, 1);
        passed &= expect("nested Task timeout parent destroyed once",
                         parent_probe.frame_destructions, 1);
        passed &= expect("nested Task timeout leaf destroyed once",
                         leaf_probe.frame_destructions, 1);
        passed &= expect("nested Task timeout leaf never resumes",
                         leaf_probe.resumes, 0);
        passed &= expect("nested Task timeout continuation exactly once",
                         continuations, 1);
    }

    {
        Testbench tb;
        Event event;
        TimeoutProbe event_probe;
        uint32_t completed = 0;
        uint32_t timed_out = 0;
        uint32_t value = 0;
        uint32_t continuations = 0;
        tb.spawn_detached(collect_timed_value(
            timed_event_value(event, event_probe), 2_ns, completed, timed_out,
            value, continuations));
        tb.set_time(2);
        event.set();

        passed &= expect("Event task timeout reports timeout", timed_out, 1);
        passed &= expect("Event task timeout removes waiter",
                         event_probe.resumes, 0);
        passed &= expect("Event task timeout destroys frame once",
                         event_probe.frame_destructions, 1);
        passed &= expect("Event task timeout resumes parent once",
                         continuations, 1);
    }

    {
        Testbench tb;
        Channel<uint32_t> channel;
        TimeoutProbe channel_probe;
        uint32_t completed = 0;
        uint32_t timed_out = 0;
        uint32_t value = 0;
        uint32_t continuations = 0;
        std::vector<uint32_t> survivor_values;
        tb.spawn_detached(collect_timed_value(
            timed_channel_value(channel, channel_probe), 2_ns, completed,
            timed_out, value, continuations));
        tb.set_time(2);
        channel.put_nowait(44);
        tb.spawn_detached(channel_get(channel, survivor_values));

        passed &= expect("Channel task timeout reports timeout", timed_out, 1);
        passed &= expect("Channel task timeout removes consumer",
                         channel_probe.resumes, 0);
        passed &= expect("Channel task timeout destroys frame once",
                         channel_probe.frame_destructions, 1);
        passed &= expect("Channel task timeout survivor receives one item",
                         static_cast<uint32_t>(survivor_values.size()), 1);
        passed &= expect("Channel task timeout preserves queued item",
                         survivor_values.empty() ? 0 : survivor_values[0], 44);
        passed &= expect("Channel task timeout drains cleanly",
                         channel.empty() ? 1 : 0, 1);
    }

    {
        Testbench tb;
        uint32_t target_resumes = 0;
        TimeoutProbe waiter_probe;
        uint32_t completed = 0;
        uint32_t timed_out = 0;
        uint32_t value = 0;
        uint32_t continuations = 0;
        const auto target = tb.spawn(delayed_increment(target_resumes, 10_ns));
        tb.spawn_detached(collect_timed_value(
            timed_process_value(target, waiter_probe), 2_ns, completed,
            timed_out, value, continuations));
        tb.set_time(2);
        tb.set_time(10);

        passed &= expect("Process wait Task timeout reports timeout",
                         timed_out, 1);
        passed &= expect("Process wait target completes normally",
                         target.done() && !target.cancelled() ? 1 : 0, 1);
        passed &= expect("Process wait timeout removes stale waiter",
                         waiter_probe.resumes, 0);
        passed &= expect("Process wait timeout destroys frame once",
                         waiter_probe.frame_destructions, 1);
        passed &= expect("Process wait timeout resumes parent once",
                         continuations, 1);
    }

    {
        Testbench tb;
        TimeoutProbe probe;
        uint32_t completed = 0;
        uint32_t timed_out = 0;
        uint32_t value = 0;
        uint32_t continuations = 0;
        const auto process = tb.spawn(collect_timed_value(
            timed_value(20_ns, 99, probe), 50_ns, completed, timed_out, value,
            continuations));
        process.cancel();
        tb.set_time(50);

        passed &= expect("Process cancellation during Task timeout done",
                         process.done() ? 1 : 0, 1);
        passed &= expect("Process cancellation during Task timeout status",
                         process.cancelled() ? 1 : 0, 1);
        passed &= expect("Process cancellation suppresses timeout continuation",
                         continuations, 0);
        passed &= expect("Process cancellation destroys timed child once",
                         probe.frame_destructions, 1);
        passed &= expect("Process cancellation suppresses timed child resume",
                         probe.resumes, 0);
    }

    {
        Event event;
        TimeoutProbe probe;
        Process retained;
        uint32_t completed = 0;
        uint32_t timed_out = 0;
        uint32_t value = 0;
        uint32_t continuations = 0;
        {
            Testbench tb;
            retained = tb.spawn(collect_timed_value(
                timed_event_value(event, probe), 50_ns, completed, timed_out,
                value, continuations));
        }
        event.set();

        passed &= expect("scheduler shutdown cancels Task timeout process",
                         retained.cancelled() ? 1 : 0, 1);
        passed &= expect("scheduler shutdown suppresses timeout continuation",
                         continuations, 0);
        passed &= expect("scheduler shutdown destroys timed child once",
                         probe.frame_destructions, 1);
        passed &= expect("scheduler shutdown cleans Event timed waiter",
                         probe.resumes, 0);
    }

    {
        Testbench tb;
        SignalValues values;
        const Signal signal{nullptr, 1, "predicate", &values,
                            get_signal_value, nullptr};
        const Signal clock{nullptr, 2, "predicate_clock"};
        uint32_t immediate = 0;
        uint32_t delayed = 0;
        values.values[1] = 7;
        tb.spawn_detached(collect_wait_until(signal, clock, 7, immediate));
        passed &= expect("wait_until immediate check", immediate, 1);
        values.values[1] = 0;
        tb.spawn_detached(collect_wait_until(signal, clock, 3, delayed));
        values.values[1] = 2;
        tb.notify_edge(clock.id, EdgeKind::Rising);
        passed &= expect("wait_until delayed pending", delayed, 0);
        values.values[1] = 3;
        tb.notify_edge(clock.id, EdgeKind::Rising);
        passed &= expect("wait_until delayed completion", delayed, 1);
    }

    {
        Testbench tb;
        Event event;
        std::vector<uint32_t> order;
        event.set();
        tb.spawn_detached(wait_event(event, 0, order));
        passed &= expect("Event set-before-wait", order.size(), 1);
        event.clear();
        const auto cancelled = tb.spawn(wait_event(event, 99, order));
        tb.spawn_detached(wait_event(event, 1, order));
        tb.spawn_detached(wait_event(event, 2, order));
        tb.spawn_detached(wait_event(event, 3, order));
        cancelled.cancel();
        event.set();
        passed &= expect("Event multiwaiter count", order.size(), 4);
        passed &= expect("Event FIFO first", order[1], 1);
        passed &= expect("Event FIFO second", order[2], 2);
        passed &= expect("Event FIFO third", order[3], 3);
        event.clear();
        tb.spawn_detached(wait_event(event, 4, order));
        event.set();
        passed &= expect("Event cancellation and reuse", order.back(), 4);
        passed &= expect("Event cancelled waiter stayed stale",
                         static_cast<uint32_t>(order.size()), 5);
    }

    {
        Event event;
        uint32_t count = 0;
        Process abandoned;
        {
            Testbench tb;
            abandoned = tb.spawn(wait_event_count(event, count));
        }
        passed &= expect("Event scheduler destruction cancellation",
                         abandoned.cancelled() ? 1 : 0, 1);
        event.set();
        {
            Testbench tb;
            tb.spawn_detached(wait_event_count(event, count));
        }
        passed &= expect("Event reuse after scheduler destruction", count, 1);
    }

    {
        Testbench tb;
        Channel<uint32_t> channel;
        std::vector<uint32_t> values;
        channel.put_nowait(1);
        channel.put_nowait(2);
        tb.spawn_detached(channel_get(channel, values));
        tb.spawn_detached(channel_get(channel, values));
        passed &= expect("Channel queued-before-get count", values.size(), 2);
        passed &= expect("Channel FIFO first", values[0], 1);
        passed &= expect("Channel FIFO second", values[1], 2);
        tb.spawn_detached(channel_get(channel, values));
        channel.put_nowait(3);
        passed &= expect("Channel waiting consumer", values.back(), 3);
        passed &= expect("Channel empty after gets", channel.empty() ? 1 : 0,
                         1);
    }

    {
        Testbench tb;
        Channel<std::unique_ptr<uint32_t>> channel;
        uint32_t value = 0;
        channel.put_nowait(std::make_unique<uint32_t>(91));
        tb.spawn_detached(channel_get_move_only(channel, value));
        passed &= expect("Channel move-only value", value, 91);
    }

    {
        Testbench tb;
        Channel<uint32_t> channel;
        ChannelMoveProbe probe;
        tb.spawn_detached(channel_get_moved_awaiter(channel, probe));
        passed &= expect("Channel moved GetAwaiter source inactive",
                         probe.source_inactive ? 1 : 0, 1);
        passed &= expect("Channel moved GetAwaiter destination active",
                         probe.destination_active ? 1 : 0, 1);
        passed &= expect("Channel moved GetAwaiter remains parked",
                         probe.completions, 0);

        channel.put_nowait(92);
        passed &= expect("Channel moved GetAwaiter receives reserved item",
                         probe.value, 92);
        passed &= expect("Channel moved GetAwaiter completes once",
                         probe.completions, 1);
        passed &= expect("Channel moved GetAwaiter deactivates on resume",
                         probe.destination_inactive_after_resume ? 1 : 0, 1);
        passed &= expect("Channel moved-from destructor consumes no item",
                         channel.empty() ? 1 : 0, 1);
    }

    {
        Testbench tb;
        Channel<uint32_t> channel;
        std::vector<uint32_t> values;
        tb.spawn_detached(channel_get(channel, values));
        tb.spawn_detached(channel_get(channel, values));
        tb.spawn_detached(channel_get(channel, values));
        tb.spawn_detached(channel_put(channel, 10));
        tb.spawn_detached(channel_put(channel, 20));
        tb.spawn_detached(channel_put(channel, 30));
        tb.set_time(1);
        passed &= expect("Channel multi producer/consumer count", values.size(),
                         3);
        passed &= expect("Channel multi producer FIFO first", values[0], 10);
        passed &= expect("Channel multi producer FIFO second", values[1], 20);
        passed &= expect("Channel multi producer FIFO third", values[2], 30);
    }

    {
        Testbench tb;
        Channel<uint32_t> channel;
        std::vector<uint32_t> cancelled_values;
        std::vector<uint32_t> surviving_values;
        const auto cancelled = tb.spawn(channel_get(channel, cancelled_values));
        tb.spawn_detached(channel_get(channel, surviving_values));
        tb.spawn_detached(put_then_cancel(channel, 55, cancelled));
        passed &= expect("Channel cancelled consumer gets no item",
                         cancelled_values.size(), 0);
        passed &= expect("Channel cancellation preserves item",
                         surviving_values.size(), 1);
        passed &= expect("Channel cancellation preserves value",
                         surviving_values[0], 55);
        passed &= expect("Channel cancellation status",
                         cancelled.cancelled() ? 1 : 0, 1);
    }

    {
        Testbench tb;
        Channel<uint32_t> channel;
        std::vector<uint32_t> values;
        std::vector<uint32_t> order;
        std::array<uint32_t, 3> completions{};
        Process cancel_reserved;

        tb.spawn_detached(channel_reentrant_get(
            channel, &cancel_reserved, values, order, completions));
        cancel_reserved = tb.spawn(
            channel_tagged_get(channel, 1, values, order, completions));
        tb.spawn_detached(
            channel_tagged_get(channel, 2, values, order, completions));

        channel.put_nowait(70);
        passed &= expect("Channel reentrant delivery count", values.size(), 2);
        passed &= expect("Channel reentrant first value", values[0], 70);
        passed &= expect("Channel reentrant handed-off value", values[1], 71);
        passed &= expect("Channel reentrant first consumer order", order[0], 0);
        passed &= expect("Channel reentrant survivor order", order[1], 2);
        passed &= expect("Channel reentrant producer-consumer completes once",
                         completions[0], 1);
        passed &= expect("Channel reentrant cancelled consumer never resumes",
                         completions[1], 0);
        passed &= expect("Channel reentrant survivor completes once",
                         completions[2], 1);
        passed &= expect("Channel reentrant reserved consumer cancelled",
                         cancel_reserved.cancelled() ? 1 : 0, 1);
        passed &= expect("Channel reentrant handoff loses no item",
                         channel.empty() ? 1 : 0, 1);
        passed &= expect("Channel reentrant wake queue drains once",
                         tb.done() ? 1 : 0, 1);
    }

    {
        Channel<uint32_t> channel;
        std::vector<uint32_t> values;
        Process abandoned;
        {
            Testbench tb;
            abandoned = tb.spawn(channel_get(channel, values));
        }
        passed &= expect("Channel scheduler destruction cancellation",
                         abandoned.cancelled() ? 1 : 0, 1);
        channel.put_nowait(88);
        {
            Testbench tb;
            tb.spawn_detached(channel_get(channel, values));
        }
        passed &= expect("Channel reuse after scheduler destruction",
                         values.back(), 88);
    }

    {
        Testbench tb;
        Results results;
        const Signal first{nullptr, 1, "first"};
        const Signal second{nullptr, 2, "second"};

        tb.spawn_detached(wait_for_edges(first, results));
        tb.spawn_detached(wait_for_change(first, results));
        tb.spawn_detached(wait_for_delay(tb, results));
        tb.spawn_detached(wait_for_first(second, results));
        tb.spawn_detached(wait_for_join(first, second, results));

        passed &= expect("falling edge interest after spawn",
                         tb.has_falling_edge_waiters() ? 1 : 0, 1);
        if (!tb.consume_timer_schedule_changed()) return 1;
        if (!expect("first timer deadline", tb.next_timer_deadline(), 7)) return 1;

        tb.notify_edge(first.id, EdgeKind::Rising);
        passed &= expect("falling edge interest after rising",
                         tb.has_falling_edge_waiters() ? 1 : 0, 1);
        tb.notify_edge(first.id, EdgeKind::Falling);
        passed &= expect("falling edge interest after falling",
                         tb.has_falling_edge_waiters() ? 1 : 0, 0);
        tb.notify_edge(second.id, EdgeKind::Rising);
        tb.set_time(7);

        passed &= expect("rising", results.rising, 1);
        passed &= expect("falling", results.falling, 1);
        passed &= expect("changed", results.changed, 1);
        passed &= expect("delay", results.delayed, 1);
        passed &= expect("first winner", results.first_winner, 0);
        passed &= expect("join", results.joined, 12);
        passed &= expect("done", tb.done() ? 1 : 0, 1);
    }

#ifdef CPPTB_CORO_WAIT_PATH_DIAGNOSTICS
    {
        Testbench tb;
        const Signal rearm_signal{nullptr, 40, "fast_rearm"};
        const Signal any_signal{nullptr, 41, "fast_any"};
        const Signal cancelled_signal{nullptr, 42, "fast_cancel"};
        const Signal compound_signal{nullptr, 43, "generic_compound"};
        uint32_t rearm_stage = 0;
        uint32_t any_wakes = 0;
        uint32_t cancelled_wakes = 0;
        uint32_t first_winner = 99;
        TimeoutOutcome timeout_outcome = TimeoutOutcome::Triggered;

        const auto rearmed = tb.spawn(fast_path_rearm(rearm_signal,
                                                     rearm_stage));
        const auto any = tb.spawn(fast_path_any(any_signal, any_wakes));
        const auto cancelled = tb.spawn(
            fast_path_cancelled_falling(cancelled_signal, cancelled_wakes));

        passed &= expect("RisingEdge selects single-edge fast path",
                         tb.single_edge_park_count(EdgeKind::Rising), 1);
        passed &= expect("FallingEdge selects single-edge fast path",
                         tb.single_edge_park_count(EdgeKind::Falling), 1);
        passed &= expect("Edge selects single-edge fast path",
                         tb.single_edge_park_count(EdgeKind::Any), 1);
        passed &= expect("single-edge waits avoid compound path",
                         tb.compound_wait_park_count(), 0);
        passed &= expect("FallingEdge fast path updates accounting",
                         tb.has_falling_edge_waiters() ? 1 : 0, 1);

        cancelled.cancel();
        passed &= expect("fast-path cancellation completes process",
                         cancelled.cancelled() ? 1 : 0, 1);
        passed &= expect("fast-path cancellation prevents wake",
                         cancelled_wakes, 0);
        passed &= expect("cancelled fast FallingEdge clears accounting",
                         tb.has_edge_interest(cancelled_signal.id,
                                              EdgeKind::Falling),
                         0);
        tb.notify_edge(cancelled_signal.id, EdgeKind::Falling);
        passed &= expect("cancelled fast FallingEdge stale re-notify skipped",
                         cancelled_wakes, 0);
        passed &= expect("cancelled fast FallingEdge remains cancelled",
                         cancelled.cancelled() ? 1 : 0, 1);

        tb.notify_edge(any_signal.id, EdgeKind::Rising);
        passed &= expect("Any fast path wakes", any_wakes, 1);
        passed &= expect("Any fast path completes coroutine",
                         any.done() ? 1 : 0, 1);

        tb.notify_edge(rearm_signal.id, EdgeKind::Rising);
        passed &= expect("fast path wakes before rearm", rearm_stage, 1);
        passed &= expect("rearmed FallingEdge uses fast path",
                         tb.single_edge_park_count(EdgeKind::Falling), 2);
        passed &= expect("rearmed fast path updates falling accounting",
                         tb.has_falling_edge_waiters() ? 1 : 0, 1);
        tb.notify_edge(rearm_signal.id, EdgeKind::Falling);
        passed &= expect("rearmed fast path wakes again", rearm_stage, 2);
        passed &= expect("rearmed fast path completes coroutine",
                         rearmed.done() ? 1 : 0, 1);
        passed &= expect("completed fast path clears falling accounting",
                         tb.has_falling_edge_waiters() ? 1 : 0, 0);

        const auto first =
            tb.spawn(generic_first_path(compound_signal, first_winner));
        passed &= expect("First remains on generic compound path",
                         tb.compound_wait_park_count(), 1);
        passed &= expect("First edge does not use single-edge fast path",
                         tb.single_edge_park_count(EdgeKind::Rising), 1);
        tb.notify_edge(compound_signal.id, EdgeKind::Rising);
        passed &= expect("generic First still wakes", first_winner, 0);
        passed &= expect("generic First completes", first.done() ? 1 : 0, 1);

        const auto timed =
            tb.spawn(generic_timeout_path(compound_signal, timeout_outcome));
        passed &= expect("edge timeout remains on generic compound path",
                         tb.compound_wait_park_count(), 2);
        passed &= expect("edge timeout avoids single-edge fast path",
                         tb.single_edge_park_count(EdgeKind::Rising), 1);
        tb.set_time(2);
        passed &= expect("generic edge timeout still times out",
                         timeout_outcome == TimeoutOutcome::TimedOut ? 1 : 0,
                         1);
        passed &= expect("generic edge timeout completes",
                         timed.done() ? 1 : 0, 1);
        passed &= expect("fast-path diagnostics test drains scheduler",
                         tb.done() ? 1 : 0, 1);
    }

    {
        Testbench tb;
        const Signal rising_signal{nullptr, 44, "first_rising_loser"};
        const Signal falling_signal{nullptr, 45, "first_falling_winner"};
        const Signal any_signal{nullptr, 46, "first_any_loser"};
        uint32_t winner = 99;
        uint32_t completions = 0;

        const auto first = tb.spawn(generic_first_three_edges(
            rising_signal, falling_signal, any_signal, winner, completions));
        passed &= expect("three-edge First uses compound path",
                         tb.compound_wait_park_count(), 1);
        passed &= expect("three-edge First registers rising loser",
                         tb.has_edge_interest(rising_signal.id,
                                              EdgeKind::Rising),
                         1);
        passed &= expect("three-edge First registers falling winner",
                         tb.has_edge_interest(falling_signal.id,
                                              EdgeKind::Falling),
                         1);
        passed &= expect("three-edge First registers any-edge loser",
                         tb.has_edge_interest(any_signal.id, EdgeKind::Any), 1);
        passed &= expect("three-edge First adds falling accounting",
                         tb.has_falling_edge_waiters() ? 1 : 0, 1);

        tb.notify_edge(falling_signal.id, EdgeKind::Falling);
        passed &= expect("three-edge First selects falling winner", winner, 1);
        passed &= expect("three-edge First resumes exactly once", completions,
                         1);
        passed &= expect("three-edge First completes", first.done() ? 1 : 0,
                         1);
        passed &= expect("three-edge First cleans rising loser",
                         tb.has_edge_interest(rising_signal.id,
                                              EdgeKind::Rising),
                         0);
        passed &= expect("three-edge First cleans falling registration",
                         tb.has_edge_interest(falling_signal.id,
                                              EdgeKind::Falling),
                         0);
        passed &= expect("three-edge First cleans any-edge loser",
                         tb.has_edge_interest(any_signal.id, EdgeKind::Any), 0);
        passed &= expect("three-edge First restores falling accounting",
                         tb.has_falling_edge_waiters() ? 1 : 0, 0);

        tb.notify_edge(rising_signal.id, EdgeKind::Rising);
        tb.notify_edge(any_signal.id, EdgeKind::Falling);
        tb.notify_edge(falling_signal.id, EdgeKind::Falling);
        passed &= expect("three-edge First stale re-notifies are skipped",
                         completions, 1);
        passed &= expect("three-edge First re-notify keeps winner", winner, 1);
        passed &= expect("three-edge First re-notify keeps falling accounting",
                         tb.has_falling_edge_waiters() ? 1 : 0, 0);
        passed &= expect("three-edge First drains scheduler",
                         tb.done() ? 1 : 0, 1);
    }

    {
        Testbench tb;
        const Signal signal{nullptr, 47, "timed_fast_falling"};
        TimeoutProbe probe;
        uint32_t completed = 0;
        uint32_t timed_out = 0;
        uint32_t value = 0;
        uint32_t continuations = 0;

        tb.spawn_detached(collect_timed_value(
            timed_fast_path_falling(signal, probe), 2_ns, completed, timed_out,
            value, continuations));
        passed &= expect("timed child selects single-edge fast path",
                         tb.single_edge_park_count(EdgeKind::Falling), 1);
        passed &= expect("timed fast child registers falling interest",
                         tb.has_edge_interest(signal.id, EdgeKind::Falling), 1);
        passed &= expect("timed fast child updates falling accounting",
                         tb.has_falling_edge_waiters() ? 1 : 0, 1);

        tb.set_time(2);
        passed &= expect("timed fast child reports timeout", timed_out, 1);
        passed &= expect("timed fast child has no value", completed, 0);
        passed &= expect("timed fast child parent resumes exactly once",
                         continuations, 1);
        passed &= expect("timed fast child cancel_awaited prevents resume",
                         probe.resumes, 0);
        passed &= expect("timed fast child cancel_state destroys frame",
                         probe.frame_destructions, 1);
        passed &= expect("timed fast child cancel_state clears interest",
                         tb.has_edge_interest(signal.id, EdgeKind::Falling), 0);
        passed &= expect("timed fast child restores falling accounting",
                         tb.has_falling_edge_waiters() ? 1 : 0, 0);

        tb.notify_edge(signal.id, EdgeKind::Falling);
        passed &= expect("timed fast child stale edge never wakes",
                         probe.resumes, 0);
        passed &= expect("timed fast child continuation stays exactly once",
                         continuations, 1);
        passed &= expect("timed fast child frame stays destroyed once",
                         probe.frame_destructions, 1);
        passed &= expect("timed fast child drains scheduler",
                         tb.done() ? 1 : 0, 1);
    }
#endif

    {
        Testbench tb{1_fs};
        Results results;
        tb.spawn_detached(wait_for_precise_delays(tb, results));
        passed &= expect("picosecond deadline",
                         static_cast<uint32_t>(tb.next_timer_deadline()), 1'000);
        tb.set_time(1'000);
        passed &= expect("femtosecond deadline",
                         static_cast<uint32_t>(tb.next_timer_deadline()), 1'999);
        tb.set_time(1'999);
        passed &= expect("precise delays", results.precise_delays, 2);
        passed &= expect("precise delay done", tb.done() ? 1 : 0, 1);
    }

    {
        Testbench tb;
        const Signal clock{nullptr, 3, "stress_clock"};
        uint32_t resumes = 0;
        for (uint32_t worker = 0; worker < 256; ++worker) {
            tb.spawn_detached(stress_worker(clock, resumes));
        }
        for (uint32_t iteration = 0; iteration < 32; ++iteration) {
            tb.notify_edge(clock.id, EdgeKind::Rising);
        }
        passed &= expect("indexed waiter stress", resumes, 256 * 32);
        passed &= expect("stress done", tb.done() ? 1 : 0, 1);
    }

    {
        Testbench tb;
        Results results;
        const auto worker = tb.spawn(process_worker(results));
        const auto waiter = tb.spawn(wait_for_process(worker, results));
        std::vector<Process> extra_waiters;
        for (uint32_t index = 0; index < 8; ++index) {
            extra_waiters.push_back(tb.spawn(wait_for_process(worker, results)));
        }

        passed &= expect("process valid", worker.valid() ? 1 : 0, 1);
        passed &= expect("process initially running", worker.done() ? 1 : 0, 0);
        tb.set_time(3);
        passed &= expect("process completed", results.process_completed, 1);
        passed &= expect("process done", worker.done() ? 1 : 0, 1);
        passed &= expect("process not cancelled", worker.cancelled() ? 1 : 0,
                         0);
        passed &= expect("process waiters resumed", results.process_waiters, 9);
        passed &= expect("waiter done", waiter.done() ? 1 : 0, 1);
        for (const auto& extra_waiter : extra_waiters) {
            passed &= expect("extra waiter done", extra_waiter.done() ? 1 : 0,
                             1);
        }

        const auto late_waiter = tb.spawn(wait_for_process(worker, results));
        passed &= expect("completed process awaits immediately",
                         results.process_waiters, 10);
        passed &= expect("late waiter done", late_waiter.done() ? 1 : 0, 1);
        passed &= expect("process completion all done", tb.done() ? 1 : 0, 1);
    }

    {
        Testbench tb;
        Results results;
        const auto worker = tb.spawn(cancellable_parent(results));
        const auto waiter = tb.spawn(wait_for_process(worker, results));
        const auto joined_worker = tb.spawn(cancellable_join_parent(results));

        worker.cancel();
        joined_worker.cancel();
        passed &= expect("cancelled process done", worker.done() ? 1 : 0, 1);
        passed &= expect("cancelled process status",
                         worker.cancelled() ? 1 : 0, 1);
        passed &= expect("cancel wakes process waiter", results.process_waiters,
                         1);
        passed &= expect("cancel waiter done", waiter.done() ? 1 : 0, 1);
        passed &= expect("cancelled Join process done",
                         joined_worker.done() ? 1 : 0, 1);
        passed &= expect("cancelled Join process status",
                         joined_worker.cancelled() ? 1 : 0, 1);
        tb.set_time(20);
        passed &= expect("nested child did not resume",
                         results.cancelled_child_resumes, 0);
        passed &= expect("cancel completion all done", tb.done() ? 1 : 0, 1);
    }

    {
        Testbench tb;
        Results results;
        uint32_t cancelled_value = 0;
        const auto target = tb.spawn(delayed_increment(cancelled_value, 20_ns));
        const auto waiter = tb.spawn(cancel_and_await_process(target, results));

        passed &= expect("in-coroutine cancel request leaves done false",
                         results.cancel_await_markers & 0x1, 0x1);
        passed &= expect("cancel-requested await resumes after completion",
                         results.cancel_await_markers, 0x3);
        passed &= expect("cancel-requested awaiter completes",
                         waiter.done() ? 1 : 0, 1);
        passed &= expect("awaited cancellation completes target",
                         target.done() ? 1 : 0, 1);
        passed &= expect("awaited cancellation records status",
                         target.cancelled() ? 1 : 0, 1);
        tb.set_time(20);
        passed &= expect("awaited cancellation prevents target resume",
                         cancelled_value, 0);
    }

    {
        Testbench tb;
        Results results;
        Process worker;
        worker = tb.spawn(self_cancelling_process(&worker, results));

        tb.set_time(1);
        passed &= expect("self cancellation status deferred until boundary",
                         results.self_cancel_markers, 0x1);
        passed &= expect("self-cancelled process done", worker.done() ? 1 : 0,
                         1);
        passed &= expect("self-cancelled process status",
                         worker.cancelled() ? 1 : 0, 1);
        tb.set_time(2);
        passed &= expect("self cancellation prevents another resume",
                         results.self_cancel_markers, 0x1);
        passed &= expect("self cancellation all done", tb.done() ? 1 : 0, 1);
    }

    {
        Testbench tb;
        Results results;
        Process worker;
        worker = tb.spawn(self_cancel_then_return(&worker, results));

        tb.set_time(1);
        passed &= expect("cancel then return status deferred in body",
                         results.normal_self_cancel, 0x1);
        passed &= expect("cancel then return done", worker.done() ? 1 : 0, 1);
        passed &= expect("cancel then return is normal",
                         worker.cancelled() ? 1 : 0, 0);
        passed &= expect("cancel then return all done", tb.done() ? 1 : 0, 1);
    }

    {
        Testbench tb;
        Results results;
        tb.spawn_detached(wait_for_variadic_join(results));
        tb.spawn_detached(wait_for_variadic_first(results));

        tb.set_time(1);
        tb.set_time(2);
        tb.set_time(3);
        passed &= expect("three-task Join", results.variadic_joined, 0xf);
        tb.set_time(4);
        passed &= expect("three-trigger First", results.variadic_first_winner,
                         1);
        passed &= expect("variadic concurrency done", tb.done() ? 1 : 0, 1);
    }

    {
        Testbench tb;
        uint32_t runs = 0;
        uint32_t destructions = 0;
#ifdef CPPTB_CORO_FRAME_POOL_DIAGNOSTICS
        detail::coroutine_frame_pool().reset_stats();
#endif
        for (uint32_t iteration = 0; iteration < 512; ++iteration) {
            const auto root = tb.spawn(immediate_root(runs, destructions));
            passed &= expect("reused tracked root done", root.done() ? 1 : 0,
                             1);
            tb.spawn_detached(immediate_root(runs, destructions));
        }
        passed &= expect("repeated root runs", runs, 1'024);
        passed &= expect("repeated root frame destruction", destructions,
                         1'024);
        passed &= expect("repeated roots all done", tb.done() ? 1 : 0, 1);
#ifdef CPPTB_CORO_FRAME_POOL_DIAGNOSTICS
        const auto& pool_stats = detail::coroutine_frame_pool().stats();
        passed &= expect("coroutine frame pool reuses allocations",
                         pool_stats.reused_allocations > 1'000 ? 1 : 0, 1);
        passed &= expect("coroutine frame pool bounds system allocations",
                         pool_stats.system_allocations < 8 ? 1 : 0, 1);
#endif
    }

#ifdef CPPTB_CORO_FRAME_POOL_DIAGNOSTICS
    {
        detail::coroutine_frame_pool().reset_stats();
        Testbench tb;
        uint32_t checksum = 0;
        const auto process = tb.spawn(oversized_frame(checksum));
        tb.set_time(1);
        passed &= expect("oversized coroutine frame completes",
                         process.done() ? 1 : 0, 1);
        passed &= expect("oversized coroutine frame preserves locals",
                         checksum, 522'240);
        const auto& pool_stats = detail::coroutine_frame_pool().stats();
        passed &= expect("oversized frame bypasses pool allocation",
                         pool_stats.system_allocations > 0 ? 1 : 0, 1);
        passed &= expect("oversized frame bypasses pool deallocation",
                         pool_stats.system_deallocations > 0 ? 1 : 0, 1);
    }
#endif

    {
        Process completed;
        Process completed_copy;
        uint32_t runs = 0;
        uint32_t destructions = 0;
        {
            Testbench tb;
            completed = tb.spawn(immediate_root(runs, destructions));
            completed_copy = completed;
            completed_copy = completed_copy;
            Process copy_assigned;
            copy_assigned = completed;
            Process moved = std::move(copy_assigned);
            passed &= expect("moved-from process invalid",
                             copy_assigned.valid() ? 1 : 0, 0);
            Process move_assigned;
            move_assigned = std::move(moved);
            passed &= expect("move-assigned source invalid",
                             moved.valid() ? 1 : 0, 0);
            move_assigned = std::move(move_assigned);
            completed_copy = move_assigned;
            passed &= expect("copied completed process done",
                             completed_copy.done() ? 1 : 0, 1);
        }
        passed &= expect("copy/move root ran once", runs, 1);
        passed &= expect("copy/move root frame reclaimed once", destructions,
                         1);
        passed &= expect("completed handle survives testbench",
                         completed.done() ? 1 : 0, 1);
        passed &= expect("completed copy survives testbench",
                         completed_copy.done() ? 1 : 0, 1);
        passed &= expect("completed handle remains normal",
                         completed.cancelled() ? 1 : 0, 0);
        completed.cancel();
        passed &= expect("cancel completed handle remains normal",
                         completed.cancelled() ? 1 : 0, 0);
    }

    {
        Process abandoned;
        Process abandoned_copy;
        uint32_t value = 0;
        {
            Testbench tb;
            abandoned = tb.spawn(delayed_increment(value, 20_ns));
            abandoned_copy = abandoned;
        }
        passed &= expect("abandoned handle remains valid",
                         abandoned.valid() ? 1 : 0, 1);
        passed &= expect("abandoned handle is done", abandoned.done() ? 1 : 0,
                         1);
        passed &= expect("abandoned handle is cancelled",
                         abandoned.cancelled() ? 1 : 0, 1);
        passed &= expect("abandoned copy is done",
                         abandoned_copy.done() ? 1 : 0, 1);
        abandoned.cancel();
        passed &= expect("destroyed testbench process did not resume", value, 0);
    }

    {
        Testbench tb;
        uint32_t stale_runs = 0;
        uint32_t stale_destructions = 0;
        uint32_t reused_value = 0;
        const auto stale = tb.spawn(
            immediate_root(stale_runs, stale_destructions));
        const auto reused = tb.spawn(delayed_increment(reused_value, 6_ns));

        stale.cancel();
        passed &= expect("stale completed handle remains normal",
                         stale.cancelled() ? 1 : 0, 0);
        passed &= expect("stale handle cannot cancel reused state",
                         reused.done() ? 1 : 0, 0);
        tb.set_time(6);
        passed &= expect("reused state completes after stale cancellation",
                         reused_value, 1);
        passed &= expect("reused state reports normal completion",
                         reused.cancelled() ? 1 : 0, 0);
        passed &= expect("stale mapped frame reclaimed", stale_destructions,
                         1);
    }

    {
        Testbench tb;
        std::vector<uint32_t> order;
        for (uint32_t id = 0; id < 32; ++id) {
            tb.spawn_detached(ordered_timer(id, order));
        }
        tb.set_time(5);
        passed &= expect("same-deadline timer count",
                         static_cast<uint32_t>(order.size()), 32);
        for (uint32_t id = 0; id < order.size(); ++id) {
            passed &= expect("same-deadline timer FIFO", order[id], id);
        }
    }

    {
        Testbench tb;
        uint32_t cancelled_value = 0;
        Results race_results;
        Process target;
        tb.spawn_detached(cancellation_racer(&target));
        target = tb.spawn(delayed_increment(cancelled_value));
        const auto waiter =
            tb.spawn(wait_for_process(target, race_results));
        tb.set_time(4);
        passed &= expect("cancellation race cancelled target",
                         target.cancelled() ? 1 : 0, 1);
        passed &= expect("cancellation race target done", target.done() ? 1 : 0,
                         1);
        passed &= expect("cancellation race prevented resume", cancelled_value,
                         0);
        passed &= expect("cancellation race waiter done", waiter.done() ? 1 : 0,
                         1);
        passed &= expect("cancellation race waiter resumed",
                         race_results.process_waiters, 1);
    }

    {
        Testbench tb;
        uint32_t completed_value = 0;
        Process target = tb.spawn(delayed_increment(completed_value));
        tb.spawn_detached(cancellation_racer(&target));
        tb.set_time(4);
        passed &= expect("completion race ran target", completed_value, 1);
        passed &= expect("completion race target done", target.done() ? 1 : 0,
                         1);
        passed &= expect("completion race remains normal",
                         target.cancelled() ? 1 : 0, 0);
    }

    {
        Testbench tb;
        Results results;
        const Signal signal{nullptr, 17, "stale_edges"};
        for (uint32_t iteration = 0; iteration < 128; ++iteration) {
            tb.spawn_detached(first_falling_or_timeout(signal, results));
            passed &= expect("Falling/Any interest registered",
                             tb.has_falling_edge_waiters() ? 1 : 0, 1);
            tb.set_time(iteration + 1);
            passed &= expect("stale Falling/Any interest ended",
                             tb.has_falling_edge_waiters() ? 1 : 0, 0);
        }
        passed &= expect("stale edge compaction timeout count",
                         results.timeout_winners, 128);
        tb.notify_edge(signal.id, EdgeKind::Falling);
        passed &= expect("stale compacted edges did not resume",
                         results.edge_winners, 0);

        const auto persistent = tb.spawn(cancellable_edge_wait(signal));
        tb.spawn_detached(first_falling_or_timeout(signal, results));
        tb.set_time(129);
        passed &= expect("one state keeps Falling/Any interest",
                         tb.has_falling_edge_waiters() ? 1 : 0, 1);
        persistent.cancel();
        passed &= expect("last state clears Falling/Any interest",
                         tb.has_falling_edge_waiters() ? 1 : 0, 0);

        tb.spawn_detached(first_falling_or_timeout(signal, results));
        tb.notify_edge(signal.id, EdgeKind::Falling);
        passed &= expect("fresh edge resumes after compaction",
                         results.edge_winners, 1);
        passed &= expect("fresh First clears all falling interest",
                         tb.has_falling_edge_waiters() ? 1 : 0, 0);

        const auto cancelled = tb.spawn(cancellable_edge_wait(signal));
        passed &= expect("cancel edge interest registered",
                         tb.has_falling_edge_waiters() ? 1 : 0, 1);
        cancelled.cancel();
        passed &= expect("cancel clears Falling/Any interest",
                         tb.has_falling_edge_waiters() ? 1 : 0, 0);
    }

    {
        Testbench tb;
        Results results;
        const Signal signal{nullptr, 31, "edge_interest"};

        const auto edge_process = tb.spawn(wait_for_edges(signal, results));
        passed &= expect("rising interest mask",
                         tb.edge_interest(signal.id), kEdgeInterestRising);
        passed &= expect("rising interest query",
                         tb.has_edge_interest(signal.id, EdgeKind::Rising), 1);
        auto change = tb.consume_edge_interest_change();
        passed &= expect("rising interest change present", change.has_value(),
                         1);
        passed &= expect("rising interest change id", change->signal_id,
                         signal.id);
        passed &= expect("rising interest change mask", change->interest,
                         kEdgeInterestRising);

        tb.notify_edge(signal.id, EdgeKind::Rising);
        passed &= expect("edge waiter advances to falling", results.rising, 1);
        passed &= expect("falling interest mask",
                         tb.edge_interest(signal.id), kEdgeInterestFalling);
        change = tb.consume_edge_interest_change();
        passed &= expect("falling interest change present", change.has_value(),
                         1);
        passed &= expect("falling interest change mask", change->interest,
                         kEdgeInterestFalling);

        tb.notify_edge(signal.id, EdgeKind::Falling);
        passed &= expect("edge waiter completes", edge_process.done(), 1);
        passed &= expect("completed waiter clears interest",
                         tb.edge_interest(signal.id), kEdgeInterestNone);
        change = tb.consume_edge_interest_change();
        passed &= expect("completion interest change present",
                         change.has_value(), 1);
        passed &= expect("completion interest mask", change->interest,
                         kEdgeInterestNone);

        const auto any_process = tb.spawn(wait_for_change(signal, results));
        passed &= expect("Any edge maps to both observer directions",
                         tb.edge_interest(signal.id),
                         kEdgeInterestRising | kEdgeInterestFalling);
        change = tb.consume_edge_interest_change();
        passed &= expect("Any interest change present", change.has_value(), 1);
        passed &= expect("Any interest change mask", change->interest,
                         kEdgeInterestRising | kEdgeInterestFalling);
        any_process.cancel();
        passed &= expect("cancelled Any waiter clears interest",
                         tb.edge_interest(signal.id), kEdgeInterestNone);
        change = tb.consume_edge_interest_change();
        passed &= expect("cancel interest change present", change.has_value(),
                         1);
        passed &= expect("cancel interest change mask", change->interest,
                         kEdgeInterestNone);
        passed &= expect("interest change queue drains",
                         tb.consume_edge_interest_change().has_value(), 0);
    }

    {
        Testbench tb;
        const Signal rising{nullptr, 32, "dynamic_rising"};
        const Signal falling{nullptr, 33, "dynamic_falling"};
        const Signal changed{nullptr, 34, "dynamic_changed"};
        uint32_t winner = 99;
        uint32_t resumes = 0;
        const auto process = tb.spawn(wait_for_dynamic_edge_first(
            rising, falling, changed, winner, resumes));

        passed &= expect("dynamic First rising interest",
                         tb.edge_interest(rising.id), kEdgeInterestRising);
        passed &= expect("dynamic First falling interest",
                         tb.edge_interest(falling.id), kEdgeInterestFalling);
        passed &= expect("dynamic First any-edge interest",
                         tb.edge_interest(changed.id),
                         kEdgeInterestRising | kEdgeInterestFalling);
        passed &= expect("dynamic First falling summary",
                         tb.has_falling_edge_waiters() ? 1 : 0, 1);

        tb.notify_edge(rising.id, EdgeKind::Rising);
        passed &= expect("dynamic First winner", winner, 0);
        passed &= expect("dynamic First completes", process.done() ? 1 : 0,
                         1);
        passed &= expect("dynamic First resumes once", resumes, 1);
        passed &= expect("dynamic First clears winner interest",
                         tb.edge_interest(rising.id), kEdgeInterestNone);
        passed &= expect("dynamic First clears falling loser interest",
                         tb.edge_interest(falling.id), kEdgeInterestNone);
        passed &= expect("dynamic First clears any-edge loser interest",
                         tb.edge_interest(changed.id), kEdgeInterestNone);
        passed &= expect("dynamic First clears falling summary",
                         tb.has_falling_edge_waiters() ? 1 : 0, 0);

        tb.notify_edge(falling.id, EdgeKind::Falling);
        tb.notify_edge(changed.id, EdgeKind::Any);
        passed &= expect("dynamic First stale losers do not resume", resumes,
                         1);
    }

    {
        Testbench tb;
        Results results;
        const Signal clock{nullptr, 32, "static_clock"};
        const Signal observer{nullptr, 33, "tracked_observer"};
        tb.configure_static_edge_source(clock.id);
        const auto initial_generation = tb.edge_interest_generation();

        passed &= expect("configured static edge source",
                         tb.is_static_edge_source(clock.id), 1);
        passed &= expect("observer remains dynamically tracked",
                         tb.is_static_edge_source(observer.id), 0);
        const auto edge_process = tb.spawn(wait_for_edges(clock, results));
        passed &= expect("static clock reports no rising interest",
                         tb.edge_interest(clock.id), kEdgeInterestNone);
        passed &= expect("static clock publishes no rising change",
                         tb.consume_edge_interest_change().has_value(), 0);
        passed &= expect("static clock generation remains stable",
                         tb.edge_interest_generation(), initial_generation);

        tb.notify_edge(clock.id, EdgeKind::Rising);
        passed &= expect("static clock rising edge resumes", results.rising,
                         1);
        passed &= expect("static clock reports no falling interest",
                         tb.edge_interest(clock.id), kEdgeInterestNone);
        passed &= expect("static clock retains falling waiter summary",
                         tb.has_falling_edge_waiters() ? 1 : 0, 1);
        passed &= expect("static clock publishes no falling change",
                         tb.consume_edge_interest_change().has_value(), 0);

        tb.notify_edge(clock.id, EdgeKind::Falling);
        passed &= expect("static clock falling edge resumes",
                         results.falling, 1);
        passed &= expect("static clock waiter completes",
                         edge_process.done(), 1);
        passed &= expect("static clock remains without interest",
                         tb.edge_interest(clock.id), kEdgeInterestNone);
        passed &= expect("static clock clears falling summary",
                         tb.has_falling_edge_waiters() ? 1 : 0, 0);
        passed &= expect("static clock publishes no completion change",
                         tb.consume_edge_interest_change().has_value(), 0);
        passed &= expect("static clock generation stays stable",
                         tb.edge_interest_generation(), initial_generation);

        const auto cancelled = tb.spawn(cancellable_edge_wait(clock));
        passed &= expect("static cancellation registers falling waiters",
                         tb.has_falling_edge_waiters() ? 1 : 0, 1);
        cancelled.cancel();
        passed &= expect("static cancellation completes", cancelled.cancelled(),
                         1);
        passed &= expect("static cancellation clears falling waiters",
                         tb.has_falling_edge_waiters() ? 1 : 0, 0);
        tb.notify_edge(clock.id, EdgeKind::Falling);
        tb.notify_edge(clock.id, EdgeKind::Any);
        passed &= expect("cancelled static wait stays cancelled",
                         cancelled.cancelled(), 1);

        uint32_t first_winner = 99;
        const auto first_process =
            tb.spawn(wait_for_static_first(clock, observer, first_winner));
        passed &= expect("three-edge First tracks observer interest",
                         tb.edge_interest(observer.id),
                         kEdgeInterestRising | kEdgeInterestFalling);
        passed &= expect("three-edge First keeps static interest empty",
                         tb.edge_interest(clock.id), kEdgeInterestNone);
        passed &= expect("three-edge First has falling registrations",
                         tb.has_falling_edge_waiters() ? 1 : 0, 1);
        auto change = tb.consume_edge_interest_change();
        passed &= expect("tracked observer publishes active interest",
                         change.has_value(), 1);
        passed &= expect("tracked observer change id", change->signal_id,
                         observer.id);
        passed &= expect("tracked observer active mask", change->interest,
                         kEdgeInterestRising | kEdgeInterestFalling);

        tb.notify_edge(clock.id, EdgeKind::Rising);
        passed &= expect("static edge wins mixed First", first_winner, 0);
        passed &= expect("mixed First completes on static edge",
                         first_process.done(), 1);
        passed &= expect("mixed First clears observer interest",
                         tb.edge_interest(observer.id), kEdgeInterestNone);
        passed &= expect("mixed First clears falling registrations",
                         tb.has_falling_edge_waiters() ? 1 : 0, 0);
        change = tb.consume_edge_interest_change();
        passed &= expect("tracked observer publishes cleared interest",
                         change.has_value(), 1);
        passed &= expect("tracked observer cleared mask", change->interest,
                         kEdgeInterestNone);
        passed &= expect("static source adds no publication changes",
                         tb.consume_edge_interest_change().has_value(), 0);

        uint32_t delay_winner = 99;
        const auto delay_process =
            tb.spawn(wait_for_static_first(clock, observer, delay_winner));
        change = tb.consume_edge_interest_change();
        passed &= expect("delay race publishes observer interest",
                         change.has_value(), 1);
        passed &= expect("delay race observer active mask", change->interest,
                         kEdgeInterestRising | kEdgeInterestFalling);
        tb.set_time(5);
        passed &= expect("Delay wins mixed First", delay_winner, 3);
        passed &= expect("mixed First completes on Delay", delay_process.done(),
                         1);
        passed &= expect("Delay clears observer interest",
                         tb.edge_interest(observer.id), kEdgeInterestNone);
        passed &= expect("Delay clears static falling registrations",
                         tb.has_falling_edge_waiters() ? 1 : 0, 0);
        change = tb.consume_edge_interest_change();
        passed &= expect("Delay publishes observer interest removal",
                         change.has_value(), 1);
        passed &= expect("Delay observer cleared mask", change->interest,
                         kEdgeInterestNone);
        passed &= expect("static source still publishes no interest",
                         tb.consume_edge_interest_change().has_value(), 0);
    }

    {
        Testbench tb;
        uint32_t spawned_runs = 0;
        uint32_t spawned_destructions = 0;
        uint32_t destructor_runs = 0;
        const auto root = tb.spawn(spawn_from_frame_destructor(
            tb, spawned_runs, spawned_destructions, destructor_runs));
        passed &= expect("reentrant destructor root done", root.done() ? 1 : 0,
                         1);
        passed &= expect("reentrant destructor invoked", destructor_runs, 1);
        passed &= expect("reentrant destructor spawned root", spawned_runs, 1);
        passed &= expect("reentrant spawned frame reclaimed",
                         spawned_destructions, 1);
        passed &= expect("reentrant spawn all done", tb.done() ? 1 : 0, 1);
    }

    {
        Testbench tb;
        uint32_t target_value = 0;
        uint32_t destructor_runs = 0;
        const auto target = tb.spawn(delayed_increment(target_value, 50_ns));
        tb.spawn_detached(cancel_from_frame_destructor(target, destructor_runs));
        passed &= expect("reentrant cancel destructor invoked", destructor_runs,
                         1);
        passed &= expect("reentrant cancel target done", target.done() ? 1 : 0,
                         1);
        passed &= expect("reentrant cancel target cancelled",
                         target.cancelled() ? 1 : 0, 1);
        tb.set_time(50);
        passed &= expect("reentrant cancellation prevented resume", target_value,
                         0);
    }

    {
        uint32_t attempted = 0;
        uint32_t spawned_runs = 0;
        uint32_t spawned_destructions = 0;
        Process retained;
        {
            Testbench tb;
            const Signal signal{nullptr, 23, "shutdown_reentry"};
            retained = tb.spawn(suspend_with_shutdown_callback(
                tb, signal, attempted, spawned_runs, spawned_destructions));
        }
        passed &= expect("shutdown frame destructor invoked", attempted, 1);
        passed &= expect("shutdown rejected reentrant spawn", spawned_runs, 0);
        passed &= expect("shutdown temporary frame destroyed",
                         spawned_destructions, 1);
        passed &= expect("shutdown retained handle done",
                         retained.done() ? 1 : 0, 1);
        passed &= expect("shutdown retained handle cancelled",
                         retained.cancelled() ? 1 : 0, 1);
    }

    {
        uint32_t target_value = 0;
        uint32_t destructor_runs = 0;
        Process target;
        Process callback;
        {
            Testbench tb;
            const Signal signal{nullptr, 24, "shutdown_cancel"};
            target = tb.spawn(delayed_increment(target_value, 50_ns));
            callback = tb.spawn(suspend_with_shutdown_cancel(
                target, signal, destructor_runs));
        }
        passed &= expect("shutdown cancel destructor invoked", destructor_runs,
                         1);
        passed &= expect("shutdown cancel target done", target.done() ? 1 : 0,
                         1);
        passed &= expect("shutdown cancel target cancelled",
                         target.cancelled() ? 1 : 0, 1);
        passed &= expect("shutdown cancel callback done",
                         callback.done() ? 1 : 0, 1);
        passed &= expect("shutdown cancel callback cancelled",
                         callback.cancelled() ? 1 : 0, 1);
        passed &= expect("shutdown cancel target did not resume", target_value,
                         0);
    }

    passed &= expect_abort("invalid Process await abort",
                           trigger_invalid_process_wait,
                           "cannot await an invalid process");
    passed &= expect_abort("zero Delay abort", trigger_zero_delay_wait,
                           "delay duration must be greater than zero");
    passed &= expect_abort(
        "subprecision Delay abort", trigger_subprecision_delay_wait,
        "delay of 1 fs is not representable at 1000 fs simulation precision");
    passed &= expect_abort(
        "late static edge source configuration abort",
        trigger_late_static_edge_source_configuration,
        "static edge source 37 must be configured before registering any waits");
    passed &= expect_abort(
        "late static source after fast edge wait abort",
        trigger_late_static_source_after_edge_wait,
        "static edge source 38 must be configured before registering any waits");
    passed &= expect_abort("invalid timed Task abort",
                           trigger_invalid_timed_task,
                           "with_timeout received an invalid task");
    passed &= expect_abort("timed-out Task value access abort",
                           trigger_invalid_timed_value_access,
                           "timed-out task has no value");
    passed &= expect_abort("zero Task timeout abort",
                           trigger_zero_task_timeout,
                           "delay duration must be greater than zero");
    passed &= expect_abort(
        "subprecision Task timeout abort", trigger_subprecision_task_timeout,
        "delay of 1 fs is not representable at 1000 fs simulation precision");
    passed &= expect_abort("typed Task exception abort",
                           trigger_typed_task_exception,
                           "unhandled exception in coroutine");
    passed &= expect_abort(
        "Event cross-scheduler abort", trigger_event_cross_scheduler,
        "Event cannot have waiters from multiple schedulers");
    passed &= expect_abort(
        "Event active lifetime abort", trigger_event_destroyed_with_waiter,
        "Event destroyed with active waiters");
    passed &= expect_abort(
        "Channel cross-scheduler abort", trigger_channel_cross_scheduler,
        "Channel cannot have waiters from multiple schedulers");
    passed &= expect_abort(
        "Channel active lifetime abort", trigger_channel_destroyed_with_waiter,
        "Channel destroyed with active waiters");
    passed &= expect_abort("unpacked array bounds abort",
                           trigger_unpacked_array_oob,
                           "array_i' dimension 1 index 3 is out of bounds [4:7]");
    passed &= expect_abort(
        "multidimensional array bounds abort",
        trigger_multidimensional_array_oob,
        "matrix_i' dimension 2 index 3 is out of bounds [-1:1]");
    passed &= expect_abort(
        "memory probe bounds abort", trigger_memory_probe_oob,
        "memory' index 3 is out of bounds [7:4]");
    passed &= expect_abort(
        "probe callback lifetime abort", trigger_probe_outside_callback,
        "internal probe 'state' used outside a DPI callback");
    return passed ? 0 : 1;
}
