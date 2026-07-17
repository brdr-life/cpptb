#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "cpptb/components.hpp"

namespace {

bool expect(const char* label, bool condition) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", label);
    return false;
}

struct RecordingSubscriber {
    uint32_t id;
    std::vector<uint32_t>& deliveries;

    void write(const uint32_t& value) {
        deliveries.push_back(id * 100u + value);
    }
};

struct SingleSlotBackend {
    cpptb::coro::Task<void> put(uint32_t value) {
        slot = value;
        co_return;
    }

    bool put_nowait(uint32_t value) {
        if (slot) return false;
        slot = value;
        return true;
    }

    cpptb::coro::Task<uint32_t> get() {
        const uint32_t value = *slot;
        slot.reset();
        co_return value;
    }

    std::optional<uint32_t> get_nowait() {
        if (!slot) return std::nullopt;
        const uint32_t value = *slot;
        slot.reset();
        return value;
    }

    std::optional<uint32_t> slot;
};

struct ReferencePutBackend {
    cpptb::coro::Task<void> put(const uint32_t&) { co_return; }
    bool put_nowait(uint32_t) { return true; }
};

static_assert(!std::constructible_from<cpptb::PutPort<uint32_t>,
                                       ReferencePutBackend&>);

using UintAnalysisPort = cpptb::AnalysisPort<uint32_t>;

struct DisconnectingSubscriber {
    std::vector<uint32_t>& deliveries;
    UintAnalysisPort::Connection* target = nullptr;

    void write(const uint32_t& value) {
        deliveries.push_back(100u + value);
        target->disconnect();
    }
};

struct ConnectingSubscriber {
    UintAnalysisPort& port;
    RecordingSubscriber& late;
    std::optional<UintAnalysisPort::Connection>& late_connection;

    void write(const uint32_t&) {
        if (!late_connection) late_connection.emplace(port.connect(late));
    }
};

struct DisconnectingThrowingSubscriber {
    UintAnalysisPort::Connection* self = nullptr;

    void write(const uint32_t&) {
        self->disconnect();
        throw std::runtime_error{"disconnect during analysis publication"};
    }
};

struct FakeSignal {
    using value_type = uint32_t;

    operator cpptb::coro::Signal() const { return signal; }
    uint32_t get() const { return *value; }
    void set(uint32_t next) const { *value = next; }

    cpptb::coro::Signal signal;
    uint32_t* value;
};

cpptb::coro::Task<void> monitor_once(
    FakeSignal clock, FakeSignal valid, FakeSignal ready, FakeSignal data,
    UintAnalysisPort& observed, bool& done) {
    cpptb::ReadyValidMonitor monitor{
        clock, valid, ready, data, cpptb::ReadyValidSampleEdge::Rising,
        cpptb::coro::SimTime{}};
    co_await monitor.run(observed, 1);
    done = true;
}

cpptb::coro::Task<void> drive_once(FakeSignal clock, FakeSignal valid,
                                   FakeSignal ready, FakeSignal data,
                                   uint32_t& stalls, bool& done) {
    cpptb::ReadyValidDriver driver{clock, valid, ready, data,
                                   cpptb::coro::SimTime{}};
    stalls = co_await driver.send(0x55);
    done = true;
}

struct ComponentDut {};

struct ThrowingSubscriber {
    void write(const uint32_t&) {
        throw std::runtime_error{"analysis subscriber failed"};
    }
};

cpptb::coro::Task<void> throwing_analysis_child() {
    cpptb::AnalysisPort<uint32_t> analysis;
    ThrowingSubscriber subscriber;
    auto connection = analysis.connect(subscriber);
    analysis.write(1);
    co_return;
}

cpptb::coro::Task<void> analysis_exception_test(
    ComponentDut, cpptb::TestContext& test) {
    test.spawn_detached(throwing_analysis_child());
    co_await cpptb::coro::Delay{cpptb::coro::operator""_ns(1)};
}

}  // namespace

int main() {
    bool passed = true;

    {
        cpptb::coro::Queue<uint32_t> queue{1};
        cpptb::PutPort put{queue};
        cpptb::GetPort get{queue};
        passed &= expect("queue PutPort accepts value", put.put_nowait(11));
        passed &= expect("queue PutPort reports full", !put.put_nowait(12));
        const auto value = get.get_nowait();
        passed &= expect("queue GetPort returns value", value && *value == 11);
        passed &= expect("queue GetPort reports empty",
                         !get.get_nowait().has_value());
    }

    {
        SingleSlotBackend backend;
        cpptb::PutPort<uint32_t> put{backend};
        cpptb::GetPort<uint32_t> get{backend};
        passed &= expect("custom backend PutPort", put.put_nowait(21));
        const auto value = get.get_nowait();
        passed &= expect("custom backend GetPort", value && *value == 21);
    }

    {
        cpptb::AnalysisPort<uint32_t> analysis;
        std::vector<uint32_t> deliveries;
        RecordingSubscriber first{1, deliveries};
        RecordingSubscriber second{2, deliveries};
        auto first_connection = analysis.connect(first);
        auto second_connection = analysis.connect(second);

        passed &= expect("analysis subscriber count",
                         analysis.subscriber_count() == 2);
        analysis.write(7);
        passed &= expect("analysis subscription order",
                         deliveries == std::vector<uint32_t>{107, 207});

        first_connection.disconnect();
        analysis.write(8);
        passed &= expect(
            "analysis disconnect",
            deliveries == std::vector<uint32_t>{107, 207, 208});
        passed &= expect("analysis connection state",
                         !first_connection.connected() &&
                             second_connection.connected());

        auto moved_connection = std::move(second_connection);
        passed &= expect("analysis connection move",
                         !second_connection.connected() &&
                             moved_connection.connected());

        {
            RecordingSubscriber scoped{3, deliveries};
            auto scoped_connection = analysis.connect(scoped);
            passed &= expect("analysis scoped connection active",
                             analysis.subscriber_count() == 2);
        }
        passed &= expect("analysis RAII disconnect",
                         analysis.subscriber_count() == 1);
    }

    {
        UintAnalysisPort analysis;
        std::vector<uint32_t> deliveries;
        DisconnectingSubscriber first{deliveries};
        RecordingSubscriber second{2, deliveries};
        auto first_connection = analysis.connect(first);
        auto second_connection = analysis.connect(second);
        first.target = &second_connection;

        analysis.write(3);
        passed &= expect("disconnect during publication skips subscriber",
                         deliveries == std::vector<uint32_t>{103});
        passed &= expect("disconnect during publication compacts",
                         analysis.subscriber_count() == 1);
    }

    {
        UintAnalysisPort analysis;
        std::vector<uint32_t> deliveries;
        RecordingSubscriber late{4, deliveries};
        std::optional<UintAnalysisPort::Connection> late_connection;
        ConnectingSubscriber connector{analysis, late, late_connection};
        auto connector_connection = analysis.connect(connector);

        analysis.write(1);
        passed &= expect("connected during publication waits for next write",
                         deliveries.empty());
        analysis.write(2);
        passed &= expect("late analysis subscriber receives next write",
                         deliveries == std::vector<uint32_t>{402});
    }

    {
        UintAnalysisPort analysis;
        DisconnectingThrowingSubscriber subscriber;
        auto connection = analysis.connect(subscriber);
        subscriber.self = &connection;
        bool threw = false;
        try {
            analysis.write(1);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        passed &= expect("analysis exception propagates", threw);
        passed &= expect("analysis exception path compacts disconnect",
                         analysis.subscriber_count() == 0 &&
                             !connection.connected());
    }

    {
        cpptb::AnalysisBuffer<uint32_t> newest{
            1, cpptb::AnalysisOverflowPolicy::DropNewest};
        newest.write(1);
        newest.write(2);
        const auto value = newest.get_nowait();
        passed &= expect("drop-newest preserves queued value",
                         value && *value == 1);
        passed &= expect("drop-newest count", newest.dropped() == 1);

        cpptb::AnalysisBuffer<uint32_t> oldest{
            1, cpptb::AnalysisOverflowPolicy::DropOldest};
        oldest.write(3);
        oldest.write(4);
        const auto replacement = oldest.get_nowait();
        passed &= expect("drop-oldest keeps newest value",
                         replacement && *replacement == 4);
        passed &= expect("drop-oldest count", oldest.dropped() == 1);

        cpptb::AnalysisBuffer<uint32_t> error{
            1, cpptb::AnalysisOverflowPolicy::Error};
        error.write(5);
        bool threw = false;
        try {
            error.write(6);
        } catch (const std::overflow_error&) {
            threw = true;
        }
        passed &= expect("error overflow policy throws", threw);
    }

    {
        cpptb::coro::Testbench scheduler;
        cpptb::TestResult result;
        cpptb::TestContext test{scheduler, result};
        cpptb::InOrderScoreboard<uint32_t> scoreboard{test, "payload"};
        cpptb::AnalysisPort<uint32_t> expected;
        cpptb::AnalysisPort<uint32_t> actual;
        auto expected_connection = expected.connect(scoreboard.expected());
        auto actual_connection = actual.connect(scoreboard.actual());

        actual.write(10);
        expected.write(10);
        expected.write(20);
        actual.write(21);
        scoreboard.finalize();

        passed &= expect("scoreboard compares out-of-order arrival",
                         scoreboard.compared() == 2);
        passed &= expect("scoreboard records all checks", result.checks == 4);
        passed &= expect("scoreboard records mismatch", result.failures == 1);
        passed &= expect("scoreboard formats actual value",
                         result.failure_records.size() == 1 &&
                             result.failure_records[0].actual == "21");
        passed &= expect("scoreboard formats expected value",
                         result.failure_records.size() == 1 &&
                             result.failure_records[0].expected == "20");
    }

    {
        const auto registration = cpptb::register_test(
            "analysis_exception_test", analysis_exception_test);
        cpptb::coro::Testbench scheduler;
        cpptb::TestResult result;
        cpptb::run_registered_test(scheduler, ComponentDut{}, result);
        passed &= expect("subscriber exception marks test error",
                         result.status == cpptb::TestStatus::Error);
        passed &= expect(
            "subscriber exception retains message",
            result.failure_records.size() == 1 &&
                result.failure_records[0].label == "analysis subscriber failed");
        passed &= expect(
            "subscriber exception attributed to spawned process",
            result.failure_records.size() == 1 &&
                result.failure_records[0].process == "spawned process" &&
                             result.failure_records[0].process_id != 0);
    }

    {
        cpptb::coro::Testbench scheduler;
        uint32_t clock_value = 0;
        uint32_t valid_value = 1;
        uint32_t ready_value = 1;
        uint32_t data_value = 0x44;
        FakeSignal clock{{nullptr, 101, "component_clock"}, &clock_value};
        FakeSignal valid{{nullptr, 102, "component_valid"}, &valid_value};
        FakeSignal ready{{nullptr, 103, "component_ready"}, &ready_value};
        FakeSignal data{{nullptr, 104, "component_data"}, &data_value};
        UintAnalysisPort observed;
        std::vector<uint32_t> deliveries;
        RecordingSubscriber recorder{0, deliveries};
        auto connection = observed.connect(recorder);
        bool done = false;

        scheduler.spawn_detached(
            monitor_once(clock, valid, ready, data, observed, done));
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);
        passed &= expect("rising-edge zero-delay monitor completes", done);
        passed &= expect("rising-edge zero-delay monitor publishes",
                         deliveries == std::vector<uint32_t>{0x44});
    }

    {
        cpptb::coro::Testbench scheduler;
        uint32_t clock_value = 0;
        uint32_t valid_value = 0;
        uint32_t ready_value = 1;
        uint32_t data_value = 0;
        FakeSignal clock{{nullptr, 105, "driver_clock"}, &clock_value};
        FakeSignal valid{{nullptr, 106, "driver_valid"}, &valid_value};
        FakeSignal ready{{nullptr, 107, "driver_ready"}, &ready_value};
        FakeSignal data{{nullptr, 108, "driver_data"}, &data_value};
        uint32_t stalls = 99;
        bool done = false;

        scheduler.spawn_detached(
            drive_once(clock, valid, ready, data, stalls, done));
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Falling);
        passed &= expect("zero-delay driver asserts valid",
                         valid_value == 1 && data_value == 0x55);
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);
        passed &= expect("zero-delay driver waits for trailing edge", !done);
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Falling);
        passed &= expect("zero-delay driver completes transfer",
                         done && valid_value == 0 && stalls == 0);
    }

    return passed ? 0 : 1;
}
