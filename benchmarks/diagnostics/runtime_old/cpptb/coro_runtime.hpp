#pragma once

#include <array>
#include <compare>
#include <concepts>
#include <coroutine>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <queue>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "vpi_user.h"

namespace cpptb::coro {

enum class EdgeKind : uint8_t {
    Rising,
    Falling,
    Any,
};

enum class WaitKind : uint8_t {
    Edge,
    Delay,
};

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
};

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

class Scheduler;
struct JoinState;
struct ProcessControl;

class Task {
   public:
    struct promise_type;
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

struct Task::promise_type {
    Scheduler* scheduler = nullptr;
    std::coroutine_handle<> continuation = nullptr;
    std::shared_ptr<JoinState> join_state;
    size_t state_index = std::numeric_limits<size_t>::max();

    Task get_return_object() {
        return Task{Task::handle_type::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
        bool await_ready() const noexcept { return false; }
        std::coroutine_handle<> await_suspend(Task::handle_type handle) const noexcept;
        void await_resume() const noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    void return_void() noexcept {}

    void unhandled_exception() {
        std::fprintf(stderr, "cpptb: unhandled exception in coroutine\n");
        std::abort();
    }
};

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
    Task::handle_type handle = nullptr;
    uint64_t wait_generation = 0;
    uint32_t edge_wait_count = 0;
    uint32_t falling_edge_wait_count = 0;
    bool waiting = false;
    bool ready = false;
    bool done = false;
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

        std::vector<Task::handle_type> handles;
        handles.reserve(states_.size());
        for (size_t state_index = 0; state_index < states_.size(); ++state_index) {
            auto& state = states_[state_index];
            end_wait(state);
            if (state.handle) {
                state.handle.promise().scheduler = nullptr;
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

    Process spawn(Task&& task) {
        if (shutting_down_) return {};
        auto handle = task.release();
        if (!handle) return {};

        handle.promise().scheduler = this;
        handle.promise().continuation = nullptr;
        const size_t state_index = adopt(handle);
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

    void spawn_detached(Task&& task) {
        if (shutting_down_) return;
        auto handle = task.release();
        if (!handle) return;

        handle.promise().scheduler = this;
        handle.promise().continuation = nullptr;
        const size_t state_index = adopt(handle);
        make_ready(state_index);
        drain_ready();
    }

    size_t adopt(Task::handle_type handle) {
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
        handle.promise().state_index = state_index;
        ++active_coroutines_;
        return state_index;
    }

    void park(Task::handle_type handle, WaitRequest request,
              size_t* winner = nullptr, size_t winner_index = 0) {
        auto& state = state_for(handle);
        const uint64_t generation = begin_wait(state);
        register_wait(request, WaitRegistration{handle.promise().state_index,
                                                generation, winner, winner_index});
    }

    template <size_t TriggerCount>
    void park_first(Task::handle_type handle,
                    const std::array<WaitRequest, TriggerCount>& requests,
                    size_t* winner) {
        auto& state = state_for(handle);
        const uint64_t generation = begin_wait(state);
        const size_t state_index = handle.promise().state_index;
        for (size_t index = 0; index < TriggerCount; ++index) {
            register_wait(
                requests[index],
                WaitRegistration{state_index, generation, winner, index});
        }
    }

    template <size_t ChildCount>
    void start_join(Task::handle_type parent,
                    std::array<Task, ChildCount>&& children) {
        static_assert(ChildCount >= 2, "Join requires at least two tasks");
        auto& parent_state = state_for(parent);
        begin_wait(parent_state);

        auto join_state = std::make_shared<JoinState>();
        join_state->remaining = ChildCount;
        join_state->parent_index = parent.promise().state_index;

        for (auto& child_task : children) {
            auto child = child_task.release();
            if (!child) {
                std::fprintf(stderr, "cpptb: Join received an invalid task\n");
                std::abort();
            }
            child.promise().scheduler = this;
            child.promise().continuation = nullptr;
            child.promise().join_state = join_state;
            make_ready(adopt(child));
        }
    }

    bool park_process(Task::handle_type handle, ProcessControl* control) {
        if (!control || control->scheduler != this) {
            std::fprintf(stderr, "cpptb: cannot await an invalid process\n");
            std::abort();
        }
        if (control->done) return false;

        auto& state = state_for(handle);
        const uint64_t generation = begin_wait(state);
        control->completion_waiters.push_back(
            WaitRegistration{handle.promise().state_index, generation, nullptr,
                             0});
        if (control->done) {
            control->completion_waiters.pop_back();
            end_wait(state);
            return false;
        }
        return true;
    }

    void finish(Task::handle_type handle) noexcept {
        const size_t state_index = handle.promise().state_index;
        if (!state_matches(state_index, handle)) return;
        auto& state = states_[state_index];
        end_wait(state);
        state.ready = false;
        state.done = true;
        if (active_coroutines_ != 0) --active_coroutines_;
        const bool child = handle.promise().continuation != nullptr ||
                           handle.promise().join_state != nullptr;
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

    void resume_edge(uint32_t signal_id, EdgeKind edge) {
        const bool resumed_specific =
            resume_edge_queue(edge_queue_index(signal_id, edge));
        const bool resumed_any =
            resume_edge_queue(edge_queue_index(signal_id, EdgeKind::Any));
        if (resumed_specific || resumed_any) {
            drain_ready();
        } else {
            scheduler_boundary();
        }
    }

    void advance_time(uint64_t time) {
        now_ = time;
        bool popped = false;
        bool resumed = false;
        while (true) {
            discard_stale_timers();
            if (timer_waiters_.empty() || timer_waiters_.top().deadline > now_) break;
            const auto registration = timer_waiters_.top();
            timer_waiters_.pop();
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

    bool has_falling_edge_waiters() const {
        return falling_edge_registrations_ != 0;
    }

    bool consume_timer_schedule_changed() {
        const bool changed = timer_schedule_changed_;
        timer_schedule_changed_ = false;
        return changed;
    }

    uint64_t next_timer_deadline() {
        discard_stale_timers();
        if (timer_waiters_.empty()) return std::numeric_limits<uint64_t>::max();
        return timer_waiters_.top().deadline;
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
            const auto& promise = candidate.handle.promise();
            if (promise.continuation == parent_handle ||
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
            handle.promise().continuation = nullptr;
            handle.promise().join_state.reset();
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
        if (state.edge_wait_count != 0) {
            stale_edge_registrations_ += state.edge_wait_count;
            state.edge_wait_count = 0;
        }
        if (state.falling_edge_wait_count != 0) {
            falling_edge_registrations_ -= state.falling_edge_wait_count;
            state.falling_edge_wait_count = 0;
        }
    }

    void register_wait(WaitRequest request, WaitRegistration registration) {
        switch (request.kind) {
            case WaitKind::Edge:
                {
                    const size_t queue_index =
                        edge_queue_index(request.signal_id, request.edge);
                    if (queue_index >= edge_waiters_.size()) {
                        edge_waiters_.resize(queue_index + 1);
                    }
                    edge_waiters_[queue_index].push_back(registration);
                    ++edge_queue_entries_;
                    auto& state = states_[registration.state_index];
                    ++state.edge_wait_count;
                    if (request.edge == EdgeKind::Falling ||
                        request.edge == EdgeKind::Any) {
                        ++falling_edge_registrations_;
                        ++state.falling_edge_wait_count;
                    }
                }
                break;
            case WaitKind::Delay: {
                if (request.delay.femtoseconds == 0) {
                    std::fprintf(stderr,
                                 "cpptb: delay duration must be greater than zero\n");
                    std::abort();
                }
                if ((request.delay.femtoseconds % femtoseconds_per_tick_) != 0) {
                    std::fprintf(
                        stderr,
                        "cpptb: delay of %llu fs is not representable at "
                        "%llu fs simulation precision\n",
                        static_cast<unsigned long long>(
                            request.delay.femtoseconds),
                        static_cast<unsigned long long>(
                            femtoseconds_per_tick_));
                    std::abort();
                }
                const uint64_t delay_ticks =
                    request.delay.femtoseconds / femtoseconds_per_tick_;
                const uint64_t max_time = std::numeric_limits<uint64_t>::max();
                const uint64_t deadline =
                    delay_ticks > max_time - now_ ? max_time
                                                  : now_ + delay_ticks;
                const uint64_t previous_deadline = next_timer_deadline();
                timer_waiters_.push(TimerRegistration{
                    deadline, next_timer_sequence_++, registration});
                if (deadline < previous_deadline) timer_schedule_changed_ = true;
                break;
            }
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
        while (!timer_waiters_.empty() &&
               !registration_valid(timer_waiters_.top().wait)) {
            timer_waiters_.pop();
            timer_schedule_changed_ = true;
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

    CoroutineState& state_for(Task::handle_type handle) {
        const size_t state_index = handle.promise().state_index;
        if (!state_matches(state_index, handle)) {
            std::fprintf(stderr, "cpptb: coroutine was not registered with scheduler\n");
            std::abort();
        }
        return states_[state_index];
    }

    bool state_matches(size_t state_index, Task::handle_type handle) const {
        return state_index < states_.size() &&
               states_[state_index].handle == handle;
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

            handle.promise().scheduler = nullptr;
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
    std::priority_queue<TimerRegistration, std::vector<TimerRegistration>,
                        TimerRegistrationLater>
        timer_waiters_;
    std::vector<size_t> ready_;
    uint64_t now_ = 0;
    uint64_t next_wait_generation_ = 0;
    uint64_t next_timer_sequence_ = 0;
    size_t active_coroutines_ = 0;
    size_t falling_edge_registrations_ = 0;
    size_t edge_queue_entries_ = 0;
    size_t stale_edge_registrations_ = 0;
    uint64_t femtoseconds_per_tick_ = 1'000'000u;
    bool timer_schedule_changed_ = false;
    bool draining_ = false;
    bool reclaiming_ = false;
    bool shutting_down_ = false;
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

    bool await_suspend(Task::handle_type handle) const {
        auto* scheduler = handle.promise().scheduler;
        if (!scheduler) return false;
        return scheduler->park_process(handle, process.control_);
    }

    void await_resume() const noexcept {}
};

inline Process::Awaiter Process::operator co_await() const {
    return Awaiter{*this};
}

inline std::coroutine_handle<> Task::promise_type::FinalAwaiter::await_suspend(
    Task::handle_type handle) const noexcept {
    auto* scheduler = handle.promise().scheduler;
    if (scheduler) scheduler->finish(handle);

    if (handle.promise().join_state) {
        if (scheduler) scheduler->finish_join_child(handle.promise().join_state);
        return std::noop_coroutine();
    }

    auto continuation = handle.promise().continuation;
    if (continuation) return continuation;
    return std::noop_coroutine();
}

struct Task::Awaiter {
    explicit Awaiter(Task&& task) : task_(std::move(task)) {}

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(Task::handle_type parent) {
        auto* scheduler = parent.promise().scheduler;
        if (!scheduler) {
            std::fprintf(stderr, "cpptb: cannot await a task without a scheduler\n");
            std::abort();
        }

        auto child = task_.release();
        if (!child) return parent;

        child.promise().scheduler = scheduler;
        child.promise().continuation = parent;
        scheduler->adopt(child);
        return child;
    }

    void await_resume() const noexcept {}

   private:
    Task task_;
};

inline Task::Awaiter Task::operator co_await() && {
    return Awaiter{std::move(*this)};
}

template <typename Trigger>
struct BasicTriggerAwaiter {
    Trigger trigger;

    bool await_ready() const noexcept { return false; }

    void await_suspend(Task::handle_type handle) const {
        auto* scheduler = handle.promise().scheduler;
        if (!scheduler) {
            std::fprintf(stderr, "cpptb: cannot wait without a scheduler\n");
            std::abort();
        }
        scheduler->park(handle, wait_request(trigger));
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

template <typename... Triggers>
struct FirstAwaiter {
    First<Triggers...> trigger;
    size_t winner = std::numeric_limits<size_t>::max();

    bool await_ready() const noexcept { return false; }

    void await_suspend(Task::handle_type handle) {
        auto* scheduler = handle.promise().scheduler;
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
        scheduler->park_first(handle, requests, &winner);
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

    std::array<Task, ChildCount> children;

    template <typename... Children>
        requires(sizeof...(Children) == ChildCount &&
                 (std::same_as<std::remove_cvref_t<Children>, Task> && ...))
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

    void await_suspend(Task::handle_type handle) {
        auto* scheduler = handle.promise().scheduler;
        if (!scheduler) {
            std::fprintf(stderr, "cpptb: cannot wait on Join without a scheduler\n");
            std::abort();
        }
        scheduler->start_join(handle, std::move(trigger.children));
    }

    void await_resume() const noexcept {}
};

template <size_t ChildCount>
JoinAwaiter<ChildCount> operator co_await(Join<ChildCount> trigger) {
    return JoinAwaiter<ChildCount>{std::move(trigger)};
}

class Testbench {
   public:
    explicit Testbench(SimTime precision = 1_ns) : scheduler_(precision) {}

    Process spawn(Task&& task) { return scheduler_.spawn(std::move(task)); }

    void spawn_detached(Task&& task) {
        scheduler_.spawn_detached(std::move(task));
    }

    void notify_edge(uint32_t signal_id, EdgeKind edge) {
        scheduler_.resume_edge(signal_id, edge);
    }

    bool has_falling_edge_waiters() const {
        return scheduler_.has_falling_edge_waiters();
    }

    bool consume_timer_schedule_changed() {
        return scheduler_.consume_timer_schedule_changed();
    }

    uint64_t next_timer_deadline() { return scheduler_.next_timer_deadline(); }

    bool done() const { return scheduler_.all_done(); }

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
