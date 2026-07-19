#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cpptb/test_api.hpp"
#include "cpptb_vc/memory_mapped.hpp"

namespace cpptb::vc {

enum class MemoryPermission : uint8_t {
    None = 0,
    Read = 1,
    Write = 2,
    ReadWrite = 3,
};

enum class MemoryByteOrder : uint8_t {
    LittleEndian,
    BigEndian,
};

inline constexpr bool memory_readable(MemoryPermission permission) noexcept {
    return (static_cast<uint8_t>(permission) &
            static_cast<uint8_t>(MemoryPermission::Read)) != 0;
}

inline constexpr bool memory_writable(MemoryPermission permission) noexcept {
    return (static_cast<uint8_t>(permission) &
            static_cast<uint8_t>(MemoryPermission::Write)) != 0;
}

struct MemoryRegionConfig {
    std::string name;
    uint64_t base = 0;
    uint64_t size = 0;
    MemoryPermission permission = MemoryPermission::ReadWrite;
    MemoryByteOrder byte_order = MemoryByteOrder::LittleEndian;
    uint8_t fill = 0;
};

struct MemoryAccessEvent {
    MemoryOperation operation = MemoryOperation::Read;
    uint64_t address = 0;
    std::span<uint8_t> data;
    std::span<uint8_t> byte_enable;
    MemoryStatus status = MemoryStatus::Okay;
    std::string_view region;
};

class MemoryAccessCallback {
   public:
    virtual ~MemoryAccessCallback() = default;

    virtual void before_access(MemoryAccessEvent&) {}
    virtual void after_access(MemoryAccessEvent&) {}
};

struct MemoryBytesResult {
    std::vector<uint8_t> data;
    MemoryStatus status = MemoryStatus::Okay;

    bool okay() const noexcept { return memory_okay(status); }
};

class SparseMemory {
   public:
    SparseMemory() = default;
    explicit SparseMemory(MemoryAccessCallback& callback) noexcept
        : callback_(&callback) {}

    void set_callback(MemoryAccessCallback* callback) noexcept {
        callback_ = callback;
    }

    void add_region(MemoryRegionConfig config) {
        if (config.name.empty()) {
            throw std::invalid_argument(
                "cpptb-vc: memory region name must not be empty");
        }
        if (config.size == 0) {
            throw std::invalid_argument(
                "cpptb-vc: memory region size must be greater than zero");
        }
        if (config.base > std::numeric_limits<uint64_t>::max() - config.size) {
            throw std::invalid_argument(
                "cpptb-vc: memory region address range overflows");
        }

        const uint64_t end = config.base + config.size;
        const auto next = regions_.lower_bound(config.base);
        if (next != regions_.end() && next->first < end) {
            throw std::invalid_argument(
                "cpptb-vc: memory region overlaps an existing region");
        }
        if (next != regions_.begin()) {
            const auto previous = std::prev(next);
            if (previous->second.end() > config.base) {
                throw std::invalid_argument(
                    "cpptb-vc: memory region overlaps an existing region");
            }
        }
        regions_.emplace(config.base, Region{std::move(config), {}});
    }

    std::size_t region_count() const noexcept { return regions_.size(); }

    std::size_t allocated_bytes() const noexcept {
        std::size_t count = 0;
        for (const auto& [base, region] : regions_) {
            static_cast<void>(base);
            count += region.bytes.size();
        }
        return count;
    }

    MemoryBytesResult read_bytes(uint64_t address, std::size_t size) {
        MemoryBytesResult result{std::vector<uint8_t>(size),
                                 MemoryStatus::Okay};
        result.status = read_into(address, result.data);
        return result;
    }

    MemoryStatus read_into(uint64_t address, std::span<uint8_t> data) {
        if (!callback_) return read_without_callback(address, data);

        Region* region = find_region(address, data.size());
        MemoryAccessEvent event{
            .operation = MemoryOperation::Read,
            .address = address,
            .data = data,
            .byte_enable = {},
            .status = MemoryStatus::Okay,
            .region = region ? std::string_view{region->config.name}
                             : std::string_view{},
        };
        callback_->before_access(event);
        if (event.status == MemoryStatus::Okay) {
            if (!region) {
                event.status = MemoryStatus::DecodeError;
            } else if (!memory_readable(region->config.permission)) {
                event.status = MemoryStatus::SlaveError;
            } else {
                for (std::size_t index = 0; index < data.size(); ++index) {
                    const uint64_t offset = address - region->config.base + index;
                    const auto found = region->bytes.find(offset);
                    event.data[index] = found == region->bytes.end()
                                            ? region->config.fill
                                            : found->second;
                }
            }
        }
        callback_->after_access(event);
        return event.status;
    }

    MemoryWriteResponse write_bytes(uint64_t address,
                                    std::span<const uint8_t> data,
                                    std::span<const uint8_t> byte_enable = {}) {
        if (!byte_enable.empty() && byte_enable.size() != data.size()) {
            throw std::invalid_argument(
                "cpptb-vc: byte-enable count must match write size");
        }
        if (!callback_) {
            return write_without_callback(address, data, byte_enable);
        }
        std::vector<uint8_t> mutable_data{data.begin(), data.end()};
        std::vector<uint8_t> mutable_enable(
            byte_enable.empty() ? data.size() : byte_enable.size(), 1);
        if (!byte_enable.empty()) {
            for (std::size_t index = 0; index < byte_enable.size(); ++index) {
                mutable_enable[index] = byte_enable[index] != 0;
            }
        }

        return write_mutable(address, mutable_data, mutable_enable);
    }

    template <std::unsigned_integral Data, std::integral Address>
    MemoryReadResponse<Data> read_word(Address address) {
        const uint64_t normalized = normalize_address(address);
        std::array<uint8_t, sizeof(Data)> bytes{};
        const MemoryStatus status = read_into(normalized, bytes);
        Data value = 0;
        const auto order = byte_order(normalized, sizeof(Data));
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            const std::size_t shift_index =
                order == MemoryByteOrder::LittleEndian
                    ? index
                    : bytes.size() - 1 - index;
            value |= static_cast<Data>(bytes[index]) << (shift_index * 8u);
        }
        return MemoryReadResponse<Data>{value, status, 0};
    }

    template <std::unsigned_integral Data, std::unsigned_integral ByteEnable,
              std::integral Address>
    MemoryWriteResponse write_word(Address address, Data value,
                                   ByteEnable byte_enable) {
        const uint64_t normalized = normalize_address(address);
        const auto order = byte_order(normalized, sizeof(Data));
        std::array<uint8_t, sizeof(Data)> bytes{};
        std::array<uint8_t, sizeof(Data)> enables{};
        for (std::size_t index = 0; index < sizeof(Data); ++index) {
            const std::size_t shift_index =
                order == MemoryByteOrder::LittleEndian
                    ? index
                    : sizeof(Data) - 1 - index;
            bytes[index] = static_cast<uint8_t>(value >> (shift_index * 8u));
            enables[index] = static_cast<uint8_t>((byte_enable >> index) & 1u);
        }
        if (!callback_) {
            return write_without_callback(normalized, bytes, enables);
        }
        return write_mutable(normalized, bytes, enables);
    }

    template <std::unsigned_integral Data, std::integral Address>
    MemoryWriteResponse write_word(Address address, Data value) {
        constexpr std::size_t bytes = sizeof(Data);
        constexpr uint64_t all_bytes =
            bytes >= 64 ? std::numeric_limits<uint64_t>::max()
                        : (uint64_t{1} << bytes) - 1u;
        return write_word(normalize_address(address), value, all_bytes);
    }

    void load(uint64_t address, std::span<const uint8_t> data) {
        Region* region = require_region(address, data.size());
        for (std::size_t index = 0; index < data.size(); ++index) {
            const uint64_t offset = address - region->config.base + index;
            if (data[index] == region->config.fill) {
                region->bytes.erase(offset);
            } else {
                region->bytes[offset] = data[index];
            }
        }
    }

    void fill(uint64_t address, std::size_t size, uint8_t value) {
        Region* region = require_region(address, size);
        for (std::size_t index = 0; index < size; ++index) {
            const uint64_t offset = address - region->config.base + index;
            if (value == region->config.fill) {
                region->bytes.erase(offset);
            } else {
                region->bytes[offset] = value;
            }
        }
    }

    std::vector<uint8_t> inspect(uint64_t address, std::size_t size) const {
        const Region* region = require_region(address, size);
        std::vector<uint8_t> data(size);
        for (std::size_t index = 0; index < size; ++index) {
            const uint64_t offset = address - region->config.base + index;
            const auto found = region->bytes.find(offset);
            data[index] = found == region->bytes.end() ? region->config.fill
                                                       : found->second;
        }
        return data;
    }

    void load_file(uint64_t address, const std::filesystem::path& path) {
        std::ifstream stream{path, std::ios::binary};
        if (!stream) {
            throw std::runtime_error("cpptb-vc: cannot open memory image: " +
                                     path.string());
        }
        std::vector<uint8_t> data{
            std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}};
        load(address, data);
    }

    void dump_file(uint64_t address, std::size_t size,
                   const std::filesystem::path& path) const {
        const auto data = inspect(address, size);
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        if (!stream) {
            throw std::runtime_error("cpptb-vc: cannot write memory image: " +
                                     path.string());
        }
        stream.write(reinterpret_cast<const char*>(data.data()),
                     static_cast<std::streamsize>(data.size()));
        if (!stream) {
            throw std::runtime_error("cpptb-vc: failed writing memory image: " +
                                     path.string());
        }
    }

   private:
    struct Region {
        MemoryRegionConfig config;
        std::unordered_map<uint64_t, uint8_t> bytes;

        uint64_t end() const noexcept { return config.base + config.size; }
    };

    MemoryStatus read_without_callback(uint64_t address,
                                       std::span<uint8_t> data) {
        Region* region = find_region(address, data.size());
        if (!region) return MemoryStatus::DecodeError;
        if (!memory_readable(region->config.permission)) {
            return MemoryStatus::SlaveError;
        }
        for (std::size_t index = 0; index < data.size(); ++index) {
            const uint64_t offset = address - region->config.base + index;
            const auto found = region->bytes.find(offset);
            data[index] = found == region->bytes.end() ? region->config.fill
                                                       : found->second;
        }
        return MemoryStatus::Okay;
    }

    MemoryWriteResponse write_without_callback(
        uint64_t address, std::span<const uint8_t> data,
        std::span<const uint8_t> byte_enable) {
        Region* region = find_region(address, data.size());
        if (!region) return MemoryWriteResponse{MemoryStatus::DecodeError, 0};
        if (!memory_writable(region->config.permission)) {
            return MemoryWriteResponse{MemoryStatus::SlaveError, 0};
        }
        for (std::size_t index = 0; index < data.size(); ++index) {
            if (!byte_enable.empty() && byte_enable[index] == 0) continue;
            store_byte(*region, address, index, data[index]);
        }
        return MemoryWriteResponse{};
    }

    MemoryWriteResponse write_mutable(uint64_t address,
                                      std::span<uint8_t> data,
                                      std::span<uint8_t> byte_enable) {
        Region* region = find_region(address, data.size());
        MemoryAccessEvent event{
            .operation = MemoryOperation::Write,
            .address = address,
            .data = data,
            .byte_enable = byte_enable,
            .status = MemoryStatus::Okay,
            .region = region ? std::string_view{region->config.name}
                             : std::string_view{},
        };
        if (callback_) callback_->before_access(event);
        if (event.status == MemoryStatus::Okay) {
            if (!region) {
                event.status = MemoryStatus::DecodeError;
            } else if (!memory_writable(region->config.permission)) {
                event.status = MemoryStatus::SlaveError;
            } else {
                for (std::size_t index = 0; index < event.data.size(); ++index) {
                    if (event.byte_enable[index] == 0) continue;
                    store_byte(*region, address, index, event.data[index]);
                }
            }
        }
        if (callback_) callback_->after_access(event);
        return MemoryWriteResponse{event.status, 0};
    }

    static void store_byte(Region& region, uint64_t address,
                           std::size_t index, uint8_t value) {
        const uint64_t offset = address - region.config.base + index;
        if (value == region.config.fill) {
            region.bytes.erase(offset);
        } else {
            region.bytes[offset] = value;
        }
    }

    template <std::integral Address>
    static uint64_t normalize_address(Address address) {
        if constexpr (std::signed_integral<Address>) {
            if (address < 0) {
                throw std::out_of_range(
                    "cpptb-vc: memory address must not be negative");
            }
        }
        return static_cast<uint64_t>(address);
    }

    Region* find_region(uint64_t address, std::size_t size) {
        return const_cast<Region*>(
            std::as_const(*this).find_region(address, size));
    }

    const Region* find_region(uint64_t address, std::size_t size) const {
        if (size > std::numeric_limits<uint64_t>::max() - address) return nullptr;
        const uint64_t end = address + size;
        auto next = regions_.upper_bound(address);
        if (next == regions_.begin()) return nullptr;
        const auto candidate = std::prev(next);
        if (candidate->second.config.base > address ||
            candidate->second.end() < end) {
            return nullptr;
        }
        return std::addressof(candidate->second);
    }

    Region* require_region(uint64_t address, std::size_t size) {
        Region* region = find_region(address, size);
        if (!region) {
            throw std::out_of_range(
                "cpptb-vc: memory range is not contained in one region");
        }
        return region;
    }

    const Region* require_region(uint64_t address, std::size_t size) const {
        const Region* region = find_region(address, size);
        if (!region) {
            throw std::out_of_range(
                "cpptb-vc: memory range is not contained in one region");
        }
        return region;
    }

    MemoryByteOrder byte_order(uint64_t address, std::size_t size) const {
        const Region* region = find_region(address, size);
        return region ? region->config.byte_order
                      : MemoryByteOrder::LittleEndian;
    }

    std::map<uint64_t, Region> regions_;
    MemoryAccessCallback* callback_ = nullptr;
};

template <typename Model, typename Address, typename Data, typename ByteEnable>
concept MemoryReferenceModelFor = requires(Model& model, Address address,
                                           Data data, ByteEnable byte_enable) {
    {
        model.template read_word<Data>(address)
    } -> std::same_as<MemoryReadResponse<Data>>;
    {
        model.template write_word<Data, ByteEnable>(address, data, byte_enable)
    } -> std::same_as<MemoryWriteResponse>;
};

template <typename Model, typename Transaction>
    requires MemoryReferenceModelFor<
        Model, std::remove_cvref_t<decltype(std::declval<Transaction>().address)>,
        std::remove_cvref_t<decltype(std::declval<Transaction>().data)>,
        std::remove_cvref_t<decltype(std::declval<Transaction>().byte_enable)>>
class MemoryPredictor {
   public:
    using address_type =
        std::remove_cvref_t<decltype(std::declval<Transaction>().address)>;
    using data_type =
        std::remove_cvref_t<decltype(std::declval<Transaction>().data)>;
    using byte_enable_type = std::remove_cvref_t<
        decltype(std::declval<Transaction>().byte_enable)>;

    MemoryPredictor(TestContext test, Model& model,
                    std::string label = "memory transaction")
        : test_(std::move(test)), model_(&model), label_(std::move(label)) {}

    void write(const Transaction& observed) {
        Transaction expected = observed;
        if (observed.operation == MemoryOperation::Read) {
            const auto result =
                model_->template read_word<data_type>(observed.address);
            expected.data = result.data;
            expected.status = result.status;
            ++reads_;
        } else {
            const auto result = model_->template write_word<data_type,
                                                             byte_enable_type>(
                observed.address, observed.data, observed.byte_enable);
            expected.status = result.status;
            ++writes_;
        }
        if (!(observed == expected)) ++mismatches_;
        test_.expect_eq(label_, observed, expected);
    }

    uint64_t reads() const noexcept { return reads_; }
    uint64_t writes() const noexcept { return writes_; }
    uint64_t mismatches() const noexcept { return mismatches_; }

   private:
    TestContext test_;
    Model* model_;
    std::string label_;
    uint64_t reads_ = 0;
    uint64_t writes_ = 0;
    uint64_t mismatches_ = 0;
};

template <typename Transaction, typename Model>
auto make_memory_predictor(TestContext test, Model& model,
                           std::string label = "memory transaction") {
    return MemoryPredictor<Model, Transaction>{std::move(test), model,
                                               std::move(label)};
}

}  // namespace cpptb::vc
