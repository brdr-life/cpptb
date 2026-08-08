#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "cpptb/cpptb.hpp"
#include "cpptb_vc/cpptb_vc.hpp"
#include "dut.hpp"

namespace cpptb::examples::apb_trace {
namespace {

using cpptb::Dut;
using coro::Join;
using coro::Task;
using namespace coro;
using namespace cpptb::vc;

constexpr uint32_t kTransferPairs = 128;
constexpr std::size_t kTransactionCount = kTransferPairs * 2u;

uint32_t next_word(uint32_t& state) {
    state = state * 1'664'525u + 1'013'904'223u;
    return state;
}

auto make_apb_bus(Dut dut) {
    return ApbBus{dut.clk,           dut.apb_select,    dut.apb_enable,
                  dut.apb_write,     dut.apb_address,   dut.apb_write_data,
                  dut.apb_read_data, dut.apb_ready,     dut.apb_error};
}

using Bus = decltype(make_apb_bus(std::declval<Dut>()));
using Master = ApbMaster<
    decltype(std::declval<Bus>().clock),
    decltype(std::declval<Bus>().select),
    decltype(std::declval<Bus>().enable),
    decltype(std::declval<Bus>().write),
    decltype(std::declval<Bus>().address),
    decltype(std::declval<Bus>().write_data),
    decltype(std::declval<Bus>().read_data),
    decltype(std::declval<Bus>().ready),
    decltype(std::declval<Bus>().error), NoApbStrobe>;
using Transaction = typename Master::transaction_type;

Task<void> reset_dut(Dut dut) {
    dut.rst_n.set(0);
    dut.apb_select.set(0);
    dut.apb_enable.set(0);
    dut.apb_write.set(0);
    dut.apb_address.set(0);
    dut.apb_write_data.set(0);
    co_await clock_cycles(dut.clk, 2);
    dut.rst_n.set(1);
}

Task<void> trace_sequence(Master& apb, TestContext& test,
                          AnalysisPort<Transaction>& expected,
                          uint64_t* observed_wait_cycles = nullptr) {
    constexpr auto kAllBytes = std::numeric_limits<uint64_t>::max();
    uint32_t state = 0x1357'9bdfu;

    for (uint32_t index = 0; index < kTransferPairs; ++index) {
        const uint32_t address = ((index * 13u) & 63u) * 4u;
        const uint32_t value = next_word(state);
        // Wait states counted the pre-evaluation way: every access-phase
        // cycle with PREADY low. The slow addresses hold PREADY low for two
        // access cycles; the old post-edge sampling only ever saw one.
        const uint32_t expected_wait_cycles = (address & 4u) != 0 ? 2u : 0u;

        const auto write = co_await apb.write(address, value);
        if (observed_wait_cycles) *observed_wait_cycles += write.wait_cycles;
        test.expect_eq("APB trace write status", write.status,
                       MemoryStatus::Okay);
        expected.write(Transaction{
            MemoryOperation::Write, address, value, kAllBytes,
            MemoryStatus::Okay, expected_wait_cycles});

        const auto read = co_await apb.read(address);
        if (observed_wait_cycles) *observed_wait_cycles += read.wait_cycles;
        test.expect_eq("APB trace read data", read.data, value);
        test.expect_eq("APB trace read status", read.status,
                       MemoryStatus::Okay);
        expected.write(Transaction{
            MemoryOperation::Read, address, value, kAllBytes,
            MemoryStatus::Okay, expected_wait_cycles});
    }
}

Task<void> transaction_recording_test(Dut dut, TestContext& test) {
    dut.clk.set_now(0);
    test.start_clock(dut.clk, 10_ns);
    co_await reset_dut(dut);

    const auto bus = make_apb_bus(dut);
    Master master{bus, ApbConfig{.sample_delay = 1_ps}};
    ApbMonitor monitor{test, bus, 1_ps};
    AnalysisPort<Transaction> expected;
    InOrderScoreboard<Transaction> scoreboard{test, "APB trace transaction"};

    TransactionRecorder recorder;
    InMemoryTransactionSink trace;
    auto trace_sink = recorder.connect(trace);
    auto& stream = recorder.stream<Transaction>("apb0.observed");

    auto expected_connection = expected.connect(scoreboard.expected());
    auto actual_connection = monitor.observed().connect(scoreboard.actual());
    auto recording_connection = monitor.observed().connect(stream);
    uint64_t observed_wait_cycles = 0;

    co_await Join{trace_sequence(master, test, expected,
                                 &observed_wait_cycles),
                  monitor.run(kTransactionCount)};

    scoreboard.finalize();
    const auto& records = trace.records();
    const auto* first = records.empty() ? nullptr : &records.front();
    const auto* last = records.empty() ? nullptr : &records.back();
    test.expect_eq("APB scoreboard comparison count", scoreboard.compared(),
                   kTransactionCount);
    test.expect_eq("recorded APB wait-cycle count", observed_wait_cycles,
                   uint64_t{256});
    test.expect_eq("recorded APB transaction count", records.size(),
                   kTransactionCount);
    test.expect_eq("first APB record sequence",
                   first ? first->sequence
                         : std::numeric_limits<uint64_t>::max(),
                   uint64_t{0});
    test.expect_eq("last APB record sequence",
                   last ? last->sequence
                        : std::numeric_limits<uint64_t>::max(),
                   uint64_t{kTransactionCount - 1});
    test.expect("APB record has a nonzero interval",
                first && first->end_time > first->begin_time);
    test.expect("APB trace retains typed fields",
                first && first->value_json.find(
                             "\"operation\":\"write\"") !=
                             std::string::npos);
}

CPPTB_REGISTER_TEST(transaction_recording_test);

}  // namespace
}  // namespace cpptb::examples::apb_trace
