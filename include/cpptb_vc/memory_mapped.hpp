#pragma once

#include <concepts>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

#include "cpptb/coro_runtime.hpp"
#include "cpptb/diagnostic.hpp"

namespace cpptb::vc {

enum class MemoryOperation : uint8_t {
    Read,
    Write,
};

enum class AccessPath : uint8_t {
    Frontdoor,
    Backdoor,
};

enum class MemoryStatus : uint8_t {
    Okay,
    SlaveError,
    DecodeError,
    Timeout,
};

inline constexpr std::string_view cpptb_diagnostic_name(
    MemoryOperation operation) noexcept {
    switch (operation) {
        case MemoryOperation::Read:
            return "read";
        case MemoryOperation::Write:
            return "write";
    }
    return "unknown";
}

inline constexpr std::string_view cpptb_diagnostic_name(
    MemoryStatus status) noexcept {
    switch (status) {
        case MemoryStatus::Okay:
            return "okay";
        case MemoryStatus::SlaveError:
            return "slave_error";
        case MemoryStatus::DecodeError:
            return "decode_error";
        case MemoryStatus::Timeout:
            return "timeout";
    }
    return "unknown";
}

inline constexpr bool memory_okay(MemoryStatus status) noexcept {
    return status == MemoryStatus::Okay;
}

template <typename Address, typename Data, typename ByteEnable = uint64_t>
struct MemoryWriteRequest {
    Address address{};
    Data data{};
    ByteEnable byte_enable = std::numeric_limits<ByteEnable>::max();

    friend bool operator==(const MemoryWriteRequest&,
                           const MemoryWriteRequest&) = default;
};

template <typename Address>
struct MemoryReadRequest {
    Address address{};

    friend bool operator==(const MemoryReadRequest&,
                           const MemoryReadRequest&) = default;
};

struct MemoryWriteResponse {
    MemoryStatus status = MemoryStatus::Okay;
    uint32_t wait_cycles = 0;

    bool okay() const noexcept { return memory_okay(status); }
    friend bool operator==(const MemoryWriteResponse&,
                           const MemoryWriteResponse&) = default;
};

template <typename Data>
struct MemoryReadResponse {
    Data data{};
    MemoryStatus status = MemoryStatus::Okay;
    uint32_t wait_cycles = 0;

    bool okay() const noexcept { return memory_okay(status); }
    friend bool operator==(const MemoryReadResponse&,
                           const MemoryReadResponse&) = default;
};

template <typename Address, typename Data, typename ByteEnable = uint64_t>
struct MemoryTransaction {
    MemoryOperation operation = MemoryOperation::Read;
    Address address{};
    Data data{};
    ByteEnable byte_enable = std::numeric_limits<ByteEnable>::max();
    MemoryStatus status = MemoryStatus::Okay;
    uint32_t wait_cycles = 0;

    friend bool operator==(const MemoryTransaction&,
                           const MemoryTransaction&) = default;
};

template <typename Master>
concept MemoryMappedMaster = requires(
    Master& master, const typename Master::write_request_type& write_request,
    const typename Master::read_request_type& read_request) {
    typename Master::address_type;
    typename Master::data_type;
    typename Master::byte_enable_type;
    typename Master::write_request_type;
    typename Master::read_request_type;
    typename Master::write_response_type;
    typename Master::read_response_type;
    {
        master.write(write_request)
    } -> std::same_as<coro::Task<typename Master::write_response_type>>;
    {
        master.read(read_request)
    } -> std::same_as<coro::Task<typename Master::read_response_type>>;
};

}  // namespace cpptb::vc

namespace cpptb {

template <typename Address, typename Data, typename ByteEnable>
struct DiagnosticFormatter<
    vc::MemoryTransaction<Address, Data, ByteEnable>> {
    static std::string format(
        const vc::MemoryTransaction<Address, Data, ByteEnable>& value) {
        const auto address = format_diagnostic(value.address);
        const auto data = format_diagnostic(value.data);
        const auto byte_enable = format_diagnostic(value.byte_enable);
        return "operation=" +
               std::string{vc::cpptb_diagnostic_name(value.operation)} +
               " address=" + address.value_or("?") +
               " data=" + data.value_or("?") +
               " byte_enable=" + byte_enable.value_or("?") +
               " status=" +
               std::string{vc::cpptb_diagnostic_name(value.status)} +
               " wait_cycles=" + std::to_string(value.wait_cycles);
    }
};

}  // namespace cpptb
