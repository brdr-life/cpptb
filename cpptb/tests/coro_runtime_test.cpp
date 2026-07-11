#include <csignal>
#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "cpptb/coro_runtime.hpp"

namespace {

using namespace cpptb::coro;

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

Task<void> cancellation_racer(Process* target) {
    co_await Delay{4_ns};
    target->cancel();
}

Task<void> delayed_increment(uint32_t& value, SimTime delay = 4_ns) {
    co_await Delay{delay};
    ++value;
}

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

Task<void> collect_timeout(Edge trigger, SimTime timeout, uint32_t& outcome,
                           uint32_t& resumes) {
    outcome = static_cast<uint32_t>(co_await with_timeout(trigger, timeout));
    ++resumes;
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
    static_assert(1_fs == SimTime{1});
    static_assert(1_ps == SimTime{1'000});
    static_assert(1_ns == SimTime{1'000'000});
    static_assert(1_us == SimTime{1'000'000'000});
    static_assert(1_ms == SimTime{1'000'000'000'000});

    bool passed = true;
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
        tb.spawn_detached(collect_timeout(Edge{winner_signal}, 5_ns, winner,
                                          winner_resumes));
        tb.spawn_detached(collect_timeout(Edge{timeout_signal}, 3_ns, timed_out,
                                          timeout_resumes));
        tb.notify_edge(winner_signal.id, EdgeKind::Any);
        tb.set_time(3);
        passed &= expect("with_timeout edge winner", winner,
                         static_cast<uint32_t>(TimeoutOutcome::Triggered));
        passed &= expect("with_timeout timeout winner", timed_out,
                         static_cast<uint32_t>(TimeoutOutcome::TimedOut));
        tb.notify_edge(timeout_signal.id, EdgeKind::Any);
        tb.set_time(5);
        passed &= expect("with_timeout edge resumes once", winner_resumes, 1);
        passed &= expect("with_timeout stale edge cleanup", timeout_resumes, 1);
        passed &= expect("with_timeout done", tb.done() ? 1 : 0, 1);
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
    }

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
    return passed ? 0 : 1;
}
