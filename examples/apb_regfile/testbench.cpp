#include <array>
#include <cstdint>
#include <limits>
#include <utility>

#include "cpptb/cpptb.hpp"
#include "cpptb_vc/cpptb_vc.hpp"
#include "dut.hpp"

namespace cpptb::examples::apb_regfile {
namespace {

using cpptb::Dut;
using coro::Event;
using coro::FallingEdge;
using coro::Join;
using coro::Task;
using namespace coro;
using namespace cpptb::vc;

constexpr uint32_t kRegisterTransactions = 12;
constexpr uint32_t kObservedTransactions =
    kRegisterTransactions * 2u + 2u;
constexpr uint32_t kIdAddress = 0x10u;
constexpr uint32_t kIdValue = 0x4350'5054u;

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

static_assert(MemoryMappedMaster<Master>);

struct CoverageSubscriber {
    Covergroup<Transaction>& coverage;

    void write(const Transaction& transaction) {
        coverage.sample(transaction);
    }
};

Task<void> reset_dut(Dut dut) {
    dut.rst_n.set(0);
    dut.apb_select.set(0);
    dut.apb_enable.set(0);
    dut.apb_write.set(0);
    dut.apb_address.set(0);
    dut.apb_write_data.set(0);

    co_await clock_cycles(dut.clk, 2);
    co_await FallingEdge{dut.clk};
    dut.rst_n.set(1);
}

template <MemoryMappedMaster BusMaster>
Task<void> register_sequence(
    BusMaster& apb, TestContext& test,
    AnalysisPort<typename BusMaster::transaction_type>* expected = nullptr) {
    const auto all_bytes = std::numeric_limits<uint64_t>::max();
    uint32_t state = 0x1020'3040u;

    for (uint32_t index = 0; index < kRegisterTransactions; ++index) {
        const uint32_t address = (index % 4u) * 4u;
        const uint32_t value = next_word(state);
        const auto write = co_await apb.write(address, value);
        test.expect_eq("APB write status", write.status, MemoryStatus::Okay);
        if (expected) {
            expected->write(Transaction{MemoryOperation::Write, address, value,
                                        all_bytes, MemoryStatus::Okay, 0});
        }

        const auto read = co_await apb.read(address);
        test.expect_eq("APB register readback", read.data, value);
        test.expect_eq("APB read status", read.status, MemoryStatus::Okay);
        if (expected) {
            expected->write(Transaction{MemoryOperation::Read, address, value,
                                        all_bytes, MemoryStatus::Okay, 0});
        }
    }

    const auto id = co_await apb.read(kIdAddress);
    test.expect_eq("APB read-only ID", id.data, kIdValue);
    test.expect_eq("APB ID status", id.status, MemoryStatus::Okay);
    if (expected) {
        expected->write(Transaction{MemoryOperation::Read, kIdAddress,
                                    kIdValue, all_bytes, MemoryStatus::Okay,
                                    0});
    }

    const auto unmapped = co_await apb.read(0xfcu);
    test.expect_eq("APB unmapped read data", unmapped.data, 0u);
    test.expect_eq("APB unmapped status", unmapped.status,
                   MemoryStatus::SlaveError);
    if (expected) {
        expected->write(Transaction{MemoryOperation::Read, 0xfcu, 0u,
                                    all_bytes, MemoryStatus::SlaveError, 0});
    }
}

class ApbMemoryPolicy : public MemoryAccessCallback {
   public:
    void after_access(MemoryAccessEvent& access) override {
        if (access.status == MemoryStatus::DecodeError) {
            access.status = MemoryStatus::SlaveError;
        }
    }
};

Task<void> component_apb_test(Dut dut, TestContext& test) {
    dut.clk.set(0);
    test.start_clock(dut.clk, 10_ns);
    co_await reset_dut(dut);

    const auto bus = make_apb_bus(dut);
    Master master{bus, ApbConfig{.sample_delay = 1_ps}};
    ApbMonitor monitor{bus, 1_ps};
    ApbProtocolChecker checker{test, bus, 1_ps};
    AnalysisPort<Transaction> expected;
    AnalysisPort<Transaction> observed;
    InOrderScoreboard<Transaction> scoreboard{test, "APB transaction"};
    Covergroup<Transaction> coverage{"apb_transactions"};
    auto& operation = coverage.coverpoint("operation", &Transaction::operation);
    operation.bin("read", MemoryOperation::Read)
        .bin("write", MemoryOperation::Write);
    auto& status = coverage.coverpoint("status", &Transaction::status);
    status.bin("okay", MemoryStatus::Okay)
        .bin("slave_error", MemoryStatus::SlaveError);
    coverage.cross("operation_x_status", operation, status);
    CoverageSubscriber coverage_subscriber{coverage};

    auto expected_connection = expected.connect(scoreboard.expected());
    auto actual_connection = observed.connect(scoreboard.actual());
    auto coverage_connection = observed.connect(coverage_subscriber);
    test.spawn_detached(checker.run_forever());

    co_await Join{register_sequence(master, test, &expected),
                  monitor.run(observed, kObservedTransactions)};

    scoreboard.finalize();
    const auto snapshot = coverage.snapshot();
    test.expect_eq("APB monitored transactions", scoreboard.compared(),
                   std::size_t{kObservedTransactions});
    test.expect_eq("APB protocol violations", checker.violations(),
                   uint64_t{0});
    test.expect_eq("APB coverage samples", snapshot.samples,
                   uint64_t{kObservedTransactions});
    test.expect_eq("APB operation coverage", snapshot.points[0].bins[0].hits,
                   uint64_t{kRegisterTransactions + 2u});
}

CPPTB_REGISTER_TEST(component_apb_test);

Task<void> memory_model_apb_test(Dut dut, TestContext& test) {
    dut.clk.set(0);
    test.start_clock(dut.clk, 10_ns);
    co_await reset_dut(dut);

    const auto bus = make_apb_bus(dut);
    Master master{bus, ApbConfig{.sample_delay = 1_ps}};
    ApbMonitor monitor{bus, 1_ps};
    AnalysisPort<Transaction> observed;

    ApbMemoryPolicy policy;
    SparseMemory memory{policy};
    memory.add_region(MemoryRegionConfig{
        .name = "registers", .base = 0, .size = 0x10});
    memory.add_region(MemoryRegionConfig{
        .name = "identification",
        .base = kIdAddress,
        .size = sizeof(kIdValue),
        .permission = MemoryPermission::Read,
    });
    const std::array id_image{
        static_cast<uint8_t>(kIdValue),
        static_cast<uint8_t>(kIdValue >> 8u),
        static_cast<uint8_t>(kIdValue >> 16u),
        static_cast<uint8_t>(kIdValue >> 24u),
    };
    memory.load(kIdAddress, id_image);

    auto predictor = make_memory_predictor<Transaction>(
        test, memory, "APB memory-model transaction");
    auto prediction_connection = observed.connect(predictor);

    co_await Join{register_sequence(master, test),
                  monitor.run(observed, kObservedTransactions)};

    test.expect_eq("memory-model reads", predictor.reads(),
                   uint64_t{kRegisterTransactions + 2u});
    test.expect_eq("memory-model writes", predictor.writes(),
                   uint64_t{kRegisterTransactions});
    test.expect_eq("memory-model mismatches", predictor.mismatches(),
                   uint64_t{0});
}

CPPTB_REGISTER_TEST(memory_model_apb_test);

}  // namespace
}  // namespace cpptb::examples::apb_regfile
