#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "cpptb_vc/memory_model.hpp"

namespace {

std::size_t allocation_count = 0;

}  // namespace

void* operator new(std::size_t size) {
    ++allocation_count;
    if (size == 0) size = 1;
    if (void* memory = std::malloc(size)) return memory;
    throw std::bad_alloc{};
}

void operator delete(void* memory) noexcept { std::free(memory); }

void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

bool expect(const char* label, bool condition) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", label);
    return false;
}

class FaultCallback : public cpptb::vc::MemoryAccessCallback {
   public:
    void before_access(cpptb::vc::MemoryAccessEvent& access) override {
        ++before;
        if (access.address == 0x04 &&
            access.operation == cpptb::vc::MemoryOperation::Write) {
            access.data[0] = 0xef;
            access.byte_enable[1] = 0;
        }
        if (access.address == 0x44 &&
            access.operation == cpptb::vc::MemoryOperation::Read) {
            access.status = cpptb::vc::MemoryStatus::Timeout;
        }
    }

    void after_access(cpptb::vc::MemoryAccessEvent& access) override {
        ++after;
        last_region = access.region;
        if (access.address == 0x40 &&
            access.operation == cpptb::vc::MemoryOperation::Read &&
            access.status == cpptb::vc::MemoryStatus::Okay) {
            access.data[0] ^= 0xffu;
        }
        if (access.status == cpptb::vc::MemoryStatus::DecodeError) {
            access.status = cpptb::vc::MemoryStatus::SlaveError;
        }
    }

    uint32_t before = 0;
    uint32_t after = 0;
    std::string last_region;
};

}  // namespace

int main() {
    using namespace cpptb::vc;
    bool passed = true;

    SparseMemory memory;
    memory.add_region(MemoryRegionConfig{
        .name = "little_ram",
        .base = 0x00,
        .size = 0x20,
        .permission = MemoryPermission::ReadWrite,
        .byte_order = MemoryByteOrder::LittleEndian,
        .fill = 0xaa,
    });
    memory.add_region(MemoryRegionConfig{
        .name = "big_ram",
        .base = 0x20,
        .size = 0x20,
        .permission = MemoryPermission::ReadWrite,
        .byte_order = MemoryByteOrder::BigEndian,
    });
    memory.add_region(MemoryRegionConfig{
        .name = "read_only",
        .base = 0x40,
        .size = 0x08,
        .permission = MemoryPermission::Read,
    });
    memory.add_region(MemoryRegionConfig{
        .name = "write_only",
        .base = 0x48,
        .size = 0x08,
        .permission = MemoryPermission::Write,
    });

    passed &= expect("regions are recorded", memory.region_count() == 4);
    passed &= expect("fill bytes remain sparse", memory.allocated_bytes() == 0);
    const auto initial = memory.read_word<uint32_t>(0x00u);
    passed &= expect("region fill is readable",
                     initial.okay() && initial.data == 0xaaaa'aaaau);

    const auto little_write = memory.write_word(0x00u, 0x1122'3344u, 0x5u);
    passed &= expect("partial little-endian write succeeds",
                     little_write.okay());
    passed &= expect("byte enables use address lanes",
                     memory.inspect(0x00, 4) ==
                         std::vector<uint8_t>({0x44, 0xaa, 0x22, 0xaa}));
    passed &= expect("partial little-endian word reads back",
                     memory.read_word<uint32_t>(0x00u).data == 0xaa22'aa44u);

    const auto big_write = memory.write_word(0x20u, 0x1122'3344u, 0xfu);
    passed &= expect("big-endian write succeeds", big_write.okay());
    passed &= expect("big-endian bytes are in address order",
                     memory.inspect(0x20, 4) ==
                         std::vector<uint8_t>({0x11, 0x22, 0x33, 0x44}));
    passed &= expect("big-endian word reads back",
                     memory.read_word<uint32_t>(0x20u).data == 0x1122'3344u);
    const auto big_partial = memory.write_word(0x20u, 0xaabb'ccddu, 0x1u);
    passed &= expect("partial big-endian write succeeds", big_partial.okay());
    passed &= expect("big-endian byte-enable bit zero selects address lane zero",
                     memory.inspect(0x20, 4) ==
                         std::vector<uint8_t>({0xaa, 0x22, 0x33, 0x44}));
    passed &= expect("partial big-endian word reads back",
                     memory.read_word<uint32_t>(0x20u).data == 0xaa22'3344u);

    static_cast<void>(memory.write_word(0x24u, 0xdead'beefu));
    const auto allocations_before_words = allocation_count;
    uint32_t repeated_word = 0;
    for (std::size_t iteration = 0; iteration < 64; ++iteration) {
        static_cast<void>(memory.write_word(0x24u, 0xdead'beefu));
        repeated_word ^= memory.read_word<uint32_t>(0x24u).data;
    }
    passed &= expect("warmed callback-free word access does not allocate",
                     allocation_count == allocations_before_words &&
                         repeated_word == 0);

    const auto all_bytes = memory.write_word(0x04u, 0x89ab'cdefu);
    passed &= expect("two-argument write enables every address lane",
                     all_bytes.okay() &&
                         memory.read_word<uint32_t>(0x04u).data == 0x89ab'cdefu);
    std::array<uint8_t, 4> read_buffer{};
    passed &= expect("read_into fills caller-owned storage without allocation",
                     memory.read_into(0x04, read_buffer) == MemoryStatus::Okay &&
                         read_buffer ==
                             std::array<uint8_t, 4>{0xef, 0xcd, 0xab, 0x89});

    passed &= expect(
        "read-only writes return slave error",
        memory.write_word(0x40u, uint32_t{1}, uint8_t{0xf}).status ==
            MemoryStatus::SlaveError);
    passed &= expect("write-only reads return slave error",
                     memory.read_word<uint32_t>(0x48u).status ==
                         MemoryStatus::SlaveError);
    passed &= expect("unmapped reads return decode error",
                     memory.read_word<uint32_t>(0x80u).status ==
                         MemoryStatus::DecodeError);
    passed &= expect("cross-region reads return decode error",
                     memory.read_word<uint32_t>(0x1eu).status ==
                         MemoryStatus::DecodeError);

    bool negative_rejected = false;
    try {
        static_cast<void>(memory.read_word<uint32_t>(-1));
    } catch (const std::out_of_range& error) {
        negative_rejected =
            std::string_view{error.what()}.find("negative") !=
            std::string_view::npos;
    }
    passed &= expect("negative integral addresses are rejected",
                     negative_rejected);

    bool enable_count_rejected = false;
    const std::array<uint8_t, 2> short_data{1, 2};
    const std::array<uint8_t, 1> short_enable{1};
    try {
        static_cast<void>(memory.write_bytes(0, short_data, short_enable));
    } catch (const std::invalid_argument& error) {
        enable_count_rejected =
            std::string_view{error.what()}.find("count") !=
            std::string_view::npos;
    }
    passed &= expect("byte-enable size mismatch is rejected",
                     enable_count_rejected);

    bool overlap_rejected = false;
    try {
        memory.add_region(MemoryRegionConfig{
            .name = "overlap", .base = 0x10, .size = 0x20});
    } catch (const std::invalid_argument& error) {
        overlap_rejected =
            std::string_view{error.what()}.find("overlaps") !=
            std::string_view::npos;
    }
    passed &= expect("overlapping regions are rejected", overlap_rejected);

    const std::array<uint8_t, 4> image{1, 2, 3, 4};
    memory.load(0x08, image);
    memory.fill(0x0a, 2, 0x5a);
    passed &= expect("load and fill update explicit bytes",
                     memory.inspect(0x08, 4) ==
                         std::vector<uint8_t>({1, 2, 0x5a, 0x5a}));

    const auto image_path =
        std::filesystem::path{"build/cpptb/memory_model_image.bin"};
    memory.dump_file(0x08, 4, image_path);
    memory.fill(0x08, 4, 0);
    memory.load_file(0x08, image_path);
    passed &= expect("file image roundtrip",
                     memory.inspect(0x08, 4) ==
                         std::vector<uint8_t>({1, 2, 0x5a, 0x5a}));
    std::filesystem::remove(image_path);

    FaultCallback callback;
    memory.set_callback(&callback);
    const auto callback_write = memory.write_word(0x04u, 0x1234'5678u, 0xfu);
    passed &= expect("write callback can modify data and byte enables",
                     callback_write.okay() &&
                         memory.read_word<uint32_t>(0x04u).data ==
                             0x1234'cdefu);
    const auto modified = memory.read_word<uint32_t>(0x40u);
    passed &= expect("callback can modify read data",
                     modified.okay() && modified.data == 0xffu);
    const auto timeout = memory.read_word<uint32_t>(0x44u);
    passed &= expect("callback can inject an access error",
                     timeout.status == MemoryStatus::Timeout);
    const auto translated = memory.read_word<uint32_t>(0x80u);
    passed &= expect("callback can translate model status",
                     translated.status == MemoryStatus::SlaveError);
    passed &= expect("callbacks identify mapped region",
                     callback.before == 5 && callback.after == 5 &&
                         callback.last_region.empty());
    memory.set_callback(nullptr);

    cpptb::coro::Testbench scheduler;
    cpptb::TestResult result;
    cpptb::TestContext test{scheduler, result};
    SparseMemory predicted;
    predicted.add_region(MemoryRegionConfig{
        .name = "registers", .base = 0, .size = 0x40});
    using Transaction = MemoryTransaction<uint32_t, uint32_t, uint8_t>;
    auto predictor = make_memory_predictor<Transaction>(test, predicted);

    predictor.write(Transaction{
        .operation = MemoryOperation::Write,
        .address = 0x10,
        .data = 0x1234'5678,
        .byte_enable = 0xf,
        .status = MemoryStatus::Okay,
    });
    predictor.write(Transaction{
        .operation = MemoryOperation::Read,
        .address = 0x10,
        .data = 0x1234'5678,
        .byte_enable = 0xf,
        .status = MemoryStatus::Okay,
    });
    predictor.write(Transaction{
        .operation = MemoryOperation::Read,
        .address = 0x10,
        .data = 0,
        .byte_enable = 0xf,
        .status = MemoryStatus::Okay,
    });
    passed &= expect("predictor applies writes and checks reads",
                     predictor.writes() == 1 && predictor.reads() == 2 &&
                         predictor.mismatches() == 1 && result.checks == 3 &&
                         result.failures == 1);
    passed &= expect(
        "predictor mismatch retains transaction context",
        result.failure_records.size() == 1 &&
            result.failure_records[0].actual.find("address=16") !=
                std::string::npos &&
            result.failure_records[0].expected.find("data=305419896") !=
                std::string::npos);

    return passed ? 0 : 1;
}
