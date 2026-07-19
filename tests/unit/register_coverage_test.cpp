#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "cpptb_vc/register_coverage.hpp"

namespace {

bool expect(const char* label, bool condition) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", label);
    return false;
}

constexpr std::array kFields{
    cpptb::vc::RegisterFieldDescriptor{
        .name = "command",
        .path = "block.control.command",
        .lsb = 0,
        .width = 8,
        .access = cpptb::vc::RegisterAccess::ReadWrite,
    },
    cpptb::vc::RegisterFieldDescriptor{
        .name = "status",
        .path = "block.control.status",
        .lsb = 8,
        .width = 8,
        .access = cpptb::vc::RegisterAccess::ReadOnly,
    },
};

constexpr std::array kRegisters{
    cpptb::vc::RegisterDescriptor{
        .name = "control",
        .path = "block.control",
        .address = 0,
        .width = 16,
        .access_width = 16,
        .fields = kFields,
    },
};

constexpr std::array kMemories{
    cpptb::vc::RegisterMemoryDescriptor{
        .name = "buffer",
        .path = "block.buffer",
        .address = 0x100,
        .entries = 4,
        .width = 32,
        .access_width = 32,
        .access = cpptb::vc::RegisterAccess::ReadWrite,
    },
};

constexpr cpptb::vc::RegisterBlockDescriptor kBlock{
    .name = "block",
    .registers = kRegisters,
    .memories = kMemories,
};

}  // namespace

int main() {
    using Transaction =
        cpptb::vc::MemoryTransaction<uint32_t, uint32_t, uint8_t>;
    using cpptb::vc::AccessPath;
    using cpptb::vc::MemoryOperation;
    bool passed = true;
    cpptb::vc::RegisterAccessCoverage coverage{kBlock, 0x1000,
                                                "peripheral"};

    coverage.write(Transaction{
        .operation = MemoryOperation::Write,
        .address = 0x1000,
        .data = 0x55,
        .byte_enable = 0x1,
    });
    coverage.write(Transaction{
        .operation = MemoryOperation::Read,
        .address = 0x1000,
    });
    coverage.write(Transaction{
        .operation = MemoryOperation::Write,
        .address = 0x1108,
        .data = 0x1234,
        .byte_enable = 0xf,
    });
    coverage.write(Transaction{
        .operation = MemoryOperation::Read,
        .address = 0x1108,
    });
    coverage.write(Transaction{
        .operation = MemoryOperation::Read,
        .address = 0x1ff0,
    });
    coverage.write(Transaction{
        .operation = MemoryOperation::Read,
        .address = 0x1000,
        .status = cpptb::vc::MemoryStatus::SlaveError,
    });

    coverage.sample_register(kRegisters[0], MemoryOperation::Write,
                             AccessPath::Backdoor);
    coverage.sample_register(kRegisters[0], MemoryOperation::Read,
                             AccessPath::Backdoor);
    coverage.sample_field(kFields[1], MemoryOperation::Read,
                          AccessPath::Backdoor);
    coverage.sample_memory(kMemories[0], 3, MemoryOperation::Read,
                           AccessPath::Backdoor);
    coverage.sample_memory(kMemories[0], 3, MemoryOperation::Write,
                           AccessPath::Backdoor);

    const auto snapshot = coverage.snapshot();
    const auto* control = snapshot.find("block.control");
    const auto* command = snapshot.find("block.control.command");
    const auto* status = snapshot.find("block.control.status");
    const auto* buffer = snapshot.find("block.buffer");
    passed &= expect("coverage snapshot retains component identity",
                     snapshot.name == "peripheral" && snapshot.samples == 9 &&
                         snapshot.failed == 1 && snapshot.unmapped == 1);
    passed &= expect(
        "register coverage separates frontdoor and backdoor operations",
        control && control->frontdoor_reads == 1 &&
            control->frontdoor_writes == 1 &&
            control->backdoor_reads == 1 && control->backdoor_writes == 1);
    passed &= expect(
        "field coverage honors access and byte-enable policies",
        command && command->frontdoor_reads == 1 &&
            command->frontdoor_writes == 1 && command->backdoor_reads == 1 &&
            command->backdoor_writes == 1 && status &&
            status->frontdoor_reads == 1 && status->frontdoor_writes == 0 &&
            status->backdoor_reads == 2 && status->backdoor_writes == 0);
    passed &= expect(
        "memory coverage records unique indices and access paths",
        buffer && buffer->frontdoor_reads == 1 &&
            buffer->frontdoor_writes == 1 && buffer->backdoor_reads == 1 &&
            buffer->backdoor_writes == 1 &&
            buffer->unique_read_indices == 2 &&
            buffer->unique_written_indices == 2);
    passed &= expect("coverage percentage uses legal operation/path bins",
                     snapshot.covered_bins() == snapshot.coverable_bins() &&
                         snapshot.coverage_percent() == 100.0);

    bool bad_index = false;
    try {
        coverage.sample_memory(kMemories[0], 4, MemoryOperation::Read);
    } catch (const std::out_of_range& error) {
        bad_index = std::string_view{error.what()}.find("block.buffer") !=
                    std::string_view::npos;
    }
    passed &= expect("coverage diagnostics retain the logical memory path",
                     bad_index);
    return passed ? 0 : 1;
}
