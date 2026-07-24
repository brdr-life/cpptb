#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cpptb_vc/register_model.hpp"

namespace cpptb::vc {

enum class RegisterCoverageKind : uint8_t {
    Register,
    Field,
    Memory,
};

inline constexpr std::string_view cpptb_diagnostic_name(
    RegisterCoverageKind kind) noexcept {
    switch (kind) {
        case RegisterCoverageKind::Register:
            return "register";
        case RegisterCoverageKind::Field:
            return "field";
        case RegisterCoverageKind::Memory:
            return "memory";
    }
    return "unknown";
}

struct RegisterAccessCoverageEntrySnapshot {
    std::string path;
    RegisterCoverageKind kind = RegisterCoverageKind::Register;
    bool readable = false;
    bool writable = false;
    uint64_t frontdoor_reads = 0;
    uint64_t frontdoor_writes = 0;
    uint64_t backdoor_reads = 0;
    uint64_t backdoor_writes = 0;
    uint64_t unique_read_indices = 0;
    uint64_t unique_written_indices = 0;

    [[nodiscard]] uint64_t coverable_bins() const noexcept {
        return (readable ? 2u : 0u) + (writable ? 2u : 0u);
    }

    [[nodiscard]] uint64_t covered_bins() const noexcept {
        uint64_t result = 0;
        result += readable && frontdoor_reads != 0;
        result += writable && frontdoor_writes != 0;
        result += readable && backdoor_reads != 0;
        result += writable && backdoor_writes != 0;
        return result;
    }
};

struct RegisterAccessCoverageSnapshot {
    std::string name;
    uint64_t samples = 0;
    uint64_t failed = 0;
    uint64_t unmapped = 0;
    std::vector<RegisterAccessCoverageEntrySnapshot> entries;

    [[nodiscard]] uint64_t coverable_bins() const noexcept {
        uint64_t result = 0;
        for (const auto& entry : entries) result += entry.coverable_bins();
        return result;
    }

    [[nodiscard]] uint64_t covered_bins() const noexcept {
        uint64_t result = 0;
        for (const auto& entry : entries) result += entry.covered_bins();
        return result;
    }

    [[nodiscard]] double coverage_percent() const noexcept {
        const uint64_t total = coverable_bins();
        return total == 0
                   ? 100.0
                   : 100.0 * static_cast<double>(covered_bins()) /
                         static_cast<double>(total);
    }

    [[nodiscard]] const RegisterAccessCoverageEntrySnapshot* find(
        std::string_view path) const noexcept {
        const auto found = std::find_if(
            entries.begin(), entries.end(), [&](const auto& entry) {
                return entry.path == path;
            });
        return found == entries.end() ? nullptr : &*found;
    }
};

class RegisterAccessCoverage {
   public:
    explicit RegisterAccessCoverage(
        const RegisterBlockDescriptor& descriptor, uint64_t base_address = 0,
        std::string name = "register_access")
        : descriptor_(&descriptor), name_(std::move(name)) {
        if (name_.empty()) {
            throw std::invalid_argument(
                "cpptb-vc: register access coverage name is empty");
        }
        build_entries(base_address);
    }

    template <std::integral Address, std::unsigned_integral Data,
              std::unsigned_integral ByteEnable>
    void write(const MemoryTransaction<Address, Data, ByteEnable>& transaction) {
        if (!memory_okay(transaction.status)) {
            ++failed_;
            return;
        }
        const auto address = normalize_address(transaction.address);
        if (!address.has_value()) {
            ++unmapped_;
            return;
        }
        if (sample_register_transfer(*address, transaction.operation,
                                     static_cast<uint64_t>(
                                         transaction.byte_enable))) {
            ++samples_;
            return;
        }
        if (sample_memory_transfer(*address, transaction.operation)) {
            ++samples_;
            return;
        }
        ++unmapped_;
    }

    template <std::integral Address, std::unsigned_integral Data,
              std::unsigned_integral ByteEnable>
    void write(const TransactionObservation<
               MemoryTransaction<Address, Data, ByteEnable>>& observation) {
        if (observation.disposition == TransactionDisposition::Completed) {
            write(observation.value);
        }
    }

    void sample_register(const RegisterDescriptor& descriptor,
                         MemoryOperation operation,
                         AccessPath path = AccessPath::Backdoor) {
        auto* entry = find_register(descriptor);
        if (!entry) fail_unknown(descriptor.path);
        sample_counter(entry->counter, operation, path);
        for (const auto& field : entry->fields) {
            if (operation_allowed(counters_[field.counter], operation)) {
                sample_counter(field.counter, operation, path);
            }
        }
        ++samples_;
    }

    void sample_field(const RegisterFieldDescriptor& descriptor,
                      MemoryOperation operation,
                      AccessPath path = AccessPath::Backdoor) {
        for (const auto& entry : registers_) {
            const auto found = std::find_if(
                entry.fields.begin(), entry.fields.end(),
                [&](const FieldEntry& field) {
                    return field.descriptor == &descriptor;
                });
            if (found == entry.fields.end()) continue;
            sample_counter(found->counter, operation, path);
            ++samples_;
            return;
        }
        fail_unknown(descriptor.path);
    }

    void sample_memory(const RegisterMemoryDescriptor& descriptor,
                       uint64_t index, MemoryOperation operation,
                       AccessPath path = AccessPath::Backdoor) {
        auto* entry = find_memory(descriptor);
        if (!entry) fail_unknown(descriptor.path);
        if (index >= descriptor.entries) {
            throw std::out_of_range(
                "cpptb-vc: register access coverage memory index out of bounds: " +
                std::string{descriptor.path});
        }
        sample_counter(entry->counter, operation, path);
        sample_index(*entry, index, operation);
        ++samples_;
    }

    template <typename Handle>
        requires requires(const Handle& handle) {
            { handle.descriptor() } ->
                std::same_as<const RegisterDescriptor&>;
        }
    void sample_register(const Handle& handle, MemoryOperation operation,
                         AccessPath path = AccessPath::Backdoor) {
        sample_register(handle.descriptor(), operation, path);
    }

    template <typename Handle>
        requires requires(const Handle& handle) {
            { handle.descriptor() } ->
                std::same_as<const RegisterMemoryDescriptor&>;
        }
    void sample_memory(const Handle& handle, uint64_t index,
                       MemoryOperation operation,
                       AccessPath path = AccessPath::Backdoor) {
        sample_memory(handle.descriptor(), index, operation, path);
    }

    [[nodiscard]] RegisterAccessCoverageSnapshot snapshot() const {
        RegisterAccessCoverageSnapshot result{
            .name = name_,
            .samples = samples_,
            .failed = failed_,
            .unmapped = unmapped_,
        };
        result.entries.reserve(counters_.size());
        for (const auto& counter : counters_) {
            result.entries.push_back(RegisterAccessCoverageEntrySnapshot{
                .path = std::string{counter.path},
                .kind = counter.kind,
                .readable = counter.readable,
                .writable = counter.writable,
                .frontdoor_reads = counter.frontdoor_reads,
                .frontdoor_writes = counter.frontdoor_writes,
                .backdoor_reads = counter.backdoor_reads,
                .backdoor_writes = counter.backdoor_writes,
                .unique_read_indices = counter.read_indices.size(),
                .unique_written_indices = counter.written_indices.size(),
            });
        }
        return result;
    }

   private:
    struct Counter {
        std::string_view path;
        RegisterCoverageKind kind = RegisterCoverageKind::Register;
        bool readable = false;
        bool writable = false;
        uint64_t frontdoor_reads = 0;
        uint64_t frontdoor_writes = 0;
        uint64_t backdoor_reads = 0;
        uint64_t backdoor_writes = 0;
        std::unordered_set<uint64_t> read_indices;
        std::unordered_set<uint64_t> written_indices;
    };

    struct FieldEntry {
        const RegisterFieldDescriptor* descriptor;
        std::size_t counter;
    };

    struct RegisterEntry {
        const RegisterDescriptor* descriptor;
        uint64_t address;
        uint64_t end_address;
        std::size_t counter;
        std::vector<FieldEntry> fields;
    };

    struct MemoryEntry {
        const RegisterMemoryDescriptor* descriptor;
        uint64_t address;
        uint64_t end_address;
        std::size_t counter;
    };

    static bool descriptor_readable(const RegisterDescriptor& descriptor) {
        if (descriptor.fields.empty()) return true;
        return std::any_of(descriptor.fields.begin(), descriptor.fields.end(),
                           [](const auto& field) {
                               return register_readable(field.access);
                           });
    }

    static bool descriptor_writable(const RegisterDescriptor& descriptor) {
        if (descriptor.fields.empty()) return true;
        return std::any_of(descriptor.fields.begin(), descriptor.fields.end(),
                           [](const auto& field) {
                               return register_writable(field.access);
                           });
    }

    std::size_t add_counter(std::string_view path, RegisterCoverageKind kind,
                            bool readable, bool writable) {
        counters_.push_back(Counter{.path = path,
                                    .kind = kind,
                                    .readable = readable,
                                    .writable = writable});
        return counters_.size() - 1;
    }

    void build_entries(uint64_t base_address) {
        for (const auto& descriptor : descriptor_->registers) {
            const uint64_t address = checked_address(base_address,
                                                     descriptor.address,
                                                     descriptor.path);
            RegisterEntry entry{
                .descriptor = &descriptor,
                .address = address,
                .end_address = checked_address(address, descriptor.width / 8u,
                                               descriptor.path),
                .counter = add_counter(
                    descriptor.path, RegisterCoverageKind::Register,
                    descriptor_readable(descriptor),
                    descriptor_writable(descriptor)),
            };
            for (const auto& field : descriptor.fields) {
                entry.fields.push_back(FieldEntry{
                    .descriptor = &field,
                    .counter = add_counter(
                        field.path, RegisterCoverageKind::Field,
                        register_readable(field.access),
                        register_writable(field.access)),
                });
            }
            registers_.push_back(std::move(entry));
        }
        for (const auto& descriptor : descriptor_->memories) {
            const uint64_t address = checked_address(base_address,
                                                     descriptor.address,
                                                     descriptor.path);
            const uint64_t bytes = checked_product(
                descriptor.entries, descriptor.width / 8u, descriptor.path);
            memories_.push_back(MemoryEntry{
                .descriptor = &descriptor,
                .address = address,
                .end_address = checked_address(address, bytes, descriptor.path),
                .counter = add_counter(
                    descriptor.path, RegisterCoverageKind::Memory,
                    register_readable(descriptor.access),
                    register_writable(descriptor.access)),
            });
        }
        std::sort(registers_.begin(), registers_.end(),
                  [](const auto& left, const auto& right) {
                      return left.address < right.address;
                  });
        std::sort(memories_.begin(), memories_.end(),
                  [](const auto& left, const auto& right) {
                      return left.address < right.address;
                  });
    }

    bool sample_register_transfer(uint64_t address, MemoryOperation operation,
                                  uint64_t byte_enable) {
        for (auto& entry : registers_) {
            if (address < entry.address || address >= entry.end_address) {
                continue;
            }
            const auto& descriptor = *entry.descriptor;
            const uint64_t transfer_bytes = descriptor.access_width / 8u;
            const uint64_t offset = address - entry.address;
            if (transfer_bytes == 0 || offset % transfer_bytes != 0) {
                return false;
            }
            const uint64_t transfer = offset / transfer_bytes;
            const uint64_t bit_lsb =
                descriptor.endianness == RegisterEndianness::Little
                    ? transfer * descriptor.access_width
                    : descriptor.width -
                          (transfer + 1u) * descriptor.access_width;
            sample_counter(entry.counter, operation, AccessPath::Frontdoor);
            for (const auto& field : entry.fields) {
                if (!operation_allowed(counters_[field.counter], operation) ||
                    !field_touched(*field.descriptor, bit_lsb,
                                   descriptor.access_width, operation,
                                   byte_enable)) {
                    continue;
                }
                sample_counter(field.counter, operation,
                               AccessPath::Frontdoor);
            }
            return true;
        }
        return false;
    }

    bool sample_memory_transfer(uint64_t address, MemoryOperation operation) {
        for (auto& entry : memories_) {
            if (address < entry.address || address >= entry.end_address) {
                continue;
            }
            const uint64_t element_bytes = entry.descriptor->width / 8u;
            const uint64_t index = (address - entry.address) / element_bytes;
            sample_counter(entry.counter, operation, AccessPath::Frontdoor);
            sample_index(entry, index, operation);
            return true;
        }
        return false;
    }

    static bool field_touched(const RegisterFieldDescriptor& field,
                              uint64_t transfer_lsb,
                              uint16_t transfer_width,
                              MemoryOperation operation,
                              uint64_t byte_enable) {
        const uint64_t field_end = field.lsb + field.width;
        const uint64_t transfer_end = transfer_lsb + transfer_width;
        const uint64_t begin = std::max<uint64_t>(field.lsb, transfer_lsb);
        const uint64_t end = std::min(field_end, transfer_end);
        if (begin >= end) return false;
        if (operation == MemoryOperation::Read) return true;
        for (uint64_t bit = begin; bit < end; ++bit) {
            const uint64_t transfer_byte = (bit - transfer_lsb) / 8u;
            if (((byte_enable >> transfer_byte) & 1u) != 0) return true;
        }
        return false;
    }

    static bool operation_allowed(const Counter& counter,
                                  MemoryOperation operation) noexcept {
        return operation == MemoryOperation::Read ? counter.readable
                                                  : counter.writable;
    }

    void sample_counter(std::size_t index, MemoryOperation operation,
                        AccessPath path) {
        Counter& counter = counters_[index];
        if (!operation_allowed(counter, operation)) return;
        uint64_t* selected = nullptr;
        if (operation == MemoryOperation::Read) {
            selected = path == AccessPath::Frontdoor
                           ? &counter.frontdoor_reads
                           : &counter.backdoor_reads;
        } else {
            selected = path == AccessPath::Frontdoor
                           ? &counter.frontdoor_writes
                           : &counter.backdoor_writes;
        }
        ++*selected;
    }

    void sample_index(MemoryEntry& entry, uint64_t index,
                      MemoryOperation operation) {
        Counter& counter = counters_[entry.counter];
        if (operation == MemoryOperation::Read) {
            counter.read_indices.insert(index);
        } else {
            counter.written_indices.insert(index);
        }
    }

    RegisterEntry* find_register(const RegisterDescriptor& descriptor) {
        const auto found = std::find_if(
            registers_.begin(), registers_.end(), [&](const auto& entry) {
                return entry.descriptor == &descriptor;
            });
        return found == registers_.end() ? nullptr : &*found;
    }

    MemoryEntry* find_memory(const RegisterMemoryDescriptor& descriptor) {
        const auto found = std::find_if(
            memories_.begin(), memories_.end(), [&](const auto& entry) {
                return entry.descriptor == &descriptor;
            });
        return found == memories_.end() ? nullptr : &*found;
    }

    template <std::integral Address>
    static std::optional<uint64_t> normalize_address(Address address) {
        if constexpr (std::signed_integral<Address>) {
            if (address < 0) return std::nullopt;
        }
        return static_cast<uint64_t>(address);
    }

    static uint64_t checked_address(uint64_t base, uint64_t offset,
                                    std::string_view path) {
        if (offset > std::numeric_limits<uint64_t>::max() - base) {
            throw std::overflow_error(
                "cpptb-vc: register access coverage address overflows: " +
                std::string{path});
        }
        return base + offset;
    }

    static uint64_t checked_product(uint64_t left, uint64_t right,
                                    std::string_view path) {
        if (left != 0 && right >
                             std::numeric_limits<uint64_t>::max() / left) {
            throw std::overflow_error(
                "cpptb-vc: register access coverage memory size overflows: " +
                std::string{path});
        }
        return left * right;
    }

    [[noreturn]] static void fail_unknown(std::string_view path) {
        throw std::invalid_argument(
            "cpptb-vc: register access coverage descriptor is not in this model: " +
            std::string{path});
    }

    const RegisterBlockDescriptor* descriptor_;
    std::string name_;
    std::vector<Counter> counters_;
    std::vector<RegisterEntry> registers_;
    std::vector<MemoryEntry> memories_;
    uint64_t samples_ = 0;
    uint64_t failed_ = 0;
    uint64_t unmapped_ = 0;
};

}  // namespace cpptb::vc
