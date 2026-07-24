#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "cpptb_vc/scoreboards.hpp"
#include "cpptb_vc/transaction_recording.hpp"

namespace {

using namespace cpptb::coro;
using namespace cpptb::vc;

bool expect(const char* label, bool condition) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", label);
    return false;
}

struct PacketTransaction {
    uint8_t opcode = 0;
    cpptb::Bits<20> payload{};
    cpptb::LogicBits<4> state{};
    std::array<uint16_t, 2> tags{};
    std::string label;
    bool accepted = false;
};

struct FloatingTransaction {
    double ratio = 0.0;
};

struct UnsupportedValue {};

struct UnsupportedTransaction {
    UnsupportedValue value;
};

CPPTB_VC_DESCRIBE_TRANSACTION(
    PacketTransaction, "packet",
    transaction_field<&PacketTransaction::opcode>("opcode"),
    transaction_field<&PacketTransaction::payload>("payload"),
    transaction_field<&PacketTransaction::state>("state"),
    transaction_field<&PacketTransaction::tags>("tags"),
    transaction_field<&PacketTransaction::label>("label"),
    transaction_field<&PacketTransaction::accepted>("accepted"));

CPPTB_VC_DESCRIBE_TRANSACTION(
    FloatingTransaction, "floating",
    transaction_field<&FloatingTransaction::ratio>("ratio"));

CPPTB_VC_DESCRIBE_TRANSACTION(
    UnsupportedTransaction, "unsupported",
    transaction_field<&UnsupportedTransaction::value>("value"));

static_assert(DescribedTransaction<PacketTransaction>);
static_assert(DescribedTransaction<
              MemoryTransaction<uint32_t, uint32_t, uint8_t>>);
static_assert(!std::copy_constructible<TransactionRecorder>);
static_assert(!std::move_constructible<TransactionRecorder>);

std::filesystem::path temporary_trace_path() {
    const auto nonce = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    return std::filesystem::temp_directory_path() /
           ("cpptb-transaction-recording-" + std::to_string(nonce) +
            ".jsonl");
}

}  // namespace

int main() {
    bool passed = true;

    {
        cpptb::coro::Testbench scheduler;
        cpptb::TestResult result;
        cpptb::TestContext test{scheduler, result};
        InOrderScoreboard<uint32_t> scoreboard{test, "observed value"};
        AnalysisPort<TransactionObservation<uint32_t>> observed;
        auto connection = observed.connect(scoreboard.actual());

        scoreboard.expected().write(17);
        observed.write(TransactionObservation<uint32_t>{
            .begin_time = 1_ns,
            .end_time = 2_ns,
            .value = 17,
        });
        observed.write(TransactionObservation<uint32_t>{
            .begin_time = 3_ns,
            .end_time = 4_ns,
            .disposition = TransactionDisposition::Incomplete,
            .value = 99,
        });
        scoreboard.finalize();
        passed &= expect(
            "scoreboards compare completed observations and ignore incomplete ones",
            scoreboard.compared() == 1 && result.failures == 0);
    }

    TransactionRecorder recorder;
    InMemoryTransactionSink memory;
    auto sink_connection = recorder.connect(memory);
    auto& packets = recorder.stream<PacketTransaction>("input.observed");

    packets.write(TransactionObservation<PacketTransaction>{
        .begin_time = 10_ns,
        .end_time = 12_ns,
        .value = PacketTransaction{
            .opcode = 7,
            .payload = cpptb::Bits<20>::from_hex("abcde"),
            .state = cpptb::LogicBits<4>::from_string("10XZ"),
            .tags = {3, 5},
            .label = "rx\n\"quoted\"",
            .accepted = true,
        },
    });
    packets.write(TransactionObservation<PacketTransaction>{
        .begin_time = 20_ns,
        .end_time = 21_ns,
        .disposition = TransactionDisposition::Aborted,
        .value = PacketTransaction{.opcode = 9},
    });

    passed &= expect("typed stream records completed and aborted observations",
                     memory.records().size() == 2);
    passed &= expect("record metadata is retained",
                     memory.records()[0].stream == "input.observed" &&
                         memory.records()[0].type == "packet" &&
                         memory.records()[0].sequence == 0 &&
                         memory.records()[0].begin_time == 10_ns &&
                         memory.records()[0].end_time == 12_ns);
    passed &= expect(
        "custom transaction fields use structured JSON",
        memory.records()[0].value_json ==
            "{\"opcode\":7,\"payload\":\"20'habcde\",\"state\":\"4'b10XZ\",\"tags\":[3,5],\"label\":\"rx\\n\\\"quoted\\\"\",\"accepted\":true}");
    passed &= expect("disposition is retained",
                     memory.records()[1].disposition ==
                         TransactionDisposition::Aborted &&
                         memory.records()[1].sequence == 1);

    recorder.set_enabled(false);
    packets.write(TransactionObservation<PacketTransaction>{
        .begin_time = 30_ns,
        .end_time = 31_ns,
        .value = PacketTransaction{.opcode = 11},
    });
    recorder.set_enabled(true);
    passed &= expect("disabled recorder does not publish or consume sequence",
                     memory.records().size() == 2 &&
                         packets.next_sequence() == 2);

    bool duplicate_rejected = false;
    try {
        static_cast<void>(
            recorder.stream<PacketTransaction>("input.observed"));
    } catch (const std::invalid_argument& error) {
        duplicate_rejected =
            std::string{error.what()}.find("duplicate transaction stream") !=
            std::string::npos;
    }
    passed &= expect("duplicate stream names have an actionable diagnostic",
                     duplicate_rejected);

    bool empty_rejected = false;
    try {
        static_cast<void>(recorder.stream<PacketTransaction>(""));
    } catch (const std::invalid_argument& error) {
        empty_rejected =
            std::string{error.what()}.find("must not be empty") !=
            std::string::npos;
    }
    passed &= expect("empty stream names have an actionable diagnostic",
                     empty_rejected);

    const auto path = temporary_trace_path();
    {
        JsonLinesTransactionSink json{path.string()};
        auto json_connection = recorder.connect(json);
        using Memory = MemoryTransaction<uint32_t, uint32_t, uint8_t>;
        auto& accesses = recorder.stream<Memory>("apb0.observed");
        accesses.write(TransactionObservation<Memory>{
            .begin_time = 40_ns,
            .end_time = 42_ns,
            .value = Memory{
                .operation = MemoryOperation::Write,
                .address = 16,
                .data = 0x1234,
                .byte_enable = 0xf,
                .status = MemoryStatus::Okay,
                .wait_cycles = 1,
            },
        });
        json.finalize();
        passed &= expect("JSON sink reports finalization", json.finalized());
    }

    std::ifstream input{path};
    const std::string trace{std::istreambuf_iterator<char>{input},
                            std::istreambuf_iterator<char>{}};
    std::filesystem::remove(path);
    passed &= expect(
        "JSON Lines sink emits stable metadata and typed payload",
        trace.find("\"stream\":\"apb0.observed\"") != std::string::npos &&
            trace.find("\"begin_time_fs\":40000000") !=
                std::string::npos &&
            trace.find("\"operation\":\"write\"") != std::string::npos &&
            trace.find("\"wait_cycles\":1") != std::string::npos &&
            !trace.empty() && trace.back() == '\n');

    sink_connection.disconnect();
    packets.write(TransactionObservation<PacketTransaction>{
        .begin_time = 50_ns,
        .end_time = 51_ns,
        .value = PacketTransaction{.opcode = 13},
    });
    passed &= expect("dropping a sink connection stops delivery",
                     memory.records().size() == 3);

    {
        TransactionRecorder local_recorder;
        InMemoryTransactionSink local_memory;
        auto local_connection = local_recorder.connect(local_memory);
        auto& floating =
            local_recorder.stream<FloatingTransaction>("floating.observed");
        floating.write(TransactionObservation<FloatingTransaction>{
            .value = FloatingTransaction{.ratio = 1.0 / 3.0},
        });
        passed &= expect(
            "floating-point fields retain round-trip precision",
            local_memory.records()[0].value_json ==
                "{\"ratio\":0.33333333333333331}");

        bool nonfinite_rejected = false;
        try {
            floating.write(TransactionObservation<FloatingTransaction>{
                .value = FloatingTransaction{
                    .ratio = std::numeric_limits<double>::infinity()},
            });
        } catch (const std::invalid_argument& error) {
            nonfinite_rejected =
                std::string{error.what()}.find("must be finite") !=
                std::string::npos;
        }
        passed &= expect("non-finite JSON fields have an actionable error",
                         nonfinite_rejected);

        bool unsupported_rejected = false;
        auto& unsupported = local_recorder.stream<UnsupportedTransaction>(
            "unsupported.observed");
        try {
            unsupported.write(
                TransactionObservation<UnsupportedTransaction>{});
        } catch (const std::invalid_argument& error) {
            unsupported_rejected =
                std::string{error.what()}.find("no JSON encoder") !=
                std::string::npos;
        }
        passed &= expect("unsupported fields have an actionable error",
                         unsupported_rejected);
    }

    {
        const auto finalized_path = temporary_trace_path();
        TransactionRecorder local_recorder;
        JsonLinesTransactionSink json{finalized_path.string()};
        auto local_connection = local_recorder.connect(json);
        auto& floating =
            local_recorder.stream<FloatingTransaction>("floating.finalized");
        json.finalize();
        bool finalized_rejected = false;
        try {
            floating.write(TransactionObservation<FloatingTransaction>{});
        } catch (const std::logic_error& error) {
            finalized_rejected =
                std::string{error.what()}.find("already finalized") !=
                std::string::npos;
        }
        std::filesystem::remove(finalized_path);
        passed &= expect("writes after finalization have an actionable error",
                         finalized_rejected);
    }

    {
        const auto atomic_path = temporary_trace_path();
        TransactionRecorder local_recorder;
        JsonLinesTransactionSink json{atomic_path.string()};
        auto local_connection = local_recorder.connect(json);
        auto& floating =
            local_recorder.stream<FloatingTransaction>("floating.atomic");
        try {
            floating.write(TransactionObservation<FloatingTransaction>{
                .value = FloatingTransaction{
                    .ratio = std::numeric_limits<double>::infinity()},
            });
        } catch (const std::invalid_argument&) {
        }
        floating.write(TransactionObservation<FloatingTransaction>{
            .value = FloatingTransaction{.ratio = 0.5},
        });
        json.finalize();
        std::ifstream input{atomic_path};
        const std::string trace{std::istreambuf_iterator<char>{input},
                                std::istreambuf_iterator<char>{}};
        std::filesystem::remove(atomic_path);
        passed &= expect(
            "serialization failures do not leave partial JSON Lines records",
            std::count(trace.begin(), trace.end(), '\n') == 1 &&
                trace.find("\"sequence\":1") != std::string::npos &&
                trace.find("\"ratio\":0.5") != std::string::npos);
    }

    {
        const auto missing_parent = temporary_trace_path() / "trace.jsonl";
        bool open_rejected = false;
        try {
            JsonLinesTransactionSink json{missing_parent.string()};
        } catch (const std::runtime_error& error) {
            open_rejected =
                std::string{error.what()}.find("could not open") !=
                std::string::npos;
        }
        passed &= expect("unopenable trace paths have an actionable error",
                         open_rejected);
    }

    return passed ? 0 : 1;
}
