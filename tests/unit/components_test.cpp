#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "cpptb_vc/cpptb_vc.hpp"

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

static_assert(!std::constructible_from<cpptb::vc::PutPort<uint32_t>,
                                       ReferencePutBackend&>);

using UintAnalysisPort = cpptb::vc::AnalysisPort<uint32_t>;

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

template <std::size_t Width>
struct FakePackedSignal {
    using value_type = uint32_t;
    static constexpr std::size_t width = Width;

    operator cpptb::coro::Signal() const { return signal; }
    uint32_t get() const { return *value; }
    void set(uint32_t next) const { *value = next; }

    cpptb::coro::Signal signal;
    uint32_t* value;
};

using FakeReadyValidDriver =
    cpptb::vc::ReadyValidDriver<FakeSignal, FakeSignal, FakeSignal, FakeSignal>;
using FakeReadyValidSink =
    cpptb::vc::ReadyValidSink<FakeSignal, FakeSignal, FakeSignal, FakeSignal>;
static_assert(cpptb::vc::StreamSource<FakeReadyValidDriver>);
static_assert(cpptb::vc::StreamSink<FakeReadyValidSink>);

struct TaggedTransaction {
    uint32_t id;
    uint32_t data;

    friend bool operator==(const TaggedTransaction&,
                           const TaggedTransaction&) = default;
};

template <typename T>
struct VectorSubscriber {
    std::vector<T>& values;

    void write(const T& value) { values.push_back(value); }
};

template <cpptb::vc::MemoryMappedMaster Master>
cpptb::coro::Task<void> apb_write_once(
    Master& master, typename Master::write_response_type& response,
    bool& done) {
    response = co_await master.write(0x24u, 0x1234'5678u, 0xdu);
    done = true;
}

template <cpptb::vc::MemoryMappedMaster Master>
cpptb::coro::Task<void> apb_read_once(
    Master& master, typename Master::read_response_type& response,
    bool& done) {
    response = co_await master.read(0x28u);
    done = true;
}

template <cpptb::vc::MemoryMappedMaster Master>
cpptb::coro::Task<void> reject_partial_apb3_write(Master& master,
                                                  bool& rejected) {
    try {
        static_cast<void>(co_await master.write(0x10u, 0x55u, 0xfu));
    } catch (const std::invalid_argument& error) {
        rejected =
            std::string_view{error.what()}.find("requires a PSTRB signal") !=
            std::string_view::npos;
    }
}

cpptb::coro::Task<void> monitor_once(
    FakeSignal clock, FakeSignal valid, FakeSignal ready, FakeSignal data,
    UintAnalysisPort& observed, bool& done) {
    cpptb::vc::ReadyValidMonitor monitor{
        clock, valid, ready, data, cpptb::vc::ReadyValidSampleEdge::Rising,
        cpptb::coro::SimTime{}};
    co_await monitor.run(observed, 1);
    done = true;
}

cpptb::coro::Task<void> drive_once(FakeSignal clock, FakeSignal valid,
                                   FakeSignal ready, FakeSignal data,
                                   uint32_t& stalls, bool& done) {
    cpptb::vc::ReadyValidDriver driver{clock, valid, ready, data,
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
    cpptb::vc::AnalysisPort<uint32_t> analysis;
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
        cpptb::vc::PutPort put{queue};
        cpptb::vc::GetPort get{queue};
        passed &= expect("queue PutPort accepts value", put.put_nowait(11));
        passed &= expect("queue PutPort reports full", !put.put_nowait(12));
        const auto value = get.get_nowait();
        passed &= expect("queue GetPort returns value", value && *value == 11);
        passed &= expect("queue GetPort reports empty",
                         !get.get_nowait().has_value());
    }

    {
        SingleSlotBackend backend;
        cpptb::vc::PutPort<uint32_t> put{backend};
        cpptb::vc::GetPort<uint32_t> get{backend};
        passed &= expect("custom backend PutPort", put.put_nowait(21));
        const auto value = get.get_nowait();
        passed &= expect("custom backend GetPort", value && *value == 21);
    }

    {
        cpptb::vc::AnalysisPort<uint32_t> analysis;
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
        cpptb::vc::AnalysisBuffer<uint32_t> newest{
            1, cpptb::vc::AnalysisOverflowPolicy::DropNewest};
        newest.write(1);
        newest.write(2);
        const auto value = newest.get_nowait();
        passed &= expect("drop-newest preserves queued value",
                         value && *value == 1);
        passed &= expect("drop-newest count", newest.dropped() == 1);

        cpptb::vc::AnalysisBuffer<uint32_t> oldest{
            1, cpptb::vc::AnalysisOverflowPolicy::DropOldest};
        oldest.write(3);
        oldest.write(4);
        const auto replacement = oldest.get_nowait();
        passed &= expect("drop-oldest keeps newest value",
                         replacement && *replacement == 4);
        passed &= expect("drop-oldest count", oldest.dropped() == 1);

        cpptb::vc::AnalysisBuffer<uint32_t> error{
            1, cpptb::vc::AnalysisOverflowPolicy::Error};
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
        cpptb::vc::InOrderScoreboard<uint32_t> scoreboard{test, "payload"};
        cpptb::vc::AnalysisPort<uint32_t> expected;
        cpptb::vc::AnalysisPort<uint32_t> actual;
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

    {
        cpptb::coro::Testbench scheduler;
        cpptb::TestResult result;
        cpptb::TestContext test{scheduler, result};
        const auto key_of = [](const TaggedTransaction& item) {
            return item.id;
        };
        cpptb::vc::KeyedScoreboard<TaggedTransaction, decltype(key_of)>
            scoreboard{test, "tagged payload", key_of};

        scoreboard.actual().write({2, 22});
        scoreboard.expected().write({1, 11});
        scoreboard.expected().write({2, 22});
        scoreboard.actual().write({1, 11});
        scoreboard.finalize();

        passed &= expect("keyed scoreboard matches out-of-order transactions",
                         scoreboard.compared() == 2 &&
                             scoreboard.expected_pending() == 0 &&
                             scoreboard.actual_pending() == 0);
        passed &= expect("keyed scoreboard emits comparisons and finalize checks",
                         result.checks == 4 && result.failures == 0);
    }

    {
        cpptb::coro::Testbench scheduler;
        cpptb::TestResult result;
        cpptb::TestContext test{scheduler, result};
        const auto key_of = [](const TaggedTransaction& item) {
            return item.id;
        };
        cpptb::vc::KeyedScoreboard<TaggedTransaction, decltype(key_of)>
            scoreboard{test, "missing tagged payload", key_of};
        scoreboard.expected().write({7, 70});
        scoreboard.finalize();
        passed &= expect("keyed scoreboard reports unmatched transaction",
                         scoreboard.expected_pending() == 1 &&
                             result.checks == 2 && result.failures == 1);
    }

    {
        auto model = cpptb::vc::make_reference_model<uint32_t>(
            [offset = uint32_t{7}](const uint32_t& value) {
                return value * 2u + offset;
            });
        std::vector<uint32_t> predictions;
        VectorSubscriber<uint32_t> subscriber{predictions};
        auto connection = model.predicted.connect(subscriber);
        model.write(5);
        passed &= expect("reference model adapter publishes prediction",
                         predictions == std::vector<uint32_t>{17});
    }

    {
        const cpptb::vc::MemoryTransaction<uint32_t, uint32_t, uint8_t>
            transaction{cpptb::vc::MemoryOperation::Write, 0x20u,
                        0x1234u, 0xfu,
                        cpptb::vc::MemoryStatus::SlaveError, 2};
        const auto diagnostic = cpptb::format_diagnostic(transaction);
        passed &= expect(
            "memory transaction diagnostic is contextual",
            diagnostic &&
                diagnostic->find("operation=write address=32") !=
                    std::string::npos &&
                diagnostic->find("status=slave_error wait_cycles=2") !=
                    std::string::npos);
    }

    {
        cpptb::coro::Testbench scheduler;
        uint32_t clock_value = 0;
        uint32_t select_value = 0;
        uint32_t enable_value = 0;
        uint32_t write_value = 0;
        uint32_t address_value = 0;
        uint32_t write_data_value = 0;
        uint32_t read_data_value = 0xa5a5'5a5a;
        uint32_t ready_value = 1;
        uint32_t error_value = 0;
        uint32_t strobe_value = 0;
        FakeSignal clock{{nullptr, 201, "pclk"}, &clock_value};
        FakeSignal select{{nullptr, 202, "psel"}, &select_value};
        FakeSignal enable{{nullptr, 203, "penable"}, &enable_value};
        FakeSignal write{{nullptr, 204, "pwrite"}, &write_value};
        FakeSignal address{{nullptr, 205, "paddr"}, &address_value};
        FakeSignal write_data{{nullptr, 206, "pwdata"}, &write_data_value};
        FakeSignal read_data{{nullptr, 207, "prdata"}, &read_data_value};
        FakeSignal ready{{nullptr, 208, "pready"}, &ready_value};
        FakeSignal error{{nullptr, 209, "pslverr"}, &error_value};
        FakePackedSignal<4> strobe{{nullptr, 210, "pstrb"}, &strobe_value};
        auto bus = cpptb::vc::ApbBus{clock, select, enable, write, address,
                                     write_data, read_data, ready, error,
                                     strobe};
        cpptb::vc::ApbMaster master{bus};
        static_assert(cpptb::vc::MemoryMappedMaster<decltype(master)>);
        static_assert(decltype(master)::all_bytes() == 0xfu);

        cpptb::vc::MemoryWriteResponse write_response;
        bool write_done = false;
        scheduler.spawn_detached(
            apb_write_once(master, write_response, write_done));
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Falling);
        passed &= expect("APB master drives setup phase",
                         select_value == 1 && enable_value == 0 &&
                             write_value == 1 && address_value == 0x24 &&
                             write_data_value == 0x1234'5678 &&
                             strobe_value == 0xd);
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Falling);
        passed &= expect("APB master drives access phase", enable_value == 1);
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Falling);
        passed &= expect("APB write returns response and idles bus",
                         write_done && write_response.okay() &&
                             write_response.wait_cycles == 0 &&
                             select_value == 0 && enable_value == 0);

        cpptb::vc::MemoryReadResponse<uint32_t> read_response;
        bool read_done = false;
        scheduler.spawn_detached(
            apb_read_once(master, read_response, read_done));
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Falling);
        passed &= expect("APB4 read setup clears PSTRB",
                         select_value == 1 && enable_value == 0 &&
                             write_value == 0 && strobe_value == 0);
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Falling);
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Falling);
        passed &= expect("APB read returns sampled data",
                         read_done && read_response.okay() &&
                             read_response.data == 0xa5a5'5a5a);
    }

    {
        cpptb::coro::Testbench scheduler;
        cpptb::TestResult result;
        cpptb::TestContext test{scheduler, result};
        uint32_t clock_value = 0;
        uint32_t select_value = 0;
        uint32_t enable_value = 0;
        uint32_t write_value = 0;
        uint32_t address_value = 0;
        uint32_t write_data_value = 0;
        uint32_t read_data_value = 0;
        uint32_t ready_value = 1;
        uint32_t error_value = 0;
        uint32_t strobe_value = 0;
        FakeSignal clock{{nullptr, 240, "monitor_pclk"}, &clock_value};
        FakeSignal select{{nullptr, 241, "monitor_psel"}, &select_value};
        FakeSignal enable{{nullptr, 242, "monitor_penable"}, &enable_value};
        FakeSignal write{{nullptr, 243, "monitor_pwrite"}, &write_value};
        FakeSignal address{{nullptr, 244, "monitor_paddr"}, &address_value};
        FakeSignal write_data{{nullptr, 245, "monitor_pwdata"},
                              &write_data_value};
        FakeSignal read_data{{nullptr, 246, "monitor_prdata"},
                             &read_data_value};
        FakeSignal ready{{nullptr, 247, "monitor_pready"}, &ready_value};
        FakeSignal error{{nullptr, 248, "monitor_pslverr"}, &error_value};
        FakePackedSignal<4> strobe{{nullptr, 249, "monitor_pstrb"},
                                   &strobe_value};
        auto bus = cpptb::vc::ApbBus{clock, select, enable, write, address,
                                     write_data, read_data, ready, error,
                                     strobe};
        cpptb::vc::ApbMonitor monitor{test, bus};
        using Observation = typename decltype(monitor)::observation_type;
        std::vector<Observation> observations;
        VectorSubscriber<Observation> subscriber{observations};
        auto connection = monitor.observed().connect(subscriber);
        scheduler.spawn_detached(monitor.run(1));

        select_value = 1;
        write_value = 1;
        address_value = 0x10;
        write_data_value = 0xaaaa'aaaa;
        strobe_value = 0xf;
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);
        select_value = 0;
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);

        select_value = 1;
        enable_value = 0;
        address_value = 0x18;
        write_data_value = 0xbbbb'bbbb;
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);
        address_value = 0x24;
        write_data_value = 0x1234'5678;
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);
        enable_value = 1;
        ready_value = 0;
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);
        ready_value = 1;
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);

        passed &= expect(
            "APB monitor resynchronizes after an abandoned setup",
            observations.size() == 1 &&
                observations[0].value.address == 0x24 &&
                observations[0].value.data == 0x1234'5678 &&
                observations[0].value.wait_cycles == 1 &&
                observations[0].disposition ==
                    cpptb::vc::TransactionDisposition::Completed);
    }

    {
        cpptb::coro::Testbench scheduler;
        cpptb::TestResult result;
        cpptb::TestContext test{scheduler, result};
        uint32_t clock_value = 0;
        uint32_t select_value = 1;
        uint32_t enable_value = 0;
        uint32_t write_value = 0;
        uint32_t address_value = 0x20;
        uint32_t write_data_value = 0x1111'1111;
        uint32_t read_data_value = 0;
        uint32_t ready_value = 1;
        uint32_t error_value = 0;
        uint32_t strobe_value = 0;
        FakeSignal clock{{nullptr, 230, "read_checker_pclk"}, &clock_value};
        FakeSignal select{{nullptr, 231, "read_checker_psel"}, &select_value};
        FakeSignal enable{{nullptr, 232, "read_checker_penable"},
                          &enable_value};
        FakeSignal write{{nullptr, 233, "read_checker_pwrite"}, &write_value};
        FakeSignal address{{nullptr, 234, "read_checker_paddr"},
                           &address_value};
        FakeSignal write_data{{nullptr, 235, "read_checker_pwdata"},
                              &write_data_value};
        FakeSignal read_data{{nullptr, 236, "read_checker_prdata"},
                             &read_data_value};
        FakeSignal ready{{nullptr, 237, "read_checker_pready"}, &ready_value};
        FakeSignal error{{nullptr, 238, "read_checker_pslverr"}, &error_value};
        FakeSignal strobe{{nullptr, 239, "read_checker_pstrb"}, &strobe_value};
        auto bus = cpptb::vc::ApbBus{clock, select, enable, write, address,
                                     write_data, read_data, ready, error,
                                     strobe};
        cpptb::vc::ApbProtocolChecker checker{test, bus};

        scheduler.spawn_detached(checker.run_cycles(2));
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);
        enable_value = 1;
        write_data_value = 0x2222'2222;
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);
        passed &= expect("APB checker ignores PWDATA changes during reads",
                         checker.violations() == 0 && result.failures == 0);

        enable_value = 0;
        strobe_value = 0x3;
        scheduler.spawn_detached(checker.run_cycles(1));
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);
        passed &= expect(
            "APB checker diagnoses nonzero PSTRB during reads",
            checker.violations() == 1 && result.failures == 1 &&
                result.failure_records[0].label ==
                    "APB PSTRB must be zero during reads");
    }

    {
        cpptb::coro::Testbench scheduler;
        uint32_t clock_value = 0;
        uint32_t select_value = 0;
        uint32_t enable_value = 0;
        uint32_t write_value = 0;
        uint32_t address_value = 0;
        uint32_t write_data_value = 0;
        uint32_t read_data_value = 0;
        uint32_t ready_value = 0;
        uint32_t error_value = 0;
        FakeSignal clock{{nullptr, 220, "timeout_pclk"}, &clock_value};
        FakeSignal select{{nullptr, 221, "timeout_psel"}, &select_value};
        FakeSignal enable{{nullptr, 222, "timeout_penable"}, &enable_value};
        FakeSignal write{{nullptr, 223, "timeout_pwrite"}, &write_value};
        FakeSignal address{{nullptr, 224, "timeout_paddr"}, &address_value};
        FakeSignal write_data{{nullptr, 225, "timeout_pwdata"},
                              &write_data_value};
        FakeSignal read_data{{nullptr, 226, "timeout_prdata"},
                             &read_data_value};
        FakeSignal ready{{nullptr, 227, "timeout_pready"}, &ready_value};
        FakeSignal error{{nullptr, 228, "timeout_pslverr"}, &error_value};
        auto bus = cpptb::vc::ApbBus{clock, select, enable, write, address,
                                     write_data, read_data, ready, error};
        cpptb::vc::ApbMaster master{
            bus, cpptb::vc::ApbConfig{.max_wait_cycles = 2}};
        cpptb::vc::MemoryReadResponse<uint32_t> response;
        bool done = false;
        scheduler.spawn_detached(apb_read_once(master, response, done));
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Falling);
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Falling);
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Falling);
        passed &= expect("APB timeout is explicit and bus returns idle",
                         done &&
                             response.status == cpptb::vc::MemoryStatus::Timeout &&
                             response.wait_cycles == 2 &&
                             select_value == 0 && enable_value == 0);
    }

    {
        cpptb::coro::Testbench scheduler;
        cpptb::TestResult result;
        cpptb::TestContext test{scheduler, result};
        uint32_t clock_value = 0;
        uint32_t select_value = 0;
        uint32_t enable_value = 1;
        uint32_t write_value = 0;
        uint32_t address_value = 0;
        uint32_t write_data_value = 0;
        uint32_t read_data_value = 0;
        uint32_t ready_value = 1;
        uint32_t error_value = 0;
        FakeSignal clock{{nullptr, 211, "checker_pclk"}, &clock_value};
        FakeSignal select{{nullptr, 212, "checker_psel"}, &select_value};
        FakeSignal enable{{nullptr, 213, "checker_penable"}, &enable_value};
        FakeSignal write{{nullptr, 214, "checker_pwrite"}, &write_value};
        FakeSignal address{{nullptr, 215, "checker_paddr"}, &address_value};
        FakeSignal write_data{{nullptr, 216, "checker_pwdata"},
                              &write_data_value};
        FakeSignal read_data{{nullptr, 217, "checker_prdata"},
                             &read_data_value};
        FakeSignal ready{{nullptr, 218, "checker_pready"}, &ready_value};
        FakeSignal error{{nullptr, 219, "checker_pslverr"}, &error_value};
        auto bus = cpptb::vc::ApbBus{clock, select, enable, write, address,
                                     write_data, read_data, ready, error};
        cpptb::vc::ApbMaster apb3_master{bus};
        bool rejected_partial_write = false;
        scheduler.spawn_detached(
            reject_partial_apb3_write(apb3_master, rejected_partial_write));
        passed &= expect("APB3 partial write has actionable diagnostic",
                         rejected_partial_write);

        cpptb::vc::ApbProtocolChecker checker{test, bus};
        scheduler.spawn_detached(checker.run_cycles(1));
        scheduler.notify_edge(clock.signal.id,
                              cpptb::coro::EdgeKind::Rising);
        passed &= expect("APB checker reports PENABLE without PSEL",
                         checker.violations() == 1 && result.failures == 1 &&
                             result.failure_records[0].label ==
                                 "APB PENABLE requires PSEL");
    }

    return passed ? 0 : 1;
}
