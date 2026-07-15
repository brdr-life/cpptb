#pragma once

#include <array>
#include <compare>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <queue>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "cpptb/packed_bits.hpp"
#include "vpi_user.h"

#define CPPTB_CORO_PACKED_SIGNAL_API 1
#define CPPTB_CORO_UNPACKED_ARRAY_API 1
#define CPPTB_CORO_MULTIDIMENSIONAL_ARRAY_API 1

namespace cpptb::coro {

enum class EdgeKind : uint8_t {
    Rising,
    Falling,
    Any,
};

enum class WaitKind : uint8_t {
    Edge,
    Delay,
    ReadWrite,
    ReadOnly,
    NextTimeStep,
};

enum class SimulationPhase : uint8_t {
    Evaluation,
    ReadWrite,
    ReadOnly,
    NextTimeStep,
};

enum EdgeInterest : uint8_t {
    kEdgeInterestNone = 0,
    kEdgeInterestRising = 1u << 0,
    kEdgeInterestFalling = 1u << 1,
};

struct EdgeInterestChange {
    uint32_t signal_id = 0;
    uint8_t interest = kEdgeInterestNone;
};

namespace detail {

template <typename>
inline constexpr bool unsupported_port_operation_v = false;

template <typename Value>
void unsupported_port_force() {
    static_assert(
        unsupported_port_operation_v<Value>,
        "cpptb: force() is not supported on ordinary DUT ports. "
        "Scheduler-owned clocks must remain controlled by "
        "TestContext::start_clock(); coherent clock pause/override is not "
        "yet supported. Use force() only on inferred hierarchical objects, "
        "for example dut.block.signal.force(value).");
}

template <typename Port>
void unsupported_port_release() {
    static_assert(
        unsupported_port_operation_v<Port>,
        "cpptb: release() is not supported on ordinary DUT ports. "
        "Scheduler-owned clocks must remain controlled by "
        "TestContext::start_clock(); coherent clock pause/override is not "
        "yet supported. Use release() only on an inferred hierarchical "
        "object that was previously forced.");
}

}  // namespace detail

struct Signal {
    using GetFn = uint32_t (*)(void* context, uint32_t id);
    using SetFn = void (*)(void* context, uint32_t id, uint32_t data);

    vpiHandle handle = nullptr;
    uint32_t id = 0;
    const char* name = "";
    void* context = nullptr;
    GetFn get_fn = nullptr;
    SetFn set_fn = nullptr;

    uint32_t get() const {
        if (!get_fn) {
            std::fprintf(stderr, "cpptb: signal %u has no get transport\n", id);
            std::abort();
        }
        return get_fn(context, id);
    }

    void set(uint32_t data) const {
        if (!set_fn) {
            std::fprintf(stderr, "cpptb: signal %u has no set transport\n", id);
            std::abort();
        }
        set_fn(context, id, data);
    }

    template <typename Value>
    void force(Value&&) const {
        detail::unsupported_port_force<Value>();
    }

    template <typename Port = Signal>
    void release() const {
        detail::unsupported_port_release<Port>();
    }
};

template <size_t Width, bool Writable>
struct SignalSpec {
    static_assert(Width > 0, "signal width must be positive");

    static constexpr size_t width = Width;
    static constexpr bool writable = Writable;
};

template <size_t ElementWidth, int32_t Left, int32_t Right, bool Writable>
struct ArraySpec {
    static_assert(ElementWidth > 0, "array element width must be positive");

    static constexpr size_t element_width = ElementWidth;
    static constexpr int32_t left = Left;
    static constexpr int32_t right = Right;
    static constexpr bool writable = Writable;
};

template <int32_t Left, int32_t Right>
struct ArrayDimension {
    static constexpr int32_t left = Left;
    static constexpr int32_t right = Right;
    static constexpr int32_t low = Left < Right ? Left : Right;
    static constexpr int32_t high = Left < Right ? Right : Left;
    static constexpr size_t size =
        static_cast<size_t>(high - low) + 1;
};

template <size_t ElementWidth, bool Writable, typename... Dimensions>
struct FixedArraySpec {
    static_assert(ElementWidth > 0, "array element width must be positive");
    static_assert(sizeof...(Dimensions) > 0,
                  "an unpacked array must have at least one dimension");

    static constexpr size_t element_width = ElementWidth;
    static constexpr bool writable = Writable;
    static constexpr size_t rank = sizeof...(Dimensions);
    static constexpr size_t element_count = (Dimensions::size * ...);
    static constexpr size_t word_count =
        element_count * ((ElementWidth + 31) / 32);
};

template <size_t Width>
using PackedSignalValue = std::conditional_t<
    (Width <= 32), uint32_t,
    std::conditional_t<(Width <= 64), uint64_t, cpptb::Bits<Width>>>;

template <size_t Width, bool Writable>
class PackedSignal {
    static_assert(Width > 32,
                  "PackedSignal is reserved for signals wider than 32 bits");

   public:
    using value_type = PackedSignalValue<Width>;
    using GetWordsFn = void (*)(void* context, uint32_t id, uint32_t* words,
                                uint32_t word_count);
    using SetWordsFn = void (*)(void* context, uint32_t id,
                                const uint32_t* words, uint32_t word_count);

    static constexpr size_t width = Width;
    static constexpr size_t word_count = (Width + 31) / 32;

    uint32_t id = 0;
    const char* name = "";
    void* context = nullptr;
    GetWordsFn get_words_fn = nullptr;
    SetWordsFn set_words_fn = nullptr;

    value_type get() const {
        if (!get_words_fn) {
            std::fprintf(stderr,
                         "cpptb: signal %u has no packed get transport\n", id);
            std::abort();
        }

        typename cpptb::Bits<Width>::word_array words{};
        get_words_fn(context, id, words.data(),
                     static_cast<uint32_t>(word_count));
        const auto bits = cpptb::Bits<Width>::from_words(words);
        if constexpr (Width <= 64) {
            return bits.to_uint64();
        } else {
            return bits;
        }
    }

    void set(value_type value) const requires(Writable) {
        if (!set_words_fn) {
            std::fprintf(stderr,
                         "cpptb: signal %u has no packed set transport\n", id);
            std::abort();
        }

        cpptb::Bits<Width> bits;
        if constexpr (Width <= 64) {
            bits = cpptb::Bits<Width>::from_uint(value);
        } else {
            bits = value;
        }
        set_words_fn(context, id, bits.words().data(),
                     static_cast<uint32_t>(word_count));
    }

    template <typename Value>
    void force(Value&&) const {
        detail::unsupported_port_force<Value>();
    }

    template <typename Port = PackedSignal>
    void release() const {
        detail::unsupported_port_release<Port>();
    }
};

template <size_t Width>
using DrivenSignal = PackedSignal<Width, true>;

template <size_t Width>
using ObservedSignal = PackedSignal<Width, false>;

namespace detail {

template <typename... Dimensions>
inline constexpr size_t array_element_count_v = (Dimensions::size * ... * 1);

}  // namespace detail

// Transport words are flattened in row-major declaration order: dimensions
// are visited from left to right, every index runs from its numeric low bound
// to high bound, and the packed element words run least-significant first.
template <size_t ElementWidth, bool Writable, size_t DimensionIndex,
          typename Dimension, typename... RemainingDimensions>
class FixedUnpackedArray {
   public:
    using GetWordsFn = void (*)(void* context, uint32_t id, uint32_t* words,
                                uint32_t word_count);
    using SetWordsFn = void (*)(void* context, uint32_t id,
                                const uint32_t* words, uint32_t word_count);

    static constexpr size_t element_width = ElementWidth;
    static constexpr size_t element_word_count = (ElementWidth + 31) / 32;
    static constexpr size_t rank = 1 + sizeof...(RemainingDimensions);
    static constexpr int32_t left() { return Dimension::left; }
    static constexpr int32_t right() { return Dimension::right; }
    static constexpr int32_t low() { return Dimension::low; }
    static constexpr int32_t high() { return Dimension::high; }
    static constexpr size_t size() { return Dimension::size; }
    static constexpr size_t element_count =
        Dimension::size *
        detail::array_element_count_v<RemainingDimensions...>;
    static constexpr size_t word_count = element_count * element_word_count;

    uint32_t base_id = 0;
    const char* name = "";
    void* context = nullptr;
    Signal::GetFn get_fn = nullptr;
    Signal::SetFn set_fn = nullptr;
    GetWordsFn get_words_fn = nullptr;
    SetWordsFn set_words_fn = nullptr;

    [[nodiscard]] auto at(int32_t index) const {
        if (index < low() || index > high()) {
            std::fprintf(stderr,
                         "cpptb: unpacked array '%s' dimension %zu index %d "
                         "is out of bounds [%d:%d]\n",
                         name, DimensionIndex + 1, index, Dimension::left,
                         Dimension::right);
            std::abort();
        }
        constexpr size_t stride_words =
            element_word_count *
            detail::array_element_count_v<RemainingDimensions...>;
        const uint32_t element_id =
            base_id + static_cast<uint32_t>(index - low()) *
                          static_cast<uint32_t>(stride_words);
        if constexpr (sizeof...(RemainingDimensions) != 0) {
            return FixedUnpackedArray<ElementWidth, Writable,
                                      DimensionIndex + 1,
                                      RemainingDimensions...>{
                element_id, name, context, get_fn, set_fn, get_words_fn,
                set_words_fn};
        } else if constexpr (ElementWidth <= 32) {
            return Signal{nullptr, element_id, name, context, get_fn, set_fn};
        } else {
            return PackedSignal<ElementWidth, Writable>{
                element_id, name, context, get_words_fn, set_words_fn};
        }
    }
};

template <size_t ElementWidth, int32_t Left, int32_t Right, bool Writable>
using UnpackedArray =
    FixedUnpackedArray<ElementWidth, Writable, 0,
                       ArrayDimension<Left, Right>>;

template <size_t Width, bool Writable, typename... Dimensions>
using FixedArray = FixedUnpackedArray<Width, Writable, 0, Dimensions...>;

template <size_t Width, int32_t Left, int32_t Right>
using DrivenArray = UnpackedArray<Width, Left, Right, true>;

template <size_t Width, int32_t Left, int32_t Right>
using ObservedArray = UnpackedArray<Width, Left, Right, false>;

template <size_t Width, typename... Dimensions>
using DrivenFixedArray = FixedArray<Width, true, Dimensions...>;

template <size_t Width, typename... Dimensions>
using ObservedFixedArray = FixedArray<Width, false, Dimensions...>;

template <size_t ElementWidth, bool Writable, typename FirstDimension,
          typename... RemainingDimensions, size_t TransportWidth,
          int32_t Left, int32_t Right>
auto reshape_fixed_array(
    FixedArraySpec<ElementWidth, Writable, FirstDimension,
                   RemainingDimensions...>,
    UnpackedArray<TransportWidth, Left, Right, Writable> transport) {
    static_assert(FirstDimension::left == Left &&
                      FirstDimension::right == Right,
                  "transport and fixed array outer dimensions must match");
    constexpr size_t expected_transport_width =
        ((ElementWidth + 31) / 32) *
        detail::array_element_count_v<RemainingDimensions...> * 32;
    static_assert(TransportWidth == expected_transport_width,
                  "transport width must cover every inner array word");
    return FixedArray<ElementWidth, Writable, FirstDimension,
                      RemainingDimensions...>{
        transport.base_id,       transport.name,
        transport.context,       transport.get_fn,
        transport.set_fn,        transport.get_words_fn,
        transport.set_words_fn};
}

inline uint32_t vpi_signal_get(void* context, uint32_t) {
    auto* handle = reinterpret_cast<vpiHandle>(context);
    if (!handle) {
        std::fprintf(stderr, "cpptb: VPI signal has no handle\n");
        std::abort();
    }

    s_vpi_value value;
    value.format = vpiIntVal;
    vpi_get_value(handle, &value);
    return static_cast<uint32_t>(value.value.integer);
}

inline void vpi_signal_set(void* context, uint32_t, uint32_t data) {
    auto* handle = reinterpret_cast<vpiHandle>(context);
    if (!handle) {
        std::fprintf(stderr, "cpptb: VPI signal has no handle\n");
        std::abort();
    }

    s_vpi_value value;
    value.format = vpiIntVal;
    value.value.integer = static_cast<PLI_INT32>(data);
    vpi_put_value(handle, &value, nullptr, vpiNoDelay);
}

inline Signal make_vpi_signal(vpiHandle handle, uint32_t id,
                              const char* name = "") {
    return Signal{handle, id, name, handle, vpi_signal_get, vpi_signal_set};
}

struct RisingEdge {
    Signal signal;
};

struct FallingEdge {
    Signal signal;
};

struct Edge {
    Signal signal;
};

struct SimTime {
    uint64_t femtoseconds = 0;

    constexpr uint64_t in_femtoseconds() const { return femtoseconds; }
    constexpr uint64_t in_picoseconds() const { return femtoseconds / 1'000u; }
    constexpr uint64_t in_nanoseconds() const {
        return femtoseconds / 1'000'000u;
    }

    friend constexpr bool operator==(SimTime, SimTime) = default;
    friend constexpr auto operator<=>(SimTime, SimTime) = default;
    friend constexpr SimTime operator+(SimTime left, SimTime right) {
        return SimTime{left.femtoseconds + right.femtoseconds};
    }
};

struct ClockRegistrar {
    using StartFn = void (*)(void* context, Signal signal, SimTime period,
                             SimTime phase);

    void* context = nullptr;
    StartFn start_fn = nullptr;

    void start(Signal signal, SimTime period, SimTime phase = {}) const {
        if (!start_fn) {
            std::fprintf(stderr,
                         "cpptb: this simulator adapter cannot start clocks\n");
            std::abort();
        }
        start_fn(context, signal, period, phase);
    }

    explicit operator bool() const { return start_fn != nullptr; }
};

constexpr SimTime operator""_fs(unsigned long long value) {
    return SimTime{static_cast<uint64_t>(value)};
}

constexpr SimTime operator""_ps(unsigned long long value) {
    return SimTime{static_cast<uint64_t>(value) * 1'000u};
}

constexpr SimTime operator""_ns(unsigned long long value) {
    return SimTime{static_cast<uint64_t>(value) * 1'000'000u};
}

constexpr SimTime operator""_us(unsigned long long value) {
    return SimTime{static_cast<uint64_t>(value) * 1'000'000'000u};
}

constexpr SimTime operator""_ms(unsigned long long value) {
    return SimTime{static_cast<uint64_t>(value) * 1'000'000'000'000u};
}

struct Delay {
    SimTime duration;

    explicit constexpr Delay(SimTime value) : duration(value) {}
};

struct ReadWrite {};
struct ReadOnly {};
struct NextTimeStep {};

template <typename... Triggers>
struct First {
    static_assert(sizeof...(Triggers) >= 2,
                  "First requires at least two triggers");

    std::tuple<Triggers...> triggers;

    explicit First(Triggers... values) : triggers(std::move(values)...) {}
};

template <typename... Triggers>
First(Triggers...) -> First<Triggers...>;

struct WaitRequest {
    WaitKind kind = WaitKind::Edge;
    uint32_t signal_id = 0;
    EdgeKind edge = EdgeKind::Rising;
    SimTime delay{};

    static WaitRequest edge_on(Signal signal, EdgeKind edge_kind) {
        return {WaitKind::Edge, signal.id, edge_kind, {}};
    }

    static WaitRequest delay_for(SimTime duration) {
        return {WaitKind::Delay, 0, EdgeKind::Rising, duration};
    }

    static WaitRequest phase(WaitKind kind) {
        return {kind, 0, EdgeKind::Rising, {}};
    }
};

inline WaitRequest wait_request(RisingEdge trigger) {
    return WaitRequest::edge_on(trigger.signal, EdgeKind::Rising);
}

inline WaitRequest wait_request(FallingEdge trigger) {
    return WaitRequest::edge_on(trigger.signal, EdgeKind::Falling);
}

inline WaitRequest wait_request(Edge trigger) {
    return WaitRequest::edge_on(trigger.signal, EdgeKind::Any);
}

inline WaitRequest wait_request(Delay trigger) {
    return WaitRequest::delay_for(trigger.duration);
}

inline WaitRequest wait_request(ReadWrite) {
    return WaitRequest::phase(WaitKind::ReadWrite);
}

inline WaitRequest wait_request(ReadOnly) {
    return WaitRequest::phase(WaitKind::ReadOnly);
}

inline WaitRequest wait_request(NextTimeStep) {
    return WaitRequest::phase(WaitKind::NextTimeStep);
}

class Scheduler;
struct JoinState;
struct ProcessControl;
struct SchedulerLifetime;

template <typename T>
class Task;

namespace detail {

#ifdef CPPTB_CORO_FRAME_POOL_DIAGNOSTICS
struct CoroutineFramePoolStats {
    uint64_t system_allocations = 0;
    uint64_t reused_allocations = 0;
    uint64_t cached_deallocations = 0;
    uint64_t system_deallocations = 0;
};
#endif

class CoroutineFramePool {
   public:
    static constexpr size_t kAlignment = alignof(std::max_align_t);
    static constexpr size_t kMaxFrameSize = 2'048;
    static constexpr size_t kBucketCount = kMaxFrameSize / kAlignment;
    static constexpr uint16_t kMaxCachedPerBucket = 32;
    static_assert(kMaxFrameSize % kAlignment == 0);

    CoroutineFramePool() = default;
    CoroutineFramePool(const CoroutineFramePool&) = delete;
    CoroutineFramePool& operator=(const CoroutineFramePool&) = delete;

    ~CoroutineFramePool() {
        for (auto* head : free_lists_) {
            while (head) {
                auto* next = head->next;
                ::operator delete(head);
                head = next;
            }
        }
    }

    void* allocate(size_t size) {
        const size_t bucket = bucket_for(size);
        if (bucket == kBucketCount) {
#ifdef CPPTB_CORO_FRAME_POOL_DIAGNOSTICS
            ++stats_.system_allocations;
#endif
            return ::operator new(size);
        }

        if (auto* node = free_lists_[bucket]) {
            free_lists_[bucket] = node->next;
            --cached_counts_[bucket];
#ifdef CPPTB_CORO_FRAME_POOL_DIAGNOSTICS
            ++stats_.reused_allocations;
#endif
            return node;
        }

#ifdef CPPTB_CORO_FRAME_POOL_DIAGNOSTICS
        ++stats_.system_allocations;
#endif
        return ::operator new((bucket + 1) * kAlignment);
    }

    void deallocate(void* pointer, size_t size) noexcept {
        if (!pointer) return;
        const size_t bucket = bucket_for(size);
        if (bucket == kBucketCount ||
            cached_counts_[bucket] == kMaxCachedPerBucket) {
#ifdef CPPTB_CORO_FRAME_POOL_DIAGNOSTICS
            ++stats_.system_deallocations;
#endif
            ::operator delete(pointer);
            return;
        }

        auto* node = static_cast<FreeNode*>(pointer);
        node->next = free_lists_[bucket];
        free_lists_[bucket] = node;
        ++cached_counts_[bucket];
#ifdef CPPTB_CORO_FRAME_POOL_DIAGNOSTICS
        ++stats_.cached_deallocations;
#endif
    }

#ifdef CPPTB_CORO_FRAME_POOL_DIAGNOSTICS
    const CoroutineFramePoolStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = {}; }
#endif

   private:
    struct FreeNode {
        FreeNode* next = nullptr;
    };

    static constexpr size_t bucket_for(size_t size) noexcept {
        if (size == 0 || size > kMaxFrameSize) return kBucketCount;
        return (size - 1) / kAlignment;
    }

    std::array<FreeNode*, kBucketCount> free_lists_{};
    std::array<uint16_t, kBucketCount> cached_counts_{};
#ifdef CPPTB_CORO_FRAME_POOL_DIAGNOSTICS
    CoroutineFramePoolStats stats_{};
#endif
};

inline CoroutineFramePool& coroutine_frame_pool() {
    static thread_local CoroutineFramePool pool;
    return pool;
}

}  // namespace detail

struct TaskPromiseBase {
    Scheduler* scheduler = nullptr;
    std::coroutine_handle<> continuation = nullptr;
    std::shared_ptr<JoinState> join_state;
    size_t state_index = std::numeric_limits<size_t>::max();

    static void* operator new(size_t size) {
        return detail::coroutine_frame_pool().allocate(size);
    }

    static void operator delete(void* pointer, size_t size) noexcept {
        detail::coroutine_frame_pool().deallocate(pointer, size);
    }

    std::suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
        bool await_ready() const noexcept { return false; }

        template <typename Promise>
        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<Promise> handle) const noexcept;

        void await_resume() const noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    void unhandled_exception() {
        std::fprintf(stderr, "cpptb: unhandled exception in coroutine\n");
        std::abort();
    }
};

template <typename T>
struct TaskPromise : TaskPromiseBase {
    std::optional<T> result;

    Task<T> get_return_object();

    template <typename U>
        requires std::constructible_from<T, U&&>
    void return_value(U&& value) {
        result.emplace(std::forward<U>(value));
    }
};

template <>
struct TaskPromise<void> : TaskPromiseBase {
    Task<void> get_return_object();
    void return_void() noexcept {}
};

template <typename T>
class Task {
   public:
    using promise_type = TaskPromise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

    explicit Task(handle_type handle = nullptr) : handle_(handle) {}

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    Task& operator=(Task&& other) noexcept {
        if (this == &other) return *this;
        destroy();
        handle_ = std::exchange(other.handle_, nullptr);
        return *this;
    }

    ~Task() { destroy(); }

    struct Awaiter;
    Awaiter operator co_await() &&;

    handle_type release() { return std::exchange(handle_, nullptr); }

   private:
    void destroy() {
        if (handle_) {
            handle_.destroy();
            handle_ = nullptr;
        }
    }

    handle_type handle_ = nullptr;
};

template <typename T>
Task<T> TaskPromise<T>::get_return_object() {
    return Task<T>{Task<T>::handle_type::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() {
    return Task<void>{Task<void>::handle_type::from_promise(*this)};
}

class Process {
   public:
    Process() = default;
    Process(const Process& other) noexcept;
    Process& operator=(const Process& other) noexcept;
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;
    ~Process();

    bool valid() const;
    bool done() const;
    bool cancelled() const;
    void cancel() const;

    struct Awaiter;
    Awaiter operator co_await() const;

   private:
    friend class Scheduler;

    explicit Process(ProcessControl* control) noexcept;

    ProcessControl* control_ = nullptr;
};

struct JoinState {
    uint32_t remaining = 0;
    size_t parent_index = std::numeric_limits<size_t>::max();
};

struct WaitRegistration {
    size_t state_index = std::numeric_limits<size_t>::max();
    uint64_t generation = 0;
    size_t* winner = nullptr;
    size_t winner_index = 0;
};

struct CoroutineState {
    std::coroutine_handle<> handle = nullptr;
    TaskPromiseBase* promise = nullptr;
    uint64_t wait_generation = 0;
    uint32_t edge_wait_count = 0;
    uint32_t falling_edge_wait_count : 29 = 0;
    uint32_t waiting : 1 = 0;
    uint32_t ready : 1 = 0;
    uint32_t done : 1 = 0;
};

struct SchedulerLifetime {};

struct ExternalWaitRegistration {
    Scheduler* scheduler = nullptr;
    std::weak_ptr<SchedulerLifetime> lifetime;
    WaitRegistration wait;
};

struct ProcessControl {
    Scheduler* scheduler = nullptr;
    std::vector<WaitRegistration> completion_waiters;
    size_t state_index = std::numeric_limits<size_t>::max();
    size_t scheduler_slot = std::numeric_limits<size_t>::max();
    size_t references = 1;
    bool done = false;
    bool cancelled = false;
    bool cancellation_requested = false;
};

inline void retain_process_control(ProcessControl* control) noexcept {
    if (control) ++control->references;
}

inline void release_process_control(ProcessControl* control) noexcept {
    if (control && --control->references == 0) delete control;
}

struct TimerRegistration {
    uint64_t deadline = 0;
    uint64_t sequence = 0;
    WaitRegistration wait;
};

struct TimerRegistrationLater {
    bool operator()(const TimerRegistration& left,
                    const TimerRegistration& right) const {
        if (left.deadline != right.deadline) {
            return left.deadline > right.deadline;
        }
        return left.sequence > right.sequence;
    }
};

class Scheduler {
   public:
    explicit Scheduler(SimTime precision = 1_ns)
        : femtoseconds_per_tick_(precision.femtoseconds) {
        if (femtoseconds_per_tick_ == 0) {
            std::fprintf(stderr, "cpptb: simulation precision cannot be zero\n");
            std::abort();
        }
    }

    ~Scheduler() {
        shutting_down_ = true;

        std::vector<std::coroutine_handle<>> handles;
        handles.reserve(states_.size());
        for (size_t state_index = 0; state_index < states_.size(); ++state_index) {
            auto& state = states_[state_index];
            end_wait(state);
            if (state.handle) {
                state.promise->scheduler = nullptr;
                handles.push_back(state.handle);
            }
            state = CoroutineState{};
            if (state_index < process_by_state_.size()) {
                process_by_state_[state_index] = nullptr;
            }
        }

        for (auto*& control : process_controls_) {
            if (!control) continue;
            control->scheduler = nullptr;
            if (!control->done) {
                control->done = true;
                control->cancelled = true;
                control->cancellation_requested = false;
            }
            control->state_index = std::numeric_limits<size_t>::max();
            control->scheduler_slot = std::numeric_limits<size_t>::max();
            release_process_control(std::exchange(control, nullptr));
        }

        for (auto handle : handles) {
            handle.destroy();
        }
    }

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    Process spawn(Task<void>&& task) {
        if (shutting_down_) return {};
        auto handle = task.release();
        if (!handle) return {};

        auto& promise = handle.promise();
        promise.scheduler = this;
        promise.continuation = nullptr;
        const size_t state_index = adopt(handle, promise);
        auto* control = new ProcessControl{};
        control->scheduler = this;
        control->state_index = state_index;
        if (free_process_slots_.empty()) {
            control->scheduler_slot = process_controls_.size();
            process_controls_.push_back(control);
        } else {
            control->scheduler_slot = free_process_slots_.back();
            free_process_slots_.pop_back();
            process_controls_[control->scheduler_slot] = control;
        }
        if (state_index >= process_by_state_.size()) {
            process_by_state_.resize(state_index + 1, nullptr);
        }
        process_by_state_[state_index] = control;
        Process process{control};
        make_ready(state_index);
        drain_ready();
        return process;
    }

    void spawn_detached(Task<void>&& task) {
        if (shutting_down_) return;
        auto handle = task.release();
        if (!handle) return;

        auto& promise = handle.promise();
        promise.scheduler = this;
        promise.continuation = nullptr;
        const size_t state_index = adopt(handle, promise);
        make_ready(state_index);
        drain_ready();
    }

    size_t adopt(std::coroutine_handle<> handle, TaskPromiseBase& promise) {
        if (!handle) return std::numeric_limits<size_t>::max();

        size_t state_index;
        if (free_states_.empty()) {
            state_index = states_.size();
            states_.push_back(CoroutineState{});
        } else {
            state_index = free_states_.back();
            free_states_.pop_back();
        }
        states_[state_index] = CoroutineState{};
        states_[state_index].handle = handle;
        states_[state_index].promise = &promise;
        if (state_index >= edge_wait_indices_by_state_.size()) {
            edge_wait_indices_by_state_.resize(state_index + 1);
        }
        edge_wait_indices_by_state_[state_index].clear();
        if (state_index >= phase_wait_counts_by_state_.size()) {
            phase_wait_counts_by_state_.resize(state_index + 1);
        }
        phase_wait_counts_by_state_[state_index].fill(0);
        promise.state_index = state_index;
        ++active_coroutines_;
        return state_index;
    }

    void park(std::coroutine_handle<> handle, TaskPromiseBase& promise,
              WaitRequest request,
              size_t* winner = nullptr, size_t winner_index = 0) {
        auto& state = state_for(handle, promise);
        const uint64_t generation = begin_wait(state);
        register_wait(request, WaitRegistration{promise.state_index,
                                                generation, winner, winner_index});
    }

    void park_edge(std::coroutine_handle<> handle, TaskPromiseBase& promise,
                   uint32_t signal_id, EdgeKind edge) {
        auto& state = state_for(handle, promise);
        const uint64_t generation = begin_wait(state);
#ifdef CPPTB_CORO_WAIT_PATH_DIAGNOSTICS
        ++single_edge_park_counts_[static_cast<size_t>(edge)];
#endif
        register_edge_wait(
            signal_id, edge,
            WaitRegistration{promise.state_index, generation, nullptr, 0});
    }

    template <size_t TriggerCount>
    void park_first(std::coroutine_handle<> handle, TaskPromiseBase& promise,
                    const std::array<WaitRequest, TriggerCount>& requests,
                    size_t* winner) {
        auto& state = state_for(handle, promise);
        const uint64_t generation = begin_wait(state);
#ifdef CPPTB_CORO_WAIT_PATH_DIAGNOSTICS
        ++compound_wait_parks_;
#endif
        const size_t state_index = promise.state_index;
        for (size_t index = 0; index < TriggerCount; ++index) {
            register_wait(
                requests[index],
                WaitRegistration{state_index, generation, winner, index});
        }
    }

    template <size_t ChildCount>
    void start_join(std::coroutine_handle<> parent,
                    TaskPromiseBase& parent_promise,
                    std::array<Task<void>, ChildCount>&& children) {
        static_assert(ChildCount >= 2, "Join requires at least two tasks");
        auto& parent_state = state_for(parent, parent_promise);
        begin_wait(parent_state);

        auto join_state = std::make_shared<JoinState>();
        join_state->remaining = ChildCount;
        join_state->parent_index = parent_promise.state_index;

        for (auto& child_task : children) {
            auto child = child_task.release();
            if (!child) {
                std::fprintf(stderr, "cpptb: Join received an invalid task\n");
                std::abort();
            }
            child.promise().scheduler = this;
            child.promise().continuation = nullptr;
            child.promise().join_state = join_state;
            make_ready(adopt(child, child.promise()));
        }
    }

    template <typename T>
    void start_timeout(std::coroutine_handle<> parent,
                       TaskPromiseBase& parent_promise,
                       std::coroutine_handle<TaskPromise<T>> child,
                       SimTime timeout) {
        validate_delay(timeout);
        auto& parent_state = state_for(parent, parent_promise);
        const uint64_t generation = begin_wait(parent_state);

        auto join_state = std::make_shared<JoinState>();
        join_state->remaining = 1;
        join_state->parent_index = parent_promise.state_index;
        child.promise().scheduler = this;
        child.promise().continuation = nullptr;
        child.promise().join_state = std::move(join_state);
        adopt(child, child.promise());
        child.resume();

        const WaitRegistration timeout_wait{parent_promise.state_index,
                                            generation, nullptr, 0};
        if (registration_valid(timeout_wait)) {
            register_wait(WaitRequest::delay_for(timeout), timeout_wait);
        }
    }

    bool park_process(std::coroutine_handle<> handle, TaskPromiseBase& promise,
                      ProcessControl* control) {
        if (!control || control->scheduler != this) {
            std::fprintf(stderr, "cpptb: cannot await an invalid process\n");
            std::abort();
        }
        if (control->done) return false;

        auto& state = state_for(handle, promise);
        const uint64_t generation = begin_wait(state);
        control->completion_waiters.push_back(
            WaitRegistration{promise.state_index, generation, nullptr,
                             0});
        if (control->done) {
            control->completion_waiters.pop_back();
            end_wait(state);
            return false;
        }
        return true;
    }

    void finish(std::coroutine_handle<> handle,
                TaskPromiseBase& promise) noexcept {
        const size_t state_index = promise.state_index;
        if (!state_matches(state_index, handle)) return;
        auto& state = states_[state_index];
        end_wait(state);
        state.ready = false;
        state.done = true;
        if (active_coroutines_ != 0) --active_coroutines_;
        const bool child = promise.continuation != nullptr ||
                           promise.join_state != nullptr;
        if (child) {
            finished_states_.push_back(state_index);
        } else {
            if (state_index < process_by_state_.size() &&
                process_by_state_[state_index]) {
                complete_process(*process_by_state_[state_index], false);
            }
            finished_states_.push_back(state_index);
        }
    }

    void cancel_process(ProcessControl* control) {
        if (!control || control->scheduler != this || control->done) return;
        if (draining_) {
            if (!control->cancellation_requested) {
                control->cancellation_requested = true;
                pending_cancellations_.push_back(control);
            }
            return;
        }

        control->cancellation_requested = true;
        cancel_state(control->state_index);
        timer_schedule_changed_ = true;
        drain_ready();
    }

    void finish_join_child(const std::shared_ptr<JoinState>& join_state) noexcept {
        if (!join_state || join_state->remaining == 0) return;
        --join_state->remaining;
        if (join_state->remaining != 0) return;

        if (join_state->parent_index >= states_.size()) return;
        auto& parent_state = states_[join_state->parent_index];
        if (parent_state.done || !parent_state.waiting) return;
        end_wait(parent_state);
        make_ready(join_state->parent_index);
    }

    ExternalWaitRegistration park_external(std::coroutine_handle<> handle,
                                           TaskPromiseBase& promise) {
        auto& state = state_for(handle, promise);
        const uint64_t generation = begin_wait(state);
        return {this, lifetime_,
                WaitRegistration{promise.state_index, generation, nullptr, 0}};
    }

    bool external_wait_valid(const ExternalWaitRegistration& registration) const {
        return !registration.lifetime.expired() &&
               registration.scheduler == this &&
               registration_valid(registration.wait);
    }

    bool external_wait_alive(const ExternalWaitRegistration& registration) const {
        if (registration.lifetime.expired() || registration.scheduler != this) {
            return false;
        }
        const auto& wait = registration.wait;
        if (wait.state_index >= states_.size()) return false;
        const auto& state = states_[wait.state_index];
        return state.handle && !state.done &&
               state.wait_generation == wait.generation;
    }

    bool wake_external(const ExternalWaitRegistration& registration) {
        return external_wait_valid(registration) &&
               resume_registration(registration.wait);
    }

    void flush_external_wakes() { drain_ready(); }

    std::weak_ptr<SchedulerLifetime> lifetime() const { return lifetime_; }

    void reclaim_awaited(std::coroutine_handle<> handle,
                         TaskPromiseBase& promise) {
        const size_t state_index = promise.state_index;
        if (!state_matches(state_index, handle) || !states_[state_index].done) {
            std::fprintf(stderr, "cpptb: awaited task was not complete\n");
            std::abort();
        }

        for (auto& finished : finished_states_) {
            if (finished == state_index) {
                finished = std::numeric_limits<size_t>::max();
                break;
            }
        }
        states_[state_index] = CoroutineState{};
        free_states_.push_back(state_index);
        promise.scheduler = nullptr;
        handle.destroy();
    }

    bool awaited_done(std::coroutine_handle<> handle,
                      const TaskPromiseBase& promise) const {
        const size_t state_index = promise.state_index;
        return state_matches(state_index, handle) && states_[state_index].done;
    }

    void cancel_awaited(std::coroutine_handle<> handle,
                        TaskPromiseBase& promise) {
        const size_t state_index = promise.state_index;
        if (!state_matches(state_index, handle)) {
            std::fprintf(stderr, "cpptb: timed task is not scheduler-owned\n");
            std::abort();
        }
        if (states_[state_index].done) return;
        cancel_state(state_index);
        timer_schedule_changed_ = true;
    }

    void resume_edge(uint32_t signal_id, EdgeKind edge) {
        current_phase_ = SimulationPhase::Evaluation;
        const bool resumed_specific =
            resume_edge_queue(edge_queue_index(signal_id, edge));
        const bool resumed_any =
            edge == EdgeKind::Any
                ? false
                : resume_edge_queue(edge_queue_index(signal_id, EdgeKind::Any));
#ifdef CPPTB_CORO_WAIT_PATH_DIAGNOSTICS
        ++edge_notification_count_;
        if (resumed_specific || resumed_any) {
            ++edge_notification_resume_count_;
        }
#endif
        if (resumed_specific || resumed_any) {
            drain_ready();
        } else {
            scheduler_boundary();
        }
    }

    void advance_time(uint64_t time) {
        current_phase_ = SimulationPhase::Evaluation;
        now_ = time;
        if (!next_timer_ || next_timer_->deadline > now_) return;

        bool popped = false;
        bool resumed = false;
        while (true) {
            discard_stale_timers();
            if (!next_timer_ || next_timer_->deadline > now_) break;
            const auto registration = *next_timer_;
            next_timer_.reset();
            popped = true;
            resumed |= resume_registration(registration.wait);
        }
        if (popped) timer_schedule_changed_ = true;
        if (resumed) {
            drain_ready();
        } else {
            scheduler_boundary();
        }
    }

    void resume_phase(WaitKind kind) {
        const size_t index = phase_wait_index(kind);
        current_phase_ = simulation_phase(kind);
        auto& queue = phase_waiters_[index];
        const size_t ready_count = queue.size();
        bool resumed = false;
        for (size_t registration = 0; registration < ready_count;
             ++registration) {
            resumed |= resume_registration(queue[registration]);
        }
        queue.erase(queue.begin(), queue.begin() + ready_count);
        if (resumed) {
            drain_ready();
        } else {
            scheduler_boundary();
        }
    }

    bool has_phase_waiters(WaitKind kind) const {
        return phase_waiter_counts_[phase_wait_index(kind)] != 0;
    }

    SimulationPhase current_phase() const { return current_phase_; }

    bool has_falling_edge_waiters() const {
        return falling_edge_registrations_ != 0;
    }

    uint8_t edge_interest(uint32_t signal_id) const {
        if (signal_id >= edge_interest_counts_.size()) {
            return kEdgeInterestNone;
        }
        return interest_mask(edge_interest_counts_[signal_id]);
    }

    bool has_edge_interest(uint32_t signal_id, EdgeKind edge) const {
        if (signal_id >= edge_interest_counts_.size()) return false;
        const auto& counts = edge_interest_counts_[signal_id];
        if (edge == EdgeKind::Any) {
            return counts[static_cast<size_t>(EdgeKind::Any)] != 0;
        }
        return counts[static_cast<size_t>(edge)] != 0 ||
               counts[static_cast<size_t>(EdgeKind::Any)] != 0;
    }

    void configure_static_edge_source(uint32_t signal_id) {
        if (wait_registered_) {
            std::fprintf(stderr,
                         "cpptb: static edge source %u must be configured "
                         "before registering any waits\n",
                         signal_id);
            std::abort();
        }
        if (signal_id >= static_edge_sources_.size()) {
            static_edge_sources_.resize(signal_id + 1, false);
        }
        static_edge_sources_[signal_id] = true;
    }

    bool is_static_edge_source(uint32_t signal_id) const {
        return signal_id < static_edge_sources_.size() &&
               static_edge_sources_[signal_id];
    }

    uint64_t edge_interest_generation() const {
        return edge_interest_generation_;
    }

    std::optional<EdgeInterestChange> consume_edge_interest_change() {
        if (edge_interest_changes_.empty()) return std::nullopt;
        const uint32_t signal_id = edge_interest_changes_.front();
        edge_interest_changes_.pop_front();
        edge_interest_change_pending_[signal_id] = false;
        return EdgeInterestChange{signal_id, edge_interest(signal_id)};
    }

    bool consume_timer_schedule_changed() {
        const bool changed = timer_schedule_changed_;
        timer_schedule_changed_ = false;
        return changed;
    }

    uint64_t next_timer_deadline() {
        discard_stale_timers();
        if (!next_timer_) return std::numeric_limits<uint64_t>::max();
        return next_timer_->deadline;
    }

    uint64_t now() const { return now_; }

    SimTime now_time() const {
        const uint64_t max_time = std::numeric_limits<uint64_t>::max();
        if (now_ > max_time / femtoseconds_per_tick_) {
            return SimTime{max_time};
        }
        return SimTime{now_ * femtoseconds_per_tick_};
    }

    bool all_done() const { return active_coroutines_ == 0; }

#ifdef CPPTB_CORO_WAIT_PATH_DIAGNOSTICS
    uint64_t single_edge_park_count(EdgeKind edge) const {
        return single_edge_park_counts_[static_cast<size_t>(edge)];
    }

    uint64_t edge_notification_count() const {
        return edge_notification_count_;
    }

    uint64_t edge_notification_resume_count() const {
        return edge_notification_resume_count_;
    }

    uint64_t compound_wait_park_count() const { return compound_wait_parks_; }
#endif

   private:
    void resume_completion_waiters(ProcessControl& process) {
        for (const auto& registration : process.completion_waiters) {
            resume_registration(registration);
        }
        process.completion_waiters.clear();
    }

    void complete_process(ProcessControl& process, bool cancelled) {
        if (process.done) return;
        process.done = true;
        process.cancelled = cancelled;
        process.cancellation_requested = false;
        resume_completion_waiters(process);
    }

    void apply_pending_cancellations() {
        for (auto* process : pending_cancellations_) {
            if (!process || process->scheduler != this || process->done) continue;
            cancel_state(process->state_index);
            timer_schedule_changed_ = true;
        }
        pending_cancellations_.clear();
    }

    void cancel_state(size_t state_index) {
        if (state_index >= states_.size()) return;
        auto& state = states_[state_index];
        if (state.done) return;

        const auto parent_handle = state.handle;
        std::vector<size_t> children;
        for (size_t index = 0; index < states_.size(); ++index) {
            if (index == state_index) continue;
            const auto& candidate = states_[index];
            if (!candidate.handle || candidate.done) continue;
            const auto& promise = *candidate.promise;
            if (promise.continuation.address() == parent_handle.address() ||
                (promise.join_state &&
                 promise.join_state->parent_index == state_index)) {
                children.push_back(index);
            }
        }
        for (const size_t child_index : children) {
            cancel_state(child_index);
        }

        end_wait(state);
        state.wait_generation = ++next_wait_generation_;
        state.ready = false;
        state.done = true;
        if (active_coroutines_ != 0) --active_coroutines_;

        auto handle = state.handle;
        if (handle) {
            state.promise->continuation = nullptr;
            state.promise->join_state.reset();
        }
        if (state_index < process_by_state_.size() &&
            process_by_state_[state_index]) {
            complete_process(*process_by_state_[state_index], true);
        }
        finished_states_.push_back(state_index);
    }

    static size_t edge_queue_index(uint32_t signal_id, EdgeKind edge) {
        return static_cast<size_t>(signal_id) * 3 + static_cast<size_t>(edge);
    }

    static size_t phase_wait_index(WaitKind kind) {
        switch (kind) {
            case WaitKind::ReadWrite:
                return 0;
            case WaitKind::ReadOnly:
                return 1;
            case WaitKind::NextTimeStep:
                return 2;
            default:
                std::fprintf(stderr,
                             "cpptb: wait kind is not a simulator phase\n");
                std::abort();
        }
    }

    static SimulationPhase simulation_phase(WaitKind kind) {
        switch (kind) {
            case WaitKind::ReadWrite:
                return SimulationPhase::ReadWrite;
            case WaitKind::ReadOnly:
                return SimulationPhase::ReadOnly;
            case WaitKind::NextTimeStep:
                return SimulationPhase::NextTimeStep;
            default:
                std::fprintf(stderr,
                             "cpptb: wait kind is not a simulator phase\n");
                std::abort();
        }
    }

    static uint8_t interest_mask(const std::array<uint32_t, 3>& counts) {
        uint8_t mask = kEdgeInterestNone;
        if (counts[static_cast<size_t>(EdgeKind::Rising)] != 0 ||
            counts[static_cast<size_t>(EdgeKind::Any)] != 0) {
            mask |= kEdgeInterestRising;
        }
        if (counts[static_cast<size_t>(EdgeKind::Falling)] != 0 ||
            counts[static_cast<size_t>(EdgeKind::Any)] != 0) {
            mask |= kEdgeInterestFalling;
        }
        return mask;
    }

    void record_edge_interest_change(uint32_t signal_id, uint8_t previous) {
        const uint8_t current = edge_interest(signal_id);
        if (current == previous) return;
        ++edge_interest_generation_;
        if (!edge_interest_change_pending_[signal_id]) {
            edge_interest_change_pending_[signal_id] = true;
            edge_interest_changes_.push_back(signal_id);
        }
    }

    void add_edge_interest(size_t state_index, size_t queue_index) {
        const uint32_t signal_id = static_cast<uint32_t>(queue_index / 3);
        if (is_static_edge_source(signal_id)) return;
        const size_t edge_index = queue_index % 3;
        if (signal_id >= edge_interest_counts_.size()) {
            edge_interest_counts_.resize(signal_id + 1);
            edge_interest_change_pending_.resize(signal_id + 1, false);
        }
        if (state_index >= edge_wait_indices_by_state_.size()) {
            edge_wait_indices_by_state_.resize(state_index + 1);
        }
        const uint8_t previous = edge_interest(signal_id);
        ++edge_interest_counts_[signal_id][edge_index];
        edge_wait_indices_by_state_[state_index].push_back(queue_index);
        record_edge_interest_change(signal_id, previous);
    }

    void remove_edge_interests(size_t state_index) {
        if (state_index >= edge_wait_indices_by_state_.size()) return;
        for (const size_t queue_index : edge_wait_indices_by_state_[state_index]) {
            const uint32_t signal_id = static_cast<uint32_t>(queue_index / 3);
            const size_t edge_index = queue_index % 3;
            const uint8_t previous = edge_interest(signal_id);
            auto& count = edge_interest_counts_[signal_id][edge_index];
            if (count == 0) {
                std::fprintf(stderr,
                             "cpptb: edge interest bookkeeping underflow\n");
                std::abort();
            }
            --count;
            record_edge_interest_change(signal_id, previous);
        }
        edge_wait_indices_by_state_[state_index].clear();
    }

    uint64_t begin_wait(CoroutineState& state) {
        if (state.waiting) end_wait(state);
        state.waiting = true;
        state.ready = false;
        state.wait_generation = ++next_wait_generation_;
        return state.wait_generation;
    }

    void end_wait(CoroutineState& state) {
        if (!state.waiting) return;
        state.waiting = false;
        const size_t state_index = static_cast<size_t>(&state - states_.data());
        remove_edge_interests(state_index);
        if (state.edge_wait_count != 0) {
            stale_edge_registrations_ += state.edge_wait_count;
            state.edge_wait_count = 0;
        }
        if (state.falling_edge_wait_count != 0) {
            falling_edge_registrations_ -= state.falling_edge_wait_count;
            state.falling_edge_wait_count = 0;
        }
        auto& phase_counts = phase_wait_counts_by_state_[state_index];
        for (size_t phase = 0; phase < phase_counts.size(); ++phase) {
            if (phase_counts[phase] >
                phase_waiter_counts_[phase]) {
                std::fprintf(stderr,
                             "cpptb: phase interest bookkeeping underflow\n");
                std::abort();
            }
            phase_waiter_counts_[phase] -= phase_counts[phase];
            phase_counts[phase] = 0;
        }
    }

    void register_edge_wait(uint32_t signal_id, EdgeKind edge,
                            WaitRegistration registration) {
        wait_registered_ = true;
        const size_t queue_index = edge_queue_index(signal_id, edge);
        if (queue_index >= edge_waiters_.size()) {
            edge_waiters_.resize(queue_index + 1);
        }
        edge_waiters_[queue_index].push_back(registration);
        ++edge_queue_entries_;
        auto& state = states_[registration.state_index];
        add_edge_interest(registration.state_index, queue_index);
        ++state.edge_wait_count;
        if (edge == EdgeKind::Falling || edge == EdgeKind::Any) {
            ++falling_edge_registrations_;
            ++state.falling_edge_wait_count;
        }
    }

    void register_wait(WaitRequest request, WaitRegistration registration) {
        wait_registered_ = true;
        switch (request.kind) {
            case WaitKind::Edge:
                register_edge_wait(request.signal_id, request.edge,
                                   registration);
                break;
            case WaitKind::Delay: {
                validate_delay(request.delay);
                const uint64_t delay_ticks =
                    request.delay.femtoseconds / femtoseconds_per_tick_;
                const uint64_t max_time = std::numeric_limits<uint64_t>::max();
                const uint64_t deadline =
                    delay_ticks > max_time - now_ ? max_time
                                                  : now_ + delay_ticks;
                const uint64_t previous_deadline = next_timer_deadline();
                push_timer(TimerRegistration{
                    deadline, next_timer_sequence_++, registration});
                if (deadline < previous_deadline) timer_schedule_changed_ = true;
                break;
            }
            case WaitKind::ReadWrite:
            case WaitKind::ReadOnly:
            case WaitKind::NextTimeStep: {
                if (current_phase_ == SimulationPhase::ReadOnly &&
                    request.kind != WaitKind::NextTimeStep) {
                    const char* trigger =
                        request.kind == WaitKind::ReadWrite ? "ReadWrite"
                                                           : "ReadOnly";
                    std::fprintf(
                        stderr,
                        "cpptb: cannot await %s after ReadOnly in the same "
                        "timestep; await NextTimeStep{}, Delay{...}, or an "
                        "edge first\n",
                        trigger);
                    std::abort();
                }
                const size_t phase = phase_wait_index(request.kind);
                phase_waiters_[phase].push_back(registration);
                ++phase_waiter_counts_[phase];
                ++phase_wait_counts_by_state_[registration.state_index][phase];
                break;
            }
        }
    }

    void validate_delay(SimTime delay) const {
        if (delay.femtoseconds == 0) {
            std::fprintf(stderr,
                         "cpptb: delay duration must be greater than zero\n");
            std::abort();
        }
        if ((delay.femtoseconds % femtoseconds_per_tick_) != 0) {
            std::fprintf(
                stderr,
                "cpptb: delay of %llu fs is not representable at "
                "%llu fs simulation precision\n",
                static_cast<unsigned long long>(delay.femtoseconds),
                static_cast<unsigned long long>(femtoseconds_per_tick_));
            std::abort();
        }
    }

    bool registration_valid(const WaitRegistration& registration) const {
        if (registration.state_index >= states_.size()) return false;
        const auto& state = states_[registration.state_index];
        return state.handle && !state.done && state.waiting &&
               state.wait_generation == registration.generation;
    }

    bool resume_registration(const WaitRegistration& registration) {
        if (!registration_valid(registration)) return false;
        auto& state = states_[registration.state_index];
        end_wait(state);
        if (registration.winner) {
            *registration.winner = registration.winner_index;
        }
        make_ready(registration.state_index);
        return true;
    }

    bool resume_edge_queue(size_t queue_index) {
        if (queue_index >= edge_waiters_.size()) return false;
        auto& queue = edge_waiters_[queue_index];
        const size_t ready_count = queue.size();
        bool resumed = false;
        for (size_t index = 0; index < ready_count; ++index) {
            resumed |= resume_registration(queue[index]);
            --edge_queue_entries_;
            if (stale_edge_registrations_ != 0) --stale_edge_registrations_;
        }
        queue.clear();
        return resumed;
    }

    void compact_stale_edge_queues() {
        constexpr size_t compact_threshold = 64;
        if (stale_edge_registrations_ < compact_threshold ||
            stale_edge_registrations_ * 2 < edge_queue_entries_) {
            return;
        }

        size_t removed = 0;
        for (auto& queue : edge_waiters_) {
            const auto old_size = queue.size();
            auto output = queue.begin();
            for (const auto& registration : queue) {
                if (registration_valid(registration)) {
                    *output++ = registration;
                }
            }
            queue.erase(output, queue.end());
            removed += old_size - queue.size();
        }
        edge_queue_entries_ -= removed;
        stale_edge_registrations_ -= removed;
    }

    void discard_stale_timers() {
        while (true) {
            if (next_timer_ && registration_valid(next_timer_->wait)) return;
            if (next_timer_) {
                next_timer_.reset();
                timer_schedule_changed_ = true;
            }
            if (timer_waiters_.empty()) return;
            next_timer_ = timer_waiters_.top();
            timer_waiters_.pop();
        }
    }

    void push_timer(TimerRegistration registration) {
        if (!next_timer_) {
            next_timer_ = registration;
            return;
        }

        TimerRegistrationLater later;
        if (later(*next_timer_, registration)) {
            timer_waiters_.push(*next_timer_);
            next_timer_ = registration;
        } else {
            timer_waiters_.push(registration);
        }
    }

    void make_ready(size_t state_index) {
        if (state_index >= states_.size()) return;
        auto& state = states_[state_index];
        if (state.done || state.ready) return;
        state.ready = true;
        ready_.push_back(state_index);
    }

    void drain_ready() {
        if (draining_) return;
        draining_ = true;
        drain_ready_queue();
        ready_.clear();
        draining_ = false;
        scheduler_boundary();
    }

    void drain_ready_queue() {
        for (size_t ready_index = 0; ready_index < ready_.size(); ++ready_index) {
            const size_t state_index = ready_[ready_index];
            if (state_index >= states_.size()) continue;
            auto& state = states_[state_index];
            if (!state.handle || state.done || !state.ready) continue;
            state.ready = false;
            state.handle.resume();
            apply_pending_cancellations();
        }
    }

    CoroutineState& state_for(std::coroutine_handle<> handle,
                              TaskPromiseBase& promise) {
        const size_t state_index = promise.state_index;
        if (!state_matches(state_index, handle) ||
            states_[state_index].promise != &promise) {
            std::fprintf(stderr, "cpptb: coroutine was not registered with scheduler\n");
            std::abort();
        }
        return states_[state_index];
    }

    bool state_matches(size_t state_index,
                       std::coroutine_handle<> handle) const {
        return state_index < states_.size() &&
               states_[state_index].handle.address() == handle.address();
    }

    void scheduler_boundary() {
        collect_finished_states();
        compact_stale_edge_queues();
    }

    void collect_finished_states() {
        if (reclaiming_) return;
        reclaiming_ = true;
        for (size_t finished_index = 0;
             finished_index < finished_states_.size(); ++finished_index) {
            const size_t state_index = finished_states_[finished_index];
            if (state_index >= states_.size()) continue;
            auto& state = states_[state_index];
            if (!state.handle || !state.done) continue;

            auto handle = state.handle;
            auto* promise = state.promise;
            ProcessControl* control = nullptr;
            if (state_index < process_by_state_.size()) {
                control = process_by_state_[state_index];
                process_by_state_[state_index] = nullptr;
            }
            state = CoroutineState{};
            free_states_.push_back(state_index);

            if (control) {
                control->state_index = std::numeric_limits<size_t>::max();
                control->scheduler = nullptr;
                const size_t slot = control->scheduler_slot;
                control->scheduler_slot = std::numeric_limits<size_t>::max();
                if (slot < process_controls_.size() &&
                    process_controls_[slot] == control) {
                    process_controls_[slot] = nullptr;
                    free_process_slots_.push_back(slot);
                }
                release_process_control(control);
            }

            promise->scheduler = nullptr;
            handle.destroy();
        }
        finished_states_.clear();
        reclaiming_ = false;
    }

    std::vector<CoroutineState> states_;
    std::vector<ProcessControl*> process_controls_;
    std::vector<ProcessControl*> process_by_state_;
    std::vector<ProcessControl*> pending_cancellations_;
    std::vector<size_t> free_states_;
    std::vector<size_t> free_process_slots_;
    std::vector<size_t> finished_states_;
    std::vector<std::vector<WaitRegistration>> edge_waiters_;
    std::array<std::vector<WaitRegistration>, 3> phase_waiters_;
    std::array<size_t, 3> phase_waiter_counts_{};
    std::vector<std::array<uint32_t, 3>> phase_wait_counts_by_state_;
    std::vector<std::vector<size_t>> edge_wait_indices_by_state_;
    std::vector<std::array<uint32_t, 3>> edge_interest_counts_;
    std::vector<bool> edge_interest_change_pending_;
    std::vector<bool> static_edge_sources_;
    std::deque<uint32_t> edge_interest_changes_;
    std::priority_queue<TimerRegistration, std::vector<TimerRegistration>,
                        TimerRegistrationLater>
        timer_waiters_;
    std::optional<TimerRegistration> next_timer_;
    std::vector<size_t> ready_;
    uint64_t now_ = 0;
    uint64_t next_wait_generation_ = 0;
    uint64_t next_timer_sequence_ = 0;
    size_t active_coroutines_ = 0;
    size_t falling_edge_registrations_ = 0;
    size_t edge_queue_entries_ = 0;
    size_t stale_edge_registrations_ = 0;
    uint64_t edge_interest_generation_ = 0;
    uint64_t femtoseconds_per_tick_ = 1'000'000u;
    SimulationPhase current_phase_ = SimulationPhase::Evaluation;
#ifdef CPPTB_CORO_WAIT_PATH_DIAGNOSTICS
    std::array<uint64_t, 3> single_edge_park_counts_{};
    uint64_t compound_wait_parks_ = 0;
    uint64_t edge_notification_count_ = 0;
    uint64_t edge_notification_resume_count_ = 0;
#endif
    bool timer_schedule_changed_ = false;
    bool draining_ = false;
    bool reclaiming_ = false;
    bool shutting_down_ = false;
    bool wait_registered_ = false;
    std::shared_ptr<SchedulerLifetime> lifetime_ =
        std::make_shared<SchedulerLifetime>();
};

inline Process::Process(ProcessControl* control) noexcept : control_(control) {
    retain_process_control(control_);
}

inline Process::Process(const Process& other) noexcept : control_(other.control_) {
    retain_process_control(control_);
}

inline Process& Process::operator=(const Process& other) noexcept {
    if (this == &other) return *this;
    retain_process_control(other.control_);
    release_process_control(control_);
    control_ = other.control_;
    return *this;
}

inline Process::Process(Process&& other) noexcept
    : control_(std::exchange(other.control_, nullptr)) {}

inline Process& Process::operator=(Process&& other) noexcept {
    if (this == &other) return *this;
    release_process_control(control_);
    control_ = std::exchange(other.control_, nullptr);
    return *this;
}

inline Process::~Process() { release_process_control(control_); }

inline bool Process::valid() const { return control_ != nullptr; }

inline bool Process::done() const {
    return control_ && control_->done;
}

inline bool Process::cancelled() const {
    return control_ && control_->done && control_->cancelled;
}

inline void Process::cancel() const {
    if (control_ && control_->scheduler) {
        control_->scheduler->cancel_process(control_);
    }
}

struct Process::Awaiter {
    Process process;

    bool await_ready() const {
        if (!process.valid()) {
            std::fprintf(stderr, "cpptb: cannot await an invalid process\n");
            std::abort();
        }
        return process.done();
    }

    template <typename Promise>
        requires std::derived_from<Promise, TaskPromiseBase>
    bool await_suspend(std::coroutine_handle<Promise> handle) const {
        auto& promise = static_cast<TaskPromiseBase&>(handle.promise());
        auto* scheduler = promise.scheduler;
        if (!scheduler) return false;
        return scheduler->park_process(handle, promise, process.control_);
    }

    void await_resume() const noexcept {}
};

inline Process::Awaiter Process::operator co_await() const {
    return Awaiter{*this};
}

template <typename Promise>
std::coroutine_handle<> TaskPromiseBase::FinalAwaiter::await_suspend(
    std::coroutine_handle<Promise> handle) const noexcept {
    auto& promise = static_cast<TaskPromiseBase&>(handle.promise());
    auto* scheduler = promise.scheduler;
    if (scheduler) scheduler->finish(handle, promise);

    if (promise.join_state) {
        if (scheduler) scheduler->finish_join_child(promise.join_state);
        return std::noop_coroutine();
    }

    auto continuation = promise.continuation;
    if (continuation) return continuation;
    return std::noop_coroutine();
}

template <typename T>
struct Task<T>::Awaiter {
    explicit Awaiter(Task&& task) : child_(task.release()) {}

    bool await_ready() const noexcept { return false; }

    template <typename ParentPromise>
        requires std::derived_from<ParentPromise, TaskPromiseBase>
    std::coroutine_handle<> await_suspend(
        std::coroutine_handle<ParentPromise> parent) {
        auto& parent_promise =
            static_cast<TaskPromiseBase&>(parent.promise());
        auto* scheduler = parent_promise.scheduler;
        if (!scheduler) {
            std::fprintf(stderr, "cpptb: cannot await a task without a scheduler\n");
            std::abort();
        }

        if (!child_) return parent;

        child_.promise().scheduler = scheduler;
        child_.promise().continuation = parent;
        scheduler->adopt(child_, child_.promise());
        return child_;
    }

    decltype(auto) await_resume() {
        if (!child_) {
            std::fprintf(stderr, "cpptb: cannot await an invalid task\n");
            std::abort();
        }

        auto& promise = child_.promise();
        auto* scheduler = promise.scheduler;
        if constexpr (std::is_void_v<T>) {
            scheduler->reclaim_awaited(child_, promise);
            child_ = nullptr;
            return;
        } else {
            if (!promise.result) {
                std::fprintf(stderr, "cpptb: completed task has no result\n");
                std::abort();
            }
            T result = std::move(*promise.result);
            scheduler->reclaim_awaited(child_, promise);
            child_ = nullptr;
            return result;
        }
    }

   private:
    handle_type child_ = nullptr;
};

template <typename T>
typename Task<T>::Awaiter Task<T>::operator co_await() && {
    return Awaiter{std::move(*this)};
}

template <typename Trigger>
struct BasicTriggerAwaiter {
    Trigger trigger;

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
        requires std::derived_from<Promise, TaskPromiseBase>
    void await_suspend(std::coroutine_handle<Promise> handle) const {
        auto& promise = static_cast<TaskPromiseBase&>(handle.promise());
        auto* scheduler = promise.scheduler;
        if (!scheduler) {
            std::fprintf(stderr, "cpptb: cannot wait without a scheduler\n");
            std::abort();
        }
        scheduler->park(handle, promise, wait_request(trigger));
    }

    void await_resume() const noexcept {}
};

constexpr EdgeKind basic_trigger_edge_kind(RisingEdge) {
    return EdgeKind::Rising;
}

constexpr EdgeKind basic_trigger_edge_kind(FallingEdge) {
    return EdgeKind::Falling;
}

constexpr EdgeKind basic_trigger_edge_kind(Edge) { return EdgeKind::Any; }

template <typename Trigger>
    requires(std::same_as<Trigger, RisingEdge> ||
             std::same_as<Trigger, FallingEdge> ||
             std::same_as<Trigger, Edge>)
struct BasicTriggerAwaiter<Trigger> {
    Trigger trigger;

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
        requires std::derived_from<Promise, TaskPromiseBase>
    void await_suspend(std::coroutine_handle<Promise> handle) const {
        auto& promise = static_cast<TaskPromiseBase&>(handle.promise());
        auto* scheduler = promise.scheduler;
        if (!scheduler) {
            std::fprintf(stderr, "cpptb: cannot wait without a scheduler\n");
            std::abort();
        }
        scheduler->park_edge(handle, promise, trigger.signal.id,
                             basic_trigger_edge_kind(trigger));
    }

    void await_resume() const noexcept {}
};

inline BasicTriggerAwaiter<RisingEdge> operator co_await(RisingEdge trigger) {
    return {trigger};
}

inline BasicTriggerAwaiter<FallingEdge> operator co_await(FallingEdge trigger) {
    return {trigger};
}

inline BasicTriggerAwaiter<Edge> operator co_await(Edge trigger) {
    return {trigger};
}

inline BasicTriggerAwaiter<Delay> operator co_await(Delay trigger) {
    return {trigger};
}

inline BasicTriggerAwaiter<ReadWrite> operator co_await(ReadWrite trigger) {
    return {trigger};
}

inline BasicTriggerAwaiter<ReadOnly> operator co_await(ReadOnly trigger) {
    return {trigger};
}

inline BasicTriggerAwaiter<NextTimeStep> operator co_await(
    NextTimeStep trigger) {
    return {trigger};
}

template <typename... Triggers>
struct FirstAwaiter {
    First<Triggers...> trigger;
    size_t winner = std::numeric_limits<size_t>::max();

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
        requires std::derived_from<Promise, TaskPromiseBase>
    void await_suspend(std::coroutine_handle<Promise> handle) {
        auto& promise = static_cast<TaskPromiseBase&>(handle.promise());
        auto* scheduler = promise.scheduler;
        if (!scheduler) {
            std::fprintf(stderr, "cpptb: cannot wait on First without a scheduler\n");
            std::abort();
        }
        const auto requests = std::apply(
            [](const auto&... values) {
                return std::array<WaitRequest, sizeof...(Triggers)>{
                    wait_request(values)...};
            },
            trigger.triggers);
        scheduler->park_first(handle, promise, requests, &winner);
    }

    size_t await_resume() const noexcept { return winner; }
};

template <typename... Triggers>
FirstAwaiter<Triggers...> operator co_await(First<Triggers...> trigger) {
    return {std::move(trigger)};
}

template <size_t ChildCount>
struct Join {
    static_assert(ChildCount >= 2, "Join requires at least two tasks");

    std::array<Task<void>, ChildCount> children;

    template <typename... Children>
        requires(sizeof...(Children) == ChildCount &&
                 (std::same_as<std::remove_cvref_t<Children>, Task<void>> && ...))
    explicit Join(Children&&... values)
        : children{std::forward<Children>(values)...} {}

    Join(const Join&) = delete;
    Join& operator=(const Join&) = delete;
    Join(Join&&) = default;
    Join& operator=(Join&&) = default;
};

template <typename... Children>
Join(Children&&...) -> Join<sizeof...(Children)>;

template <size_t ChildCount>
struct JoinAwaiter {
    Join<ChildCount> trigger;

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
        requires std::derived_from<Promise, TaskPromiseBase>
    void await_suspend(std::coroutine_handle<Promise> handle) {
        auto& promise = static_cast<TaskPromiseBase&>(handle.promise());
        auto* scheduler = promise.scheduler;
        if (!scheduler) {
            std::fprintf(stderr, "cpptb: cannot wait on Join without a scheduler\n");
            std::abort();
        }
        scheduler->start_join(handle, promise, std::move(trigger.children));
    }

    void await_resume() const noexcept {}
};

template <size_t ChildCount>
JoinAwaiter<ChildCount> operator co_await(Join<ChildCount> trigger) {
    return JoinAwaiter<ChildCount>{std::move(trigger)};
}

inline Task<void> clock_cycles(Signal clock, uint64_t count) {
    for (uint64_t cycle = 0; cycle < count; ++cycle) {
        co_await RisingEdge{clock};
    }
}

enum class TimeoutOutcome : uint8_t {
    Triggered,
    TimedOut,
};

template <typename T>
class TimeoutResult {
   public:
    TimeoutResult() noexcept = default;
    explicit TimeoutResult(T value) : value_(std::move(value)) {}

    bool has_value() const noexcept { return value_.has_value(); }
    bool triggered() const noexcept { return has_value(); }
    bool completed() const noexcept { return has_value(); }
    bool timed_out() const noexcept { return !has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & {
        require_value();
        return *value_;
    }

    const T& value() const& {
        require_value();
        return *value_;
    }

    T&& value() && {
        require_value();
        return std::move(*value_);
    }

    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(*this).value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

   private:
    void require_value() const {
        if (value_) return;
        std::fprintf(stderr, "cpptb: timed-out task has no value\n");
        std::abort();
    }

    std::optional<T> value_;
};

template <>
class TimeoutResult<void> {
   public:
    TimeoutResult() noexcept = default;

    static TimeoutResult completed_result() noexcept {
        return TimeoutResult{true};
    }

    bool has_value() const noexcept { return completed_; }
    bool triggered() const noexcept { return completed_; }
    bool completed() const noexcept { return completed_; }
    bool timed_out() const noexcept { return !completed_; }
    explicit operator bool() const noexcept { return completed_; }

    void value() const {
        if (completed_) return;
        std::fprintf(stderr, "cpptb: timed-out task has no value\n");
        std::abort();
    }

   private:
    explicit TimeoutResult(bool completed) noexcept : completed_(completed) {}

    bool completed_ = false;
};

template <typename Trigger>
concept EdgeTrigger =
    std::same_as<std::remove_cvref_t<Trigger>, RisingEdge> ||
    std::same_as<std::remove_cvref_t<Trigger>, FallingEdge> ||
    std::same_as<std::remove_cvref_t<Trigger>, Edge>;

template <EdgeTrigger Trigger>
Task<TimeoutOutcome> with_timeout(Trigger trigger, SimTime timeout) {
    const size_t winner = co_await First{std::move(trigger), Delay{timeout}};
    co_return winner == 0 ? TimeoutOutcome::Triggered
                          : TimeoutOutcome::TimedOut;
}

template <typename T>
struct TaskTimeoutAwaiter {
    using handle_type = typename Task<T>::handle_type;

    TaskTimeoutAwaiter(Task<T>&& task, SimTime duration)
        : child(task.release()), timeout(duration) {}

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
        requires std::derived_from<Promise, TaskPromiseBase>
    void await_suspend(std::coroutine_handle<Promise> handle) {
        if (!child) {
            std::fprintf(stderr,
                         "cpptb: with_timeout received an invalid task\n");
            std::abort();
        }

        auto& promise = static_cast<TaskPromiseBase&>(handle.promise());
        auto* scheduler = promise.scheduler;
        if (!scheduler) {
            std::fprintf(
                stderr,
                "cpptb: cannot wait on a timed task without a scheduler\n");
            std::abort();
        }
        scheduler->start_timeout(handle, promise, child, timeout);
    }

    TimeoutResult<T> await_resume() {
        if (!child) {
            std::fprintf(stderr,
                         "cpptb: with_timeout received an invalid task\n");
            std::abort();
        }

        auto& promise = child.promise();
        auto* scheduler = promise.scheduler;
        if (!scheduler) {
            std::fprintf(stderr, "cpptb: timed task lost its scheduler\n");
            std::abort();
        }

        if constexpr (std::is_void_v<T>) {
            if (scheduler->awaited_done(child, promise)) {
                scheduler->reclaim_awaited(child, promise);
                child = nullptr;
                return TimeoutResult<void>::completed_result();
            }
        } else {
            if (promise.result) {
                T result = std::move(*promise.result);
                scheduler->reclaim_awaited(child, promise);
                child = nullptr;
                return TimeoutResult<T>{std::move(result)};
            }
        }

        scheduler->cancel_awaited(child, promise);
        child = nullptr;
        return {};
    }

    handle_type child = nullptr;
    SimTime timeout;
};

template <typename T>
Task<TimeoutResult<T>> with_timeout(Task<T> task, SimTime timeout) {
    co_return co_await TaskTimeoutAwaiter<T>{std::move(task), timeout};
}

template <typename SignalType, typename Predicate>
    requires std::invocable<Predicate&, uint32_t> &&
             requires(SignalType signal) {
                 { signal.get() } -> std::convertible_to<uint32_t>;
             }
Task<void> wait_until(SignalType signal, Predicate predicate, Signal clock) {
    while (!static_cast<bool>(predicate(signal.get()))) {
        co_await RisingEdge{clock};
    }
}

class Event {
   public:
    Event() = default;
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    ~Event() {
        cleanup();
        if (!waiters_.empty()) {
            std::fprintf(stderr,
                         "cpptb: Event destroyed with active waiters\n");
            std::abort();
        }
    }

    bool is_set() const noexcept { return set_; }

    void set() {
        set_ = true;
        cleanup();
        if (waiters_.empty()) return;

        auto* scheduler = scheduler_;
        for (const auto& waiter : waiters_) {
            scheduler->wake_external(waiter);
        }
        waiters_.clear();
        reset_binding();
        scheduler->flush_external_wakes();
    }

    void clear() {
        set_ = false;
        cleanup();
    }

    struct Awaiter {
        Event* event;

        bool await_ready() const noexcept { return event->is_set(); }

        template <typename Promise>
            requires std::derived_from<Promise, TaskPromiseBase>
        bool await_suspend(std::coroutine_handle<Promise> handle) {
            auto& promise = static_cast<TaskPromiseBase&>(handle.promise());
            if (!promise.scheduler) {
                std::fprintf(stderr,
                             "cpptb: cannot wait on Event without a scheduler\n");
                std::abort();
            }
            return event->suspend(handle, promise, *promise.scheduler);
        }

        void await_resume() const noexcept {}
    };

    Awaiter wait() { return Awaiter{this}; }
    Awaiter operator co_await() { return wait(); }

   private:
    bool suspend(std::coroutine_handle<> handle, TaskPromiseBase& promise,
                 Scheduler& scheduler) {
        cleanup();
        if (set_) return false;
        bind(scheduler);
        waiters_.push_back(scheduler.park_external(handle, promise));
        return true;
    }

    void bind(Scheduler& scheduler) {
        if (scheduler_ && scheduler_ != &scheduler) {
            std::fprintf(stderr,
                         "cpptb: Event cannot have waiters from multiple schedulers\n");
            std::abort();
        }
        scheduler_ = &scheduler;
        lifetime_ = scheduler.lifetime();
    }

    void cleanup() {
        auto output = waiters_.begin();
        for (const auto& waiter : waiters_) {
            if (!waiter.lifetime.expired() && waiter.scheduler &&
                waiter.scheduler->external_wait_alive(waiter)) {
                *output++ = waiter;
            }
        }
        waiters_.erase(output, waiters_.end());
        if (waiters_.empty()) reset_binding();
    }

    void reset_binding() {
        scheduler_ = nullptr;
        lifetime_.reset();
    }

    std::deque<ExternalWaitRegistration> waiters_;
    Scheduler* scheduler_ = nullptr;
    std::weak_ptr<SchedulerLifetime> lifetime_;
    bool set_ = false;
};

template <typename T>
class Channel {
    static_assert(std::move_constructible<T>,
                  "Channel values must be move constructible");

   public:
    Channel() = default;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    ~Channel() {
        cleanup();
        if (!waiters_.empty()) {
            std::fprintf(stderr,
                         "cpptb: Channel destroyed with active waiters\n");
            std::abort();
        }
    }

    size_t size() {
        cleanup();
        return items_.size();
    }

    bool empty() { return size() == 0; }

    void put_nowait(T value) {
        cleanup();
        items_.push_back(std::move(value));
        wake_available();
    }

    Task<void> put(T value) {
        put_nowait(std::move(value));
        co_return;
    }

    struct GetAwaiter {
        explicit GetAwaiter(Channel& owner) : channel(&owner) {}
        GetAwaiter(const GetAwaiter&) = delete;
        GetAwaiter& operator=(const GetAwaiter&) = delete;

        GetAwaiter(GetAwaiter&& other) noexcept
            : channel(other.channel), registration(std::move(other.registration)),
              active(std::exchange(other.active, false)) {}

        ~GetAwaiter() {
            if (active && registration) channel->abandon(*registration);
        }

        bool await_ready() { return channel->has_unreserved_item(); }

        template <typename Promise>
            requires std::derived_from<Promise, TaskPromiseBase>
        bool await_suspend(std::coroutine_handle<Promise> handle) {
            auto& promise = static_cast<TaskPromiseBase&>(handle.promise());
            if (!promise.scheduler) {
                std::fprintf(stderr,
                             "cpptb: cannot get from Channel without a scheduler\n");
                std::abort();
            }
            return channel->suspend(handle, promise, *promise.scheduler,
                                    registration);
        }

        T await_resume() {
            T value = channel->consume(registration);
            active = false;
            return value;
        }

        Channel* channel;
        std::optional<ExternalWaitRegistration> registration;
        bool active = true;
    };

    Task<T> get() { co_return co_await GetAwaiter{*this}; }

   private:
    struct Waiter {
        ExternalWaitRegistration registration;
        bool reserved = false;
    };

    static bool same_wait(const ExternalWaitRegistration& left,
                          const ExternalWaitRegistration& right) {
        return left.scheduler == right.scheduler &&
               left.wait.state_index == right.wait.state_index &&
               left.wait.generation == right.wait.generation;
    }

    bool has_unreserved_item() {
        cleanup();
        return items_.size() > reserved_items_;
    }

    bool suspend(std::coroutine_handle<> handle, TaskPromiseBase& promise,
                 Scheduler& scheduler,
                 std::optional<ExternalWaitRegistration>& registration) {
        cleanup();
        if (items_.size() > reserved_items_) return false;
        bind(scheduler);
        registration = scheduler.park_external(handle, promise);
        waiters_.push_back(Waiter{*registration, false});
        return true;
    }

    T consume(const std::optional<ExternalWaitRegistration>& registration) {
        if (registration) {
            auto waiter = waiters_.begin();
            while (waiter != waiters_.end() &&
                   !same_wait(waiter->registration, *registration)) {
                ++waiter;
            }
            if (waiter == waiters_.end() || !waiter->reserved || items_.empty()) {
                std::fprintf(stderr,
                             "cpptb: Channel consumer resumed without an item\n");
                std::abort();
            }
            T value = std::move(items_.front());
            items_.pop_front();
            --reserved_items_;
            waiters_.erase(waiter);
            cleanup();
            wake_available();
            return value;
        }

        cleanup();
        if (items_.size() <= reserved_items_) {
            std::fprintf(stderr,
                         "cpptb: Channel consumer resumed without an item\n");
            std::abort();
        }
        auto item = items_.begin() + static_cast<std::ptrdiff_t>(reserved_items_);
        T value = std::move(*item);
        items_.erase(item);
        return value;
    }

    void abandon(const ExternalWaitRegistration& registration) {
        auto waiter = waiters_.begin();
        while (waiter != waiters_.end() &&
               !same_wait(waiter->registration, registration)) {
            ++waiter;
        }
        if (waiter == waiters_.end()) return;
        if (waiter->reserved) --reserved_items_;
        waiters_.erase(waiter);
        cleanup();
        wake_available();
    }

    void wake_available() {
        cleanup();
        Scheduler* scheduler = nullptr;
        for (auto& waiter : waiters_) {
            if (items_.size() <= reserved_items_) break;
            if (waiter.reserved) continue;
            waiter.reserved = true;
            ++reserved_items_;
            scheduler = waiter.registration.scheduler;
            if (!scheduler->wake_external(waiter.registration)) {
                waiter.reserved = false;
                --reserved_items_;
            }
        }
        if (scheduler) scheduler->flush_external_wakes();
    }

    void bind(Scheduler& scheduler) {
        if (scheduler_ && scheduler_ != &scheduler) {
            std::fprintf(stderr,
                         "cpptb: Channel cannot have waiters from multiple schedulers\n");
            std::abort();
        }
        scheduler_ = &scheduler;
        lifetime_ = scheduler.lifetime();
    }

    void cleanup() {
        auto output = waiters_.begin();
        for (auto& waiter : waiters_) {
            const bool alive = !waiter.registration.lifetime.expired() &&
                               waiter.registration.scheduler &&
                               waiter.registration.scheduler->external_wait_alive(
                                   waiter.registration);
            if (alive) {
                *output++ = std::move(waiter);
            } else if (waiter.reserved) {
                --reserved_items_;
            }
        }
        waiters_.erase(output, waiters_.end());
        if (waiters_.empty()) reset_binding();
    }

    void reset_binding() {
        scheduler_ = nullptr;
        lifetime_.reset();
    }

    std::deque<T> items_;
    std::deque<Waiter> waiters_;
    Scheduler* scheduler_ = nullptr;
    std::weak_ptr<SchedulerLifetime> lifetime_;
    size_t reserved_items_ = 0;
};

class Testbench {
   public:
    explicit Testbench(SimTime precision = 1_ns) : scheduler_(precision) {}

    Process spawn(Task<void>&& task) { return scheduler_.spawn(std::move(task)); }

    void spawn_detached(Task<void>&& task) {
        scheduler_.spawn_detached(std::move(task));
    }

    void notify_edge(uint32_t signal_id, EdgeKind edge) {
        scheduler_.resume_edge(signal_id, edge);
    }

    void notify_read_write() { scheduler_.resume_phase(WaitKind::ReadWrite); }

    void notify_read_only() { scheduler_.resume_phase(WaitKind::ReadOnly); }

    void notify_next_time_step() {
        scheduler_.resume_phase(WaitKind::NextTimeStep);
    }

    bool has_read_write_waiters() const {
        return scheduler_.has_phase_waiters(WaitKind::ReadWrite);
    }

    bool has_read_only_waiters() const {
        return scheduler_.has_phase_waiters(WaitKind::ReadOnly);
    }

    bool has_next_time_step_waiters() const {
        return scheduler_.has_phase_waiters(WaitKind::NextTimeStep);
    }

    SimulationPhase current_phase() const {
        return scheduler_.current_phase();
    }

    uint8_t edge_interest(uint32_t signal_id) const {
        return scheduler_.edge_interest(signal_id);
    }

    bool has_edge_interest(uint32_t signal_id, EdgeKind edge) const {
        return scheduler_.has_edge_interest(signal_id, edge);
    }

    void configure_static_edge_source(uint32_t signal_id) {
        scheduler_.configure_static_edge_source(signal_id);
    }

    bool is_static_edge_source(uint32_t signal_id) const {
        return scheduler_.is_static_edge_source(signal_id);
    }

    uint64_t edge_interest_generation() const {
        return scheduler_.edge_interest_generation();
    }

    std::optional<EdgeInterestChange> consume_edge_interest_change() {
        return scheduler_.consume_edge_interest_change();
    }

    bool has_falling_edge_waiters() const {
        return scheduler_.has_falling_edge_waiters();
    }

    bool consume_timer_schedule_changed() {
        return scheduler_.consume_timer_schedule_changed();
    }

    uint64_t next_timer_deadline() { return scheduler_.next_timer_deadline(); }

    bool done() const { return scheduler_.all_done(); }

#ifdef CPPTB_CORO_WAIT_PATH_DIAGNOSTICS
    uint64_t single_edge_park_count(EdgeKind edge) const {
        return scheduler_.single_edge_park_count(edge);
    }

    uint64_t edge_notification_count() const {
        return scheduler_.edge_notification_count();
    }

    uint64_t edge_notification_resume_count() const {
        return scheduler_.edge_notification_resume_count();
    }

    uint64_t compound_wait_park_count() const {
        return scheduler_.compound_wait_park_count();
    }
#endif

    SimTime now() const { return scheduler_.now_time(); }

    uint64_t now_ticks() const { return time_; }

    void set_time(uint64_t time) {
        time_ = time;
        scheduler_.advance_time(time);
    }

    void log(std::string_view message) const {
        std::printf("cpptb[%llu fs]: %.*s\n",
                    static_cast<unsigned long long>(now().femtoseconds),
                    static_cast<int>(message.size()), message.data());
    }

    void expect_eq(std::string_view label, uint32_t actual, uint32_t expected) {
        if (actual == expected) {
            std::printf("cpptb[%llu fs]: PASS %.*s = 0x%08x\n",
                        static_cast<unsigned long long>(now().femtoseconds),
                        static_cast<int>(label.size()), label.data(), actual);
            return;
        }

        ++failures_;
        std::printf(
            "cpptb[%llu fs]: FAIL %.*s got 0x%08x expected 0x%08x\n",
            static_cast<unsigned long long>(now().femtoseconds),
            static_cast<int>(label.size()), label.data(), actual, expected);
    }

    uint32_t failures() const { return failures_; }

   private:
    Scheduler scheduler_;
    uint64_t time_ = 0;
    uint32_t failures_ = 0;
};

}  // namespace cpptb::coro
