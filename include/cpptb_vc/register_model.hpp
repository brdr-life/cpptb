#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "cpptb/test_api.hpp"
#include "cpptb/packed_bits.hpp"
#include "cpptb_vc/memory_mapped.hpp"
#include "cpptb_vc/transaction_recording.hpp"

namespace cpptb::vc {

enum class RegisterAccess : uint8_t {
    None,
    ReadOnly,
    WriteOnly,
    ReadWrite,
    WriteOnce,
    ReadWriteOnce,
};

enum class RegisterReadEffect : uint8_t {
    None,
    Clear,
    Set,
    User,
};

enum class RegisterWriteEffect : uint8_t {
    None,
    WriteOneSet,
    WriteOneClear,
    WriteOneToggle,
    WriteZeroSet,
    WriteZeroClear,
    WriteZeroToggle,
    Clear,
    Set,
    User,
};

enum class RegisterPrediction : uint8_t {
    Direct,
    Read,
    Write,
};

enum class MirrorCheck : uint8_t {
    Disabled,
    Enabled,
};

enum class RegisterEndianness : uint8_t {
    Little,
    Big,
};

template <typename Value, typename TransportResponse,
          typename ValidMask = Value>
struct RegisterReadResponse {
    Value data{};
    TransportResponse transport{};
    ValidMask valid_mask{};
    uint32_t transfers_completed = 0;
    std::optional<uint64_t> failed_address;

    [[nodiscard]] bool okay() const noexcept { return !failed_address.has_value(); }
};

template <typename TransportResponse>
struct RegisterWriteResponse {
    TransportResponse transport{};
    uint32_t transfers_completed = 0;
    std::optional<uint64_t> failed_address;

    [[nodiscard]] bool okay() const noexcept { return !failed_address.has_value(); }
};

template <typename Data, typename TransportResponse>
struct RegisterMemoryReadResponse {
    Data data{};
    TransportResponse transport{};
    AccessPath path = AccessPath::Frontdoor;
    uint64_t transfers_completed = 0;
    std::optional<uint64_t> failed_index;

    [[nodiscard]] bool okay() const noexcept {
        return !failed_index.has_value();
    }
};

template <typename TransportResponse>
struct RegisterMemoryWriteResponse {
    TransportResponse transport{};
    AccessPath path = AccessPath::Frontdoor;
    uint64_t transfers_completed = 0;
    std::optional<uint64_t> failed_index;

    [[nodiscard]] bool okay() const noexcept {
        return !failed_index.has_value();
    }
};

struct RegisterBackdoorSliceDescriptor {
    std::string_view path;
    uint16_t register_lsb = 0;
    uint16_t width = 0;

    [[nodiscard]] constexpr std::string_view hdl_path() const noexcept {
        return path;
    }
};

struct RegisterFieldDescriptor {
    std::string_view name;
    std::string_view path;
    uint16_t lsb = 0;
    uint16_t width = 0;
    RegisterAccess access = RegisterAccess::ReadWrite;
    RegisterReadEffect read_effect = RegisterReadEffect::None;
    RegisterWriteEffect write_effect = RegisterWriteEffect::None;
    uint64_t reset_value = 0;
    uint64_t reset_mask = 0;
    bool volatile_value = false;
    std::span<const RegisterBackdoorSliceDescriptor> backdoor_slices;
};

struct RegisterDescriptor {
    std::string_view name;
    std::string_view path;
    uint64_t address = 0;
    uint16_t width = 32;
    uint16_t access_width = 32;
    RegisterEndianness endianness = RegisterEndianness::Little;
    uint64_t reset_value = 0;
    uint64_t reset_mask = 0;
    std::span<const uint32_t> reset_value_words;
    std::span<const uint32_t> reset_mask_words;
    std::span<const RegisterFieldDescriptor> fields;
    std::span<const RegisterBackdoorSliceDescriptor> backdoor_slices;
};

struct RegisterUserEffectBitContext {
    const RegisterDescriptor& register_descriptor;
    const RegisterFieldDescriptor& field_descriptor;
    uint16_t field_bit = 0;
    bool previous = false;
    bool previous_valid = false;
    bool value = false;
};

struct RegisterUserEffectBitResult {
    bool value = false;
    bool valid = false;
};

struct RegisterUserEffectFieldContext {
    const RegisterDescriptor& register_descriptor;
    const RegisterFieldDescriptor& field_descriptor;
    uint64_t previous = 0;
    uint64_t previous_valid_mask = 0;
    uint64_t value = 0;
};

struct RegisterUserEffectFieldResult {
    uint64_t value = 0;
    uint64_t valid_mask = 0;
};

class RegisterUserEffectPolicy {
   public:
    virtual ~RegisterUserEffectPolicy() = default;

    virtual bool encode_write(
        const RegisterUserEffectBitContext& context) = 0;
    virtual RegisterUserEffectBitResult predict_write(
        const RegisterUserEffectBitContext& context) = 0;
    virtual RegisterUserEffectBitResult predict_read(
        const RegisterUserEffectBitContext& context) = 0;

    virtual uint64_t encode_write_field(
        const RegisterUserEffectFieldContext& context) {
        uint64_t encoded = 0;
        for (uint16_t bit = 0; bit < context.field_descriptor.width; ++bit) {
            const uint64_t mask = uint64_t{1} << bit;
            if (encode_write(RegisterUserEffectBitContext{
                    context.register_descriptor, context.field_descriptor, bit,
                    (context.previous & mask) != 0,
                    (context.previous_valid_mask & mask) != 0,
                    (context.value & mask) != 0})) {
                encoded |= mask;
            }
        }
        return encoded;
    }

    virtual RegisterUserEffectFieldResult predict_write_field(
        const RegisterUserEffectFieldContext& context) {
        RegisterUserEffectFieldResult result;
        for (uint16_t bit = 0; bit < context.field_descriptor.width; ++bit) {
            const uint64_t mask = uint64_t{1} << bit;
            const auto predicted = predict_write(RegisterUserEffectBitContext{
                context.register_descriptor, context.field_descriptor, bit,
                (context.previous & mask) != 0,
                (context.previous_valid_mask & mask) != 0,
                (context.value & mask) != 0});
            if (predicted.value) result.value |= mask;
            if (predicted.valid) result.valid_mask |= mask;
        }
        return result;
    }

    virtual RegisterUserEffectFieldResult predict_read_field(
        const RegisterUserEffectFieldContext& context) {
        RegisterUserEffectFieldResult result;
        for (uint16_t bit = 0; bit < context.field_descriptor.width; ++bit) {
            const uint64_t mask = uint64_t{1} << bit;
            const auto predicted = predict_read(RegisterUserEffectBitContext{
                context.register_descriptor, context.field_descriptor, bit,
                (context.previous & mask) != 0,
                (context.previous_valid_mask & mask) != 0,
                (context.value & mask) != 0});
            if (predicted.value) result.value |= mask;
            if (predicted.valid) result.valid_mask |= mask;
        }
        return result;
    }
};

struct RegisterMemoryDescriptor {
    std::string_view name;
    std::string_view path;
    uint64_t address = 0;
    uint64_t entries = 0;
    uint16_t width = 0;
    uint16_t access_width = 0;
    RegisterAccess access = RegisterAccess::ReadWrite;
    std::string_view hdl_path;
};

struct RegisterBlockDescriptor {
    std::string_view name;
    uint32_t address_unit_bits = 8;
    std::span<const RegisterDescriptor> registers;
    std::span<const RegisterMemoryDescriptor> memories;
};

inline constexpr uint64_t register_mask(uint16_t width) noexcept {
    return width >= 64 ? std::numeric_limits<uint64_t>::max()
                       : width == 0 ? 0 : (uint64_t{1} << width) - 1u;
}

inline constexpr uint64_t register_field_mask(
    const RegisterFieldDescriptor& field) noexcept {
    return register_mask(field.width) << field.lsb;
}

inline constexpr bool register_readable(RegisterAccess access) noexcept {
    return access == RegisterAccess::ReadOnly ||
           access == RegisterAccess::ReadWrite ||
           access == RegisterAccess::ReadWriteOnce;
}

inline constexpr bool register_writable(RegisterAccess access) noexcept {
    return access == RegisterAccess::WriteOnly ||
           access == RegisterAccess::ReadWrite ||
           access == RegisterAccess::WriteOnce ||
           access == RegisterAccess::ReadWriteOnce;
}

template <std::unsigned_integral Data>
class RegisterBackdoor {
   public:
    virtual ~RegisterBackdoor() = default;
    virtual Data peek(const RegisterDescriptor& descriptor,
                      uint64_t effective_address) = 0;
    virtual void poke(const RegisterDescriptor& descriptor,
                      uint64_t effective_address, Data value) = 0;
};

class WideRegisterBackdoor {
   public:
    virtual ~WideRegisterBackdoor() = default;

    virtual void peek_words(const RegisterDescriptor& descriptor,
                            uint64_t effective_address,
                            std::span<uint32_t> words) = 0;
    virtual void poke_words(const RegisterDescriptor& descriptor,
                            uint64_t effective_address,
                            std::span<const uint32_t> words) = 0;
};

class WideRegisterMemoryBackdoor {
   public:
    virtual ~WideRegisterMemoryBackdoor() = default;

    virtual void peek_words(const RegisterMemoryDescriptor& descriptor,
                            uint64_t index, uint64_t effective_address,
                            std::span<uint32_t> words) = 0;
    virtual void poke_words(const RegisterMemoryDescriptor& descriptor,
                            uint64_t index, uint64_t effective_address,
                            std::span<const uint32_t> words) = 0;

    virtual void peek_elements(const RegisterMemoryDescriptor& descriptor,
                               uint64_t first_index,
                               uint64_t first_effective_address,
                               std::span<uint32_t> words,
                               std::size_t words_per_element) {
        const uint64_t element_bytes = descriptor.width / 8u;
        const std::size_t count = words.size() / words_per_element;
        for (std::size_t offset = 0; offset < count; ++offset) {
            peek_words(descriptor, first_index + offset,
                       first_effective_address + offset * element_bytes,
                       words.subspan(offset * words_per_element,
                                     words_per_element));
        }
    }

    virtual void poke_elements(const RegisterMemoryDescriptor& descriptor,
                               uint64_t first_index,
                               uint64_t first_effective_address,
                               std::span<const uint32_t> words,
                               std::size_t words_per_element) {
        const uint64_t element_bytes = descriptor.width / 8u;
        const std::size_t count = words.size() / words_per_element;
        for (std::size_t offset = 0; offset < count; ++offset) {
            poke_words(descriptor, first_index + offset,
                       first_effective_address + offset * element_bytes,
                       words.subspan(offset * words_per_element,
                                     words_per_element));
        }
    }
};

template <MemoryMappedMaster Master>
    requires std::unsigned_integral<typename Master::data_type>
class RegisterFrontdoor {
   public:
    using write_request_type = typename Master::write_request_type;
    using read_request_type = typename Master::read_request_type;
    using write_response_type = typename Master::write_response_type;
    using read_response_type = typename Master::read_response_type;

    virtual ~RegisterFrontdoor() = default;
    virtual coro::Task<write_response_type> write(
        Master& master, const RegisterDescriptor& descriptor,
        write_request_type request) = 0;
    virtual coro::Task<read_response_type> read(
        Master& master, const RegisterDescriptor& descriptor,
        read_request_type request) = 0;
};

template <MemoryMappedMaster Master>
    requires std::unsigned_integral<typename Master::data_type>
class RegisterMemoryFrontdoor {
   public:
    using write_request_type = typename Master::write_request_type;
    using read_request_type = typename Master::read_request_type;
    using write_response_type = typename Master::write_response_type;
    using read_response_type = typename Master::read_response_type;

    virtual ~RegisterMemoryFrontdoor() = default;
    virtual coro::Task<write_response_type> write(
        Master& master, const RegisterMemoryDescriptor& descriptor,
        uint64_t index, write_request_type request) = 0;
    virtual coro::Task<read_response_type> read(
        Master& master, const RegisterMemoryDescriptor& descriptor,
        uint64_t index, read_request_type request) = 0;
};

template <MemoryMappedMaster Master>
    requires std::unsigned_integral<typename Master::data_type>
class RegisterAddressMap {
   public:
    using address_type = typename Master::address_type;
    using frontdoor_type = RegisterFrontdoor<Master>;
    using memory_frontdoor_type = RegisterMemoryFrontdoor<Master>;

    RegisterAddressMap(std::string name, Master& master,
                       uint64_t base_address = 0)
        : name_(std::move(name)), master_(&master), base_address_(base_address) {
        if (name_.empty()) {
            throw std::invalid_argument(
                "cpptb-vc: register address-map name is empty");
        }
    }

    RegisterAddressMap& route(const RegisterDescriptor& descriptor,
                              uint64_t offset,
                              frontdoor_type* frontdoor = nullptr) {
        const auto found = std::find_if(
            routes_.begin(), routes_.end(), [&](const Route& candidate) {
                return candidate.descriptor == &descriptor;
            });
        if (found == routes_.end()) {
            routes_.push_back(Route{&descriptor, offset, frontdoor});
            try {
                static_cast<void>(effective_address(descriptor));
            } catch (...) {
                routes_.pop_back();
                throw;
            }
        } else {
            const auto previous = *found;
            found->offset = offset;
            found->frontdoor = frontdoor;
            try {
                static_cast<void>(effective_address(descriptor));
            } catch (...) {
                *found = previous;
                throw;
            }
        }
        return *this;
    }

    RegisterAddressMap& frontdoor(const RegisterDescriptor& descriptor,
                                  frontdoor_type& frontdoor) {
        const auto* selected = find_route(descriptor);
        return route(descriptor,
                     selected ? selected->offset : descriptor.address,
                     &frontdoor);
    }

    RegisterAddressMap& route(
        const RegisterMemoryDescriptor& descriptor, uint64_t offset,
        memory_frontdoor_type* frontdoor = nullptr) {
        const auto found = std::find_if(
            memory_routes_.begin(), memory_routes_.end(),
            [&](const MemoryRoute& candidate) {
                return candidate.descriptor == &descriptor;
            });
        if (found == memory_routes_.end()) {
            memory_routes_.push_back(
                MemoryRoute{&descriptor, offset, frontdoor});
            try {
                static_cast<void>(effective_address(descriptor, 0));
            } catch (...) {
                memory_routes_.pop_back();
                throw;
            }
        } else {
            const auto previous = *found;
            found->offset = offset;
            found->frontdoor = frontdoor;
            try {
                static_cast<void>(effective_address(descriptor, 0));
            } catch (...) {
                *found = previous;
                throw;
            }
        }
        return *this;
    }

    RegisterAddressMap& frontdoor(
        const RegisterMemoryDescriptor& descriptor,
        memory_frontdoor_type& frontdoor) {
        const auto* selected = find_memory_route(descriptor);
        return route(descriptor,
                     selected ? selected->offset : descriptor.address,
                     &frontdoor);
    }

    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    [[nodiscard]] uint64_t base_address() const noexcept {
        return base_address_;
    }
    [[nodiscard]] Master& master() const noexcept { return *master_; }

    [[nodiscard]] uint64_t effective_address(
        const RegisterDescriptor& descriptor) const {
        const Route* route = find_route(descriptor);
        const uint64_t offset = route ? route->offset : descriptor.address;
        if (offset > std::numeric_limits<uint64_t>::max() - base_address_) {
            throw std::overflow_error(
                "cpptb-vc: register address map overflows for " +
                std::string{descriptor.path} + " in map " + name_);
        }
        const uint64_t address = base_address_ + offset;
        if constexpr (std::integral<address_type>) {
            if (address > static_cast<uint64_t>(
                              std::numeric_limits<address_type>::max())) {
                throw std::overflow_error(
                    "cpptb-vc: register address exceeds frontdoor width for " +
                    std::string{descriptor.path} + " in map " + name_);
            }
        }
        return address;
    }

    [[nodiscard]] uint64_t effective_address(
        const RegisterMemoryDescriptor& descriptor, uint64_t index) const {
        if (descriptor.width == 0 || descriptor.width % 8u != 0) {
            throw std::logic_error(
                "cpptb-vc: register memory has invalid element width: " +
                std::string{descriptor.path});
        }
        if (index >= descriptor.entries) {
            throw std::out_of_range(
                "cpptb-vc: register memory address-map index out of bounds: " +
                std::string{descriptor.path} + " in map " + name_);
        }
        const MemoryRoute* route = find_memory_route(descriptor);
        const uint64_t offset = route ? route->offset : descriptor.address;
        const uint64_t bytes = descriptor.width / 8u;
        if (index > (std::numeric_limits<uint64_t>::max() - offset) / bytes ||
            offset + index * bytes >
                std::numeric_limits<uint64_t>::max() - base_address_) {
            throw std::overflow_error(
                "cpptb-vc: register memory address map overflows for " +
                std::string{descriptor.path} + " in map " + name_);
        }
        const uint64_t address = base_address_ + offset + index * bytes;
        if constexpr (std::integral<address_type>) {
            if (address > static_cast<uint64_t>(
                              std::numeric_limits<address_type>::max())) {
                throw std::overflow_error(
                    "cpptb-vc: register memory address exceeds frontdoor "
                    "width for " +
                    std::string{descriptor.path} + " in map " + name_);
            }
        }
        return address;
    }

    coro::Task<typename Master::write_response_type> write(
        const RegisterDescriptor& descriptor,
        typename Master::write_request_type request) {
        const Route* route = find_route(descriptor);
        if (route && route->frontdoor) {
            co_return co_await route->frontdoor->write(*master_, descriptor,
                                                       request);
        }
        co_return co_await master_->write(request);
    }

    coro::Task<typename Master::read_response_type> read(
        const RegisterDescriptor& descriptor,
        typename Master::read_request_type request) {
        const Route* route = find_route(descriptor);
        if (route && route->frontdoor) {
            co_return co_await route->frontdoor->read(*master_, descriptor,
                                                      request);
        }
        co_return co_await master_->read(request);
    }

    coro::Task<typename Master::write_response_type> write(
        const RegisterMemoryDescriptor& descriptor, uint64_t index,
        typename Master::write_request_type request) {
        const MemoryRoute* route = find_memory_route(descriptor);
        if (route && route->frontdoor) {
            co_return co_await route->frontdoor->write(
                *master_, descriptor, index, request);
        }
        co_return co_await master_->write(request);
    }

    coro::Task<typename Master::read_response_type> read(
        const RegisterMemoryDescriptor& descriptor, uint64_t index,
        typename Master::read_request_type request) {
        const MemoryRoute* route = find_memory_route(descriptor);
        if (route && route->frontdoor) {
            co_return co_await route->frontdoor->read(
                *master_, descriptor, index, request);
        }
        co_return co_await master_->read(request);
    }

   private:
    struct Route {
        const RegisterDescriptor* descriptor;
        uint64_t offset;
        frontdoor_type* frontdoor;
    };

    struct MemoryRoute {
        const RegisterMemoryDescriptor* descriptor;
        uint64_t offset;
        memory_frontdoor_type* frontdoor;
    };

    [[nodiscard]] const Route* find_route(
        const RegisterDescriptor& descriptor) const noexcept {
        const auto found = std::find_if(
            routes_.begin(), routes_.end(), [&](const Route& candidate) {
                return candidate.descriptor == &descriptor;
            });
        return found == routes_.end() ? nullptr : &*found;
    }

    [[nodiscard]] const MemoryRoute* find_memory_route(
        const RegisterMemoryDescriptor& descriptor) const noexcept {
        const auto found = std::find_if(
            memory_routes_.begin(), memory_routes_.end(),
            [&](const MemoryRoute& candidate) {
                return candidate.descriptor == &descriptor;
            });
        return found == memory_routes_.end() ? nullptr : &*found;
    }

    std::string name_;
    Master* master_;
    uint64_t base_address_ = 0;
    std::vector<Route> routes_;
    std::vector<MemoryRoute> memory_routes_;
};

template <std::unsigned_integral Data>
class RegisterMemoryBackdoor {
   public:
    virtual ~RegisterMemoryBackdoor() = default;

    virtual Data peek(const RegisterMemoryDescriptor& descriptor,
                      uint64_t index, uint64_t effective_address) = 0;
    virtual void poke(const RegisterMemoryDescriptor& descriptor,
                      uint64_t index, uint64_t effective_address,
                      Data value) = 0;

    virtual void peek_into(const RegisterMemoryDescriptor& descriptor,
                           uint64_t first_index,
                           uint64_t first_effective_address,
                           std::span<Data> values) {
        const uint64_t element_bytes = descriptor.width / 8u;
        for (uint64_t offset = 0; offset < values.size(); ++offset) {
            values[static_cast<std::size_t>(offset)] = peek(
                descriptor, first_index + offset,
                first_effective_address + offset * element_bytes);
        }
    }

    virtual void poke(const RegisterMemoryDescriptor& descriptor,
                      uint64_t first_index,
                      uint64_t first_effective_address,
                      std::span<const Data> values) {
        const uint64_t element_bytes = descriptor.width / 8u;
        for (uint64_t offset = 0; offset < values.size(); ++offset) {
            poke(descriptor, first_index + offset,
                 first_effective_address + offset * element_bytes,
                 values[static_cast<std::size_t>(offset)]);
        }
    }
};

namespace register_detail {

inline void validate_field_layout(const RegisterDescriptor& descriptor) {
    for (std::size_t index = 0; index < descriptor.fields.size(); ++index) {
        const auto& field = descriptor.fields[index];
        if (field.width == 0 || field.lsb >= descriptor.width ||
            field.width > descriptor.width - field.lsb) {
            throw std::invalid_argument(
                "cpptb-vc: register field lies outside its register: " +
                std::string{field.path});
        }
        const uint32_t field_end =
            static_cast<uint32_t>(field.lsb) + field.width;
        for (std::size_t previous_index = 0; previous_index < index;
             ++previous_index) {
            const auto& previous = descriptor.fields[previous_index];
            const uint32_t previous_end =
                static_cast<uint32_t>(previous.lsb) + previous.width;
            if (field.lsb < previous_end && previous.lsb < field_end) {
                throw std::invalid_argument(
                    "cpptb-vc: register fields overlap: " +
                    std::string{previous.path} + " and " +
                    std::string{field.path});
            }
        }
    }
}

template <typename T>
class ReadyOrTask {
   public:
    explicit ReadyOrTask(T value) : value_(std::move(value)) {}
    explicit ReadyOrTask(coro::Task<T> task) : task_(std::move(task)) {}

    ReadyOrTask(const ReadyOrTask&) = delete;
    ReadyOrTask& operator=(const ReadyOrTask&) = delete;
    ReadyOrTask(ReadyOrTask&&) noexcept = default;
    ReadyOrTask& operator=(ReadyOrTask&&) noexcept = default;

    class Awaiter {
       public:
        explicit Awaiter(ReadyOrTask&& operation)
            : value_(std::move(operation.value_)) {
            if (operation.task_) {
                task_.emplace(
                    std::move(*operation.task_).operator co_await());
            }
        }

        [[nodiscard]] bool await_ready() const noexcept {
            return value_.has_value();
        }

        template <typename Promise>
            requires std::derived_from<Promise, coro::TaskPromiseBase>
        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<Promise> parent) {
            return task_->await_suspend(parent);
        }

        T await_resume() {
            if (value_) return std::move(*value_);
            return task_->await_resume();
        }

       private:
        std::optional<T> value_;
        std::optional<typename coro::Task<T>::Awaiter> task_;
    };

    Awaiter operator co_await() && { return Awaiter{std::move(*this)}; }

   private:
    std::optional<T> value_;
    std::optional<coro::Task<T>> task_;
};

template <typename Value>
[[nodiscard]] constexpr uint64_t hdl_value_to_uint64(Value value) {
    if constexpr (std::integral<Value>) {
        return static_cast<uint64_t>(value);
    } else if constexpr (requires { value.signal_value(); }) {
        return static_cast<uint64_t>(value.signal_value());
    } else {
        return value.to_uint64();
    }
}

template <typename Signal>
[[nodiscard]] constexpr typename Signal::value_type hdl_value_from_uint64(
    uint64_t value) {
    using Value = typename Signal::value_type;
    if constexpr (std::integral<Value>) {
        return static_cast<Value>(value);
    } else {
        return Value::from_signal_value(
            static_cast<typename Signal::raw_value_type>(value));
    }
}

template <uint16_t ExpectedWidth, typename Signal>
[[nodiscard]] uint64_t read_hdl_full(const Signal& signal) {
    static_assert(Signal::width == ExpectedWidth,
                  "SystemRDL HDL path width does not match the register slice");
    return hdl_value_to_uint64(signal.get()) & register_mask(ExpectedWidth);
}

template <uint16_t ExpectedWidth, typename Signal>
void write_hdl_full(const Signal& signal, uint64_t value) {
    static_assert(Signal::width == ExpectedWidth,
                  "SystemRDL HDL path width does not match the register slice");
    signal.deposit(hdl_value_from_uint64<Signal>(value));
}

template <int32_t Index, uint16_t ExpectedWidth, typename Object>
[[nodiscard]] uint64_t read_hdl_select(const Object& object) {
    if constexpr (requires { object.at(Index).get(); }) {
        const auto element = object.at(Index);
        static_assert(Object::width == ExpectedWidth,
                      "SystemRDL HDL array element width does not match the "
                      "register slice");
        return hdl_value_to_uint64(element.get()) &
               register_mask(ExpectedWidth);
    } else {
        static_assert(ExpectedWidth == 1,
                      "a packed bit select maps exactly one register bit");
        static_assert(Index >= 0 &&
                          static_cast<std::size_t>(Index) < Object::width,
                      "SystemRDL HDL bit select is out of range");
        return (hdl_value_to_uint64(object.get()) >> Index) & 1u;
    }
}

template <int32_t Index, uint16_t ExpectedWidth, typename Object>
void write_hdl_select(const Object& object, uint64_t value) {
    if constexpr (requires { object.at(Index).deposit(
                                hdl_value_from_uint64<
                                    decltype(object.at(Index))>(value)); }) {
        const auto element = object.at(Index);
        static_assert(Object::width == ExpectedWidth,
                      "SystemRDL HDL array element width does not match the "
                      "register slice");
        element.deposit(hdl_value_from_uint64<decltype(element)>(value));
    } else {
        static_assert(ExpectedWidth == 1,
                      "a packed bit select maps exactly one register bit");
        static_assert(Index >= 0 &&
                          static_cast<std::size_t>(Index) < Object::width,
                      "SystemRDL HDL bit select is out of range");
        uint64_t current = hdl_value_to_uint64(object.get());
        current = (current & ~(uint64_t{1} << Index)) |
                  ((value & 1u) << Index);
        object.deposit(hdl_value_from_uint64<Object>(current));
    }
}

template <int32_t Msb, int32_t Lsb, uint16_t ExpectedWidth, typename Signal>
[[nodiscard]] uint64_t read_hdl_range(const Signal& signal) {
    static_assert(Msb >= Lsb && Lsb >= 0,
                  "SystemRDL HDL packed range must be descending");
    static_assert(static_cast<std::size_t>(Msb) < Signal::width,
                  "SystemRDL HDL packed range is out of range");
    static_assert(Msb - Lsb + 1 == ExpectedWidth,
                  "SystemRDL HDL packed range width does not match the "
                  "register slice");
    return (hdl_value_to_uint64(signal.get()) >> Lsb) &
           register_mask(ExpectedWidth);
}

template <int32_t Msb, int32_t Lsb, uint16_t ExpectedWidth, typename Signal>
void write_hdl_range(const Signal& signal, uint64_t value) {
    static_assert(Msb >= Lsb && Lsb >= 0,
                  "SystemRDL HDL packed range must be descending");
    static_assert(static_cast<std::size_t>(Msb) < Signal::width,
                  "SystemRDL HDL packed range is out of range");
    static_assert(Msb - Lsb + 1 == ExpectedWidth,
                  "SystemRDL HDL packed range width does not match the "
                  "register slice");
    const uint64_t mask = register_mask(ExpectedWidth) << Lsb;
    uint64_t current = hdl_value_to_uint64(signal.get());
    current = (current & ~mask) | ((value << Lsb) & mask);
    signal.deposit(hdl_value_from_uint64<Signal>(current));
}

inline bool word_span_bit(std::span<const uint32_t> words,
                          std::size_t index) {
    if (index >= words.size() * 32u) {
        throw std::out_of_range(
            "cpptb-vc: generated wide backdoor word span is too small");
    }
    return ((words[index / 32u] >> (index % 32u)) & 1u) != 0;
}

inline void set_word_span_bit(std::span<uint32_t> words, std::size_t index,
                              bool value) {
    if (index >= words.size() * 32u) {
        throw std::out_of_range(
            "cpptb-vc: generated wide backdoor word span is too small");
    }
    const uint32_t mask = uint32_t{1} << (index % 32u);
    if (value) {
        words[index / 32u] |= mask;
    } else {
        words[index / 32u] &= ~mask;
    }
}

template <typename Value>
[[nodiscard]] constexpr bool hdl_value_bit(const Value& value,
                                           std::size_t index) {
    if constexpr (std::integral<Value>) {
        constexpr std::size_t bit_count = sizeof(Value) * 8u;
        return index < bit_count &&
               ((static_cast<uint64_t>(value) >> index) & uint64_t{1}) != 0;
    } else if constexpr (requires { value.bit(index); }) {
        return value.bit(index);
    } else if constexpr (requires { value.signal_value(); }) {
        using Raw = decltype(value.signal_value());
        constexpr std::size_t bit_count = sizeof(Raw) * 8u;
        return index < bit_count &&
               ((static_cast<uint64_t>(value.signal_value()) >> index) &
                uint64_t{1}) != 0;
    } else {
        static_assert(sizeof(Value) == 0,
                      "HDL value does not provide packed bit access");
    }
}

template <typename Value>
constexpr void set_hdl_value_bit(Value& value, std::size_t index, bool bit) {
    if constexpr (std::integral<Value>) {
        constexpr std::size_t bit_count = sizeof(Value) * 8u;
        if (index >= bit_count) {
            throw std::out_of_range(
                "cpptb-vc: generated HDL bit index is out of range");
        }
        uint64_t raw = static_cast<uint64_t>(value);
        const uint64_t mask = uint64_t{1} << index;
        raw = bit ? raw | mask : raw & ~mask;
        value = static_cast<Value>(raw);
    } else if constexpr (requires { value.set_bit(index, bit); }) {
        value.set_bit(index, bit);
    } else {
        static_assert(sizeof(Value) == 0,
                      "HDL value does not provide mutable packed bit access");
    }
}

template <uint16_t ExpectedWidth, typename Signal>
void read_hdl_full_words(const Signal& signal, std::span<uint32_t> words,
                         std::size_t register_lsb) {
    static_assert(Signal::width == ExpectedWidth,
                  "SystemRDL HDL path width does not match the register slice");
    const auto value = signal.get();
    for (std::size_t bit = 0; bit < ExpectedWidth; ++bit) {
        set_word_span_bit(words, register_lsb + bit,
                          hdl_value_bit(value, bit));
    }
}

template <uint16_t ExpectedWidth, typename Signal>
void write_hdl_full_words(const Signal& signal,
                          std::span<const uint32_t> words,
                          std::size_t register_lsb) {
    static_assert(Signal::width == ExpectedWidth,
                  "SystemRDL HDL path width does not match the register slice");
    auto value = signal.get();
    for (std::size_t bit = 0; bit < ExpectedWidth; ++bit) {
        set_hdl_value_bit(value, bit,
                          word_span_bit(words, register_lsb + bit));
    }
    signal.deposit(value);
}

template <int32_t Index, uint16_t ExpectedWidth, typename Object>
void read_hdl_select_words(const Object& object, std::span<uint32_t> words,
                           std::size_t register_lsb) {
    if constexpr (requires { object.at(Index).get(); }) {
        const auto element = object.at(Index);
        read_hdl_full_words<ExpectedWidth>(element, words, register_lsb);
    } else {
        static_assert(ExpectedWidth == 1,
                      "a packed bit select maps exactly one register bit");
        static_assert(Index >= 0 &&
                          static_cast<std::size_t>(Index) < Object::width,
                      "SystemRDL HDL bit select is out of range");
        set_word_span_bit(words, register_lsb,
                          hdl_value_bit(object.get(), Index));
    }
}

template <int32_t Index, uint16_t ExpectedWidth, typename Object>
void write_hdl_select_words(const Object& object,
                            std::span<const uint32_t> words,
                            std::size_t register_lsb) {
    if constexpr (requires { object.at(Index).get(); }) {
        const auto element = object.at(Index);
        write_hdl_full_words<ExpectedWidth>(element, words, register_lsb);
    } else {
        static_assert(ExpectedWidth == 1,
                      "a packed bit select maps exactly one register bit");
        static_assert(Index >= 0 &&
                          static_cast<std::size_t>(Index) < Object::width,
                      "SystemRDL HDL bit select is out of range");
        auto value = object.get();
        set_hdl_value_bit(value, Index, word_span_bit(words, register_lsb));
        object.deposit(value);
    }
}

template <int32_t Msb, int32_t Lsb, uint16_t ExpectedWidth, typename Signal>
void read_hdl_range_words(const Signal& signal, std::span<uint32_t> words,
                          std::size_t register_lsb) {
    static_assert(Msb >= Lsb && Lsb >= 0,
                  "SystemRDL HDL packed range must be descending");
    static_assert(static_cast<std::size_t>(Msb) < Signal::width,
                  "SystemRDL HDL packed range is out of range");
    static_assert(Msb - Lsb + 1 == ExpectedWidth,
                  "SystemRDL HDL packed range width does not match the register slice");
    const auto value = signal.get();
    for (std::size_t bit = 0; bit < ExpectedWidth; ++bit) {
        set_word_span_bit(words, register_lsb + bit,
                          hdl_value_bit(value, Lsb + bit));
    }
}

template <int32_t Msb, int32_t Lsb, uint16_t ExpectedWidth, typename Signal>
void write_hdl_range_words(const Signal& signal,
                           std::span<const uint32_t> words,
                           std::size_t register_lsb) {
    static_assert(Msb >= Lsb && Lsb >= 0,
                  "SystemRDL HDL packed range must be descending");
    static_assert(static_cast<std::size_t>(Msb) < Signal::width,
                  "SystemRDL HDL packed range is out of range");
    static_assert(Msb - Lsb + 1 == ExpectedWidth,
                  "SystemRDL HDL packed range width does not match the register slice");
    auto value = signal.get();
    for (std::size_t bit = 0; bit < ExpectedWidth; ++bit) {
        set_hdl_value_bit(value, Lsb + bit,
                          word_span_bit(words, register_lsb + bit));
    }
    signal.deposit(value);
}

class LockGuard {
   public:
    explicit LockGuard(coro::Lock& lock) noexcept : lock_(&lock) {}
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
    ~LockGuard() {
        if (lock_) lock_->release();
    }

   private:
    coro::Lock* lock_;
};

inline constexpr uint64_t insert_field(uint64_t value, uint64_t field_value,
                                       const RegisterFieldDescriptor& field) {
    const uint64_t mask = register_field_mask(field);
    return (value & ~mask) | ((field_value << field.lsb) & mask);
}

inline constexpr uint64_t extract_field(
    uint64_t value, const RegisterFieldDescriptor& field) {
    return (value >> field.lsb) & register_mask(field.width);
}

inline constexpr uint64_t apply_write_effect(
    uint64_t previous, uint64_t written,
    const RegisterFieldDescriptor& field) noexcept {
    const uint64_t mask = register_mask(field.width);
    const uint64_t old_field = extract_field(previous, field);
    const uint64_t write_field = extract_field(written, field);
    uint64_t next = write_field;
    switch (field.write_effect) {
        case RegisterWriteEffect::None:
            next = write_field;
            break;
        case RegisterWriteEffect::WriteOneSet:
            next = old_field | write_field;
            break;
        case RegisterWriteEffect::WriteOneClear:
            next = old_field & ~write_field;
            break;
        case RegisterWriteEffect::WriteOneToggle:
            next = old_field ^ write_field;
            break;
        case RegisterWriteEffect::WriteZeroSet:
            next = old_field | (~write_field & mask);
            break;
        case RegisterWriteEffect::WriteZeroClear:
            next = old_field & write_field;
            break;
        case RegisterWriteEffect::WriteZeroToggle:
            next = old_field ^ (~write_field & mask);
            break;
        case RegisterWriteEffect::Clear:
            next = 0;
            break;
        case RegisterWriteEffect::Set:
            next = mask;
            break;
        case RegisterWriteEffect::User:
            next = write_field;
            break;
    }
    return insert_field(previous, next, field);
}

inline constexpr uint64_t apply_write_valid_mask(
    uint64_t previous_valid, uint64_t written,
    const RegisterFieldDescriptor& field) noexcept {
    const uint64_t mask = register_mask(field.width);
    const uint64_t old_valid = extract_field(previous_valid, field);
    const uint64_t write_field = extract_field(written, field);
    uint64_t next_valid = mask;
    switch (field.write_effect) {
        case RegisterWriteEffect::None:
        case RegisterWriteEffect::Clear:
        case RegisterWriteEffect::Set:
            next_valid = mask;
            break;
        case RegisterWriteEffect::WriteOneSet:
        case RegisterWriteEffect::WriteOneClear:
            next_valid = old_valid | write_field;
            break;
        case RegisterWriteEffect::WriteOneToggle:
        case RegisterWriteEffect::WriteZeroToggle:
            next_valid = old_valid;
            break;
        case RegisterWriteEffect::WriteZeroSet:
        case RegisterWriteEffect::WriteZeroClear:
            next_valid = old_valid | (~write_field & mask);
            break;
        case RegisterWriteEffect::User:
            next_valid = 0;
            break;
    }
    return insert_field(previous_valid, next_valid, field);
}

inline constexpr uint64_t apply_read_valid_mask(
    uint64_t previous_valid,
    const RegisterFieldDescriptor& field) noexcept {
    const uint64_t next_valid =
        field.read_effect == RegisterReadEffect::User
            ? 0
            : register_mask(field.width);
    return insert_field(previous_valid, next_valid, field);
}

inline constexpr uint64_t encode_desired_write(
    uint64_t previous, uint64_t desired,
    const RegisterFieldDescriptor& field) noexcept {
    const uint64_t mask = register_mask(field.width);
    const uint64_t old_field = extract_field(previous, field);
    const uint64_t desired_field = extract_field(desired, field);
    uint64_t written = desired_field;
    switch (field.write_effect) {
        case RegisterWriteEffect::None:
        case RegisterWriteEffect::User:
            written = desired_field;
            break;
        case RegisterWriteEffect::WriteOneSet:
            written = desired_field & ~old_field;
            break;
        case RegisterWriteEffect::WriteOneClear:
            written = old_field & ~desired_field;
            break;
        case RegisterWriteEffect::WriteOneToggle:
            written = old_field ^ desired_field;
            break;
        case RegisterWriteEffect::WriteZeroSet:
            written = old_field | ~desired_field;
            break;
        case RegisterWriteEffect::WriteZeroClear:
            written = ~old_field | desired_field;
            break;
        case RegisterWriteEffect::WriteZeroToggle:
            written = ~(old_field ^ desired_field);
            break;
        case RegisterWriteEffect::Clear:
        case RegisterWriteEffect::Set:
            written = 0;
            break;
    }
    return insert_field(0, written & mask, field);
}

inline constexpr uint64_t encode_desired_write(
    uint64_t previous, uint64_t previous_valid, uint64_t desired,
    const RegisterFieldDescriptor& field) noexcept {
    const uint64_t mask = register_mask(field.width);
    const uint64_t old_field = extract_field(previous, field);
    const uint64_t old_valid = extract_field(previous_valid, field);
    const uint64_t desired_field = extract_field(desired, field);
    uint64_t written = desired_field;
    switch (field.write_effect) {
        case RegisterWriteEffect::None:
        case RegisterWriteEffect::User:
            written = desired_field;
            break;
        case RegisterWriteEffect::WriteOneSet:
            written = desired_field & (~old_field | ~old_valid);
            break;
        case RegisterWriteEffect::WriteOneClear:
            written = ~desired_field & (old_field | ~old_valid);
            break;
        case RegisterWriteEffect::WriteOneToggle:
            written = old_valid & (old_field ^ desired_field);
            break;
        case RegisterWriteEffect::WriteZeroSet:
            written = (old_field & old_valid) | ~desired_field;
            break;
        case RegisterWriteEffect::WriteZeroClear:
            written = (~old_field & old_valid) | desired_field;
            break;
        case RegisterWriteEffect::WriteZeroToggle:
            written = (old_valid & ~(old_field ^ desired_field)) | ~old_valid;
            break;
        case RegisterWriteEffect::Clear:
        case RegisterWriteEffect::Set:
            written = 0;
            break;
    }
    return insert_field(0, written & mask, field);
}

inline constexpr uint64_t apply_read_effect(
    uint64_t sampled, const RegisterFieldDescriptor& field) noexcept {
    switch (field.read_effect) {
        case RegisterReadEffect::None:
        case RegisterReadEffect::User:
            return sampled;
        case RegisterReadEffect::Clear:
            return insert_field(sampled, 0, field);
        case RegisterReadEffect::Set:
            return insert_field(sampled, register_mask(field.width), field);
    }
    return sampled;
}

}  // namespace register_detail

template <MemoryMappedMaster Master>
    requires std::unsigned_integral<typename Master::data_type>
class RegisterHandle;

template <MemoryMappedMaster Master>
    requires std::unsigned_integral<typename Master::data_type>
class RegisterFieldHandle {
   public:
    using data_type = uint64_t;
    using transport_read_response_type = typename Master::read_response_type;
    using read_response_type = RegisterReadResponse<
        data_type, typename Master::read_response_type>;
    using write_response_type =
        RegisterWriteResponse<typename Master::write_response_type>;

    RegisterFieldHandle(RegisterHandle<Master>& parent,
                        const RegisterFieldDescriptor& descriptor)
        : parent_(&parent), descriptor_(&descriptor) {}

    data_type desired() const {
        return static_cast<data_type>(register_detail::extract_field(
            parent_->desired(), *descriptor_));
    }

    data_type mirrored() const {
        return static_cast<data_type>(register_detail::extract_field(
            parent_->mirrored(), *descriptor_));
    }

    data_type desired_valid_mask() const {
        return static_cast<data_type>(register_detail::extract_field(
            parent_->desired_valid_mask(), *descriptor_));
    }

    data_type mirrored_valid_mask() const {
        return static_cast<data_type>(register_detail::extract_field(
            parent_->mirrored_valid_mask(), *descriptor_));
    }

    void set_desired(data_type value) {
        if (!register_writable(descriptor_->access)) {
            throw std::logic_error("cpptb-vc: field is not writable: " +
                                   std::string{descriptor_->path});
        }
        parent_->set_field_desired(value, *descriptor_);
    }

    coro::Task<read_response_type> read() {
        auto response = co_await parent_->read();
        response.data = static_cast<data_type>(
            register_detail::extract_field(response.data, *descriptor_));
        response.valid_mask = register_detail::extract_field(
            response.valid_mask, *descriptor_);
        co_return response;
    }

    coro::Task<read_response_type> read(RegisterAddressMap<Master>& map) {
        auto response = co_await parent_->read(map);
        response.data = static_cast<data_type>(
            register_detail::extract_field(response.data, *descriptor_));
        response.valid_mask = register_detail::extract_field(
            response.valid_mask, *descriptor_);
        co_return response;
    }

    coro::Task<write_response_type> write(data_type value) {
        set_desired(value);
        co_return co_await parent_->update();
    }

    coro::Task<write_response_type> write(data_type value,
                                          RegisterAddressMap<Master>& map) {
        set_desired(value);
        co_return co_await parent_->update(map);
    }

    const RegisterFieldDescriptor& descriptor() const noexcept {
        return *descriptor_;
    }

    [[nodiscard]] std::string_view name() const noexcept {
        return descriptor_->name;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return descriptor_->path;
    }

    [[nodiscard]] std::span<const RegisterBackdoorSliceDescriptor>
    hdl_slices() const noexcept {
        return descriptor_->backdoor_slices;
    }

    [[nodiscard]] std::optional<std::string_view> hdl_path() const noexcept {
        if (descriptor_->backdoor_slices.size() != 1) return std::nullopt;
        return descriptor_->backdoor_slices.front().path;
    }

    [[nodiscard]] uint64_t address() const noexcept {
        return parent_->address();
    }

    [[nodiscard]] uint16_t lsb() const noexcept { return descriptor_->lsb; }

    [[nodiscard]] uint16_t width() const noexcept {
        return descriptor_->width;
    }

   private:
    RegisterHandle<Master>* parent_;
    const RegisterFieldDescriptor* descriptor_;
};

template <MemoryMappedMaster Master>
    requires std::unsigned_integral<typename Master::data_type>
class RegisterMemoryHandle {
   public:
    using address_type = typename Master::address_type;
    using data_type = typename Master::data_type;
    using byte_enable_type = typename Master::byte_enable_type;
    using transport_read_response_type = typename Master::read_response_type;
    using transport_write_response_type = typename Master::write_response_type;
    using read_response_type =
        RegisterMemoryReadResponse<data_type, transport_read_response_type>;
    using write_response_type =
        RegisterMemoryWriteResponse<transport_write_response_type>;
    using read_operation_type = register_detail::ReadyOrTask<read_response_type>;
    using write_operation_type =
        register_detail::ReadyOrTask<write_response_type>;

    class Slice {
       public:
        [[nodiscard]] uint64_t first_index() const noexcept {
            return first_index_;
        }

        [[nodiscard]] std::size_t size() const noexcept { return size_; }

        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

        [[nodiscard]] std::string_view name() const noexcept {
            return parent_->name();
        }

        [[nodiscard]] std::string_view path() const noexcept {
            return parent_->path();
        }

        [[nodiscard]] std::optional<std::string_view> hdl_path() const noexcept {
            return parent_->hdl_path();
        }

        [[nodiscard]] uint64_t address() const {
            return parent_->address_after_range_check(first_index_);
        }

        read_operation_type read(
            uint64_t offset, AccessPath path = AccessPath::Frontdoor) {
            return parent_->read(index(offset), path);
        }

        read_operation_type read(uint64_t offset,
                                 RegisterAddressMap<Master>& map) {
            return parent_->read(index(offset), map);
        }

        write_operation_type write(
            uint64_t offset, data_type value,
            AccessPath path = AccessPath::Frontdoor) {
            return parent_->write(index(offset), value, path);
        }

        write_operation_type write(uint64_t offset, data_type value,
                                   RegisterAddressMap<Master>& map) {
            return parent_->write(index(offset), value, map);
        }

        read_operation_type read_into(
            std::span<data_type> values,
            AccessPath path = AccessPath::Frontdoor) {
            require_size(values.size());
            return parent_->read_into(first_index_, values, path);
        }

        read_operation_type read_into(std::span<data_type> values,
                                      RegisterAddressMap<Master>& map) {
            require_size(values.size());
            return parent_->read_into(first_index_, values, map);
        }

        read_operation_type read(
            std::span<data_type> values,
            AccessPath path = AccessPath::Frontdoor) {
            return read_into(values, path);
        }

        write_operation_type write(
            std::span<const data_type> values,
            AccessPath path = AccessPath::Frontdoor) {
            require_size(values.size());
            return parent_->write(first_index_, values, path);
        }

        write_operation_type write(std::span<const data_type> values,
                                   RegisterAddressMap<Master>& map) {
            require_size(values.size());
            return parent_->write(first_index_, values, map);
        }

        [[nodiscard]] data_type peek(uint64_t offset) const {
            return parent_->peek(index(offset));
        }

        void poke(uint64_t offset, data_type value) const {
            parent_->poke(index(offset), value);
        }

        void peek_into(std::span<data_type> values) const {
            require_size(values.size());
            parent_->peek_into(first_index_, values);
        }

        void poke(std::span<const data_type> values) const {
            require_size(values.size());
            parent_->poke(first_index_, values);
        }

       private:
        friend class RegisterMemoryHandle;

        Slice(RegisterMemoryHandle& parent, uint64_t first_index,
              std::size_t size) noexcept
            : parent_(&parent), first_index_(first_index), size_(size) {}

        [[nodiscard]] uint64_t index(uint64_t offset) const {
            if (offset >= size_) fail_offset();
            return first_index_ + offset;
        }

        void require_size(std::size_t size) const {
            if (size != size_) fail_size();
        }

        [[noreturn]] void fail_offset() const {
            throw std::out_of_range(
                "cpptb-vc: register memory slice offset out of bounds: " +
                std::string{parent_->path()});
        }

        [[noreturn]] void fail_size() const {
            throw std::invalid_argument(
                "cpptb-vc: register memory slice span size does not match "
                "the slice: " +
                std::string{parent_->path()});
        }

        RegisterMemoryHandle* parent_;
        uint64_t first_index_ = 0;
        std::size_t size_ = 0;
    };

    RegisterMemoryHandle(Master& master,
                         const RegisterMemoryDescriptor& descriptor,
                         uint64_t base_address = 0,
                         RegisterMemoryBackdoor<data_type>* backdoor = nullptr)
        : master_(&master), descriptor_(&descriptor), backdoor_(backdoor) {
        if (base_address >
            std::numeric_limits<uint64_t>::max() - descriptor.address) {
            throw std::invalid_argument(
                "cpptb-vc: register memory base plus offset overflows: " +
                std::string{descriptor.path});
        }
        base_address_ = base_address + descriptor.address;
        validate_element();
        if (descriptor.entries >
            (std::numeric_limits<uint64_t>::max() - base_address_) /
                element_bytes()) {
            throw std::invalid_argument(
                "cpptb-vc: register memory address range overflows: " +
                std::string{descriptor.path});
        }
    }

    read_operation_type read(
        uint64_t index, AccessPath path = AccessPath::Frontdoor) {
        require_range(index, 1);
        if (!register_readable(descriptor_->access)) {
            throw std::logic_error(
                "cpptb-vc: register memory is not readable: " +
                std::string{descriptor_->path});
        }
        read_response_type result{.path = path};
        if (path == AccessPath::Backdoor) {
            require_backdoor();
            result.data = backdoor_->peek(*descriptor_, index,
                                          address_after_range_check(index));
            result.transfers_completed = 1;
            return read_operation_type{std::move(result)};
        }
        return read_operation_type{frontdoor_read(index)};
    }

    read_operation_type read(uint64_t index,
                             RegisterAddressMap<Master>& map) {
        require_read(index, 1);
        return read_operation_type{frontdoor_read(index, &map)};
    }

    write_operation_type write(
        uint64_t index, data_type value,
        AccessPath path = AccessPath::Frontdoor) {
        require_range(index, 1);
        if (!register_writable(descriptor_->access)) {
            throw std::logic_error(
                "cpptb-vc: register memory is not writable: " +
                std::string{descriptor_->path});
        }
        write_response_type result{.path = path};
        if (path == AccessPath::Backdoor) {
            require_backdoor();
            backdoor_->poke(*descriptor_, index,
                            address_after_range_check(index), value);
            result.transfers_completed = 1;
            return write_operation_type{std::move(result)};
        }
        return write_operation_type{frontdoor_write(index, value)};
    }

    write_operation_type write(uint64_t index, data_type value,
                               RegisterAddressMap<Master>& map) {
        require_write(index, 1);
        return write_operation_type{frontdoor_write(index, value, &map)};
    }

    write_operation_type write(
        uint64_t first_index, std::span<const data_type> values,
        AccessPath path = AccessPath::Frontdoor) {
        require_range(first_index, values.size());
        if (!register_writable(descriptor_->access)) {
            throw std::logic_error(
                "cpptb-vc: register memory is not writable: " +
                std::string{descriptor_->path});
        }

        write_response_type result{.path = path};
        if (values.empty()) return write_operation_type{std::move(result)};
        if (path == AccessPath::Backdoor) {
            require_backdoor();
            backdoor_->poke(*descriptor_, first_index,
                            address_after_range_check(first_index), values);
            result.transfers_completed = values.size();
            return write_operation_type{std::move(result)};
        }
        return write_operation_type{frontdoor_write(first_index, values)};
    }

    write_operation_type write(uint64_t first_index,
                               std::span<const data_type> values,
                               RegisterAddressMap<Master>& map) {
        require_write(first_index, values.size());
        write_response_type result{.path = AccessPath::Frontdoor};
        if (values.empty()) return write_operation_type{std::move(result)};
        return write_operation_type{frontdoor_write(first_index, values,
                                                    &map)};
    }

    read_operation_type read_into(
        uint64_t first_index, std::span<data_type> values,
        AccessPath path = AccessPath::Frontdoor) {
        require_range(first_index, values.size());
        if (!register_readable(descriptor_->access)) {
            throw std::logic_error(
                "cpptb-vc: register memory is not readable: " +
                std::string{descriptor_->path});
        }

        read_response_type result{.path = path};
        if (values.empty()) return read_operation_type{std::move(result)};
        if (path == AccessPath::Backdoor) {
            require_backdoor();
            backdoor_->peek_into(*descriptor_, first_index,
                                 address_after_range_check(first_index), values);
            result.data = values.back();
            result.transfers_completed = values.size();
            return read_operation_type{std::move(result)};
        }
        return read_operation_type{frontdoor_read(first_index, values)};
    }

    read_operation_type read_into(uint64_t first_index,
                                  std::span<data_type> values,
                                  RegisterAddressMap<Master>& map) {
        require_read(first_index, values.size());
        read_response_type result{.path = AccessPath::Frontdoor};
        if (values.empty()) return read_operation_type{std::move(result)};
        return read_operation_type{frontdoor_read(first_index, values, &map)};
    }

    read_operation_type read(
        uint64_t first_index, std::span<data_type> values,
        AccessPath path = AccessPath::Frontdoor) {
        return read_into(first_index, values, path);
    }

    read_operation_type read_offset(
        uint64_t byte_offset, AccessPath path = AccessPath::Frontdoor) {
        return read(index_from_offset_for_range(byte_offset, 1), path);
    }

    read_operation_type read_offset(
        uint64_t byte_offset, std::span<data_type> values,
        AccessPath path = AccessPath::Frontdoor) {
        return read_into(
            index_from_offset_for_range(byte_offset, values.size()), values,
            path);
    }

    write_operation_type write_offset(
        uint64_t byte_offset, data_type value,
        AccessPath path = AccessPath::Frontdoor) {
        return write(index_from_offset_for_range(byte_offset, 1), value, path);
    }

    write_operation_type write_offset(
        uint64_t byte_offset, std::span<const data_type> values,
        AccessPath path = AccessPath::Frontdoor) {
        return write(
            index_from_offset_for_range(byte_offset, values.size()), values,
            path);
    }

    read_operation_type read_absolute(
        uint64_t absolute_address,
        AccessPath path = AccessPath::Frontdoor) {
        return read(index_from_absolute_for_range(absolute_address, 1), path);
    }

    read_operation_type read_absolute(
        uint64_t absolute_address, std::span<data_type> values,
        AccessPath path = AccessPath::Frontdoor) {
        return read_into(
            index_from_absolute_for_range(absolute_address, values.size()),
            values, path);
    }

    write_operation_type write_absolute(
        uint64_t absolute_address, data_type value,
        AccessPath path = AccessPath::Frontdoor) {
        return write(index_from_absolute_for_range(absolute_address, 1), value,
                     path);
    }

    write_operation_type write_absolute(
        uint64_t absolute_address, std::span<const data_type> values,
        AccessPath path = AccessPath::Frontdoor) {
        return write(
            index_from_absolute_for_range(absolute_address, values.size()),
            values, path);
    }

    [[nodiscard]] data_type peek(uint64_t index) const {
        require_range(index, 1);
        require_backdoor();
        return backdoor_->peek(*descriptor_, index,
                               address_after_range_check(index));
    }

    void poke(uint64_t index, data_type value) const {
        require_range(index, 1);
        require_backdoor();
        backdoor_->poke(*descriptor_, index, address_after_range_check(index),
                        value);
    }

    void peek_into(uint64_t first_index, std::span<data_type> values) const {
        require_range(first_index, values.size());
        if (values.empty()) return;
        require_backdoor();
        backdoor_->peek_into(*descriptor_, first_index,
                             address_after_range_check(first_index), values);
    }

    void poke(uint64_t first_index,
              std::span<const data_type> values) const {
        require_range(first_index, values.size());
        if (values.empty()) return;
        require_backdoor();
        backdoor_->poke(*descriptor_, first_index,
                        address_after_range_check(first_index), values);
    }

    [[nodiscard]] data_type peek_offset(uint64_t byte_offset) const {
        return peek(index_from_offset_for_range(byte_offset, 1));
    }

    void peek_offset_into(uint64_t byte_offset,
                          std::span<data_type> values) const {
        peek_into(index_from_offset_for_range(byte_offset, values.size()),
                  values);
    }

    void poke_offset(uint64_t byte_offset, data_type value) const {
        poke(index_from_offset_for_range(byte_offset, 1), value);
    }

    void poke_offset(uint64_t byte_offset,
                     std::span<const data_type> values) const {
        poke(index_from_offset_for_range(byte_offset, values.size()), values);
    }

    [[nodiscard]] data_type peek_absolute(uint64_t absolute_address) const {
        return peek(index_from_absolute_for_range(absolute_address, 1));
    }

    void peek_absolute_into(uint64_t absolute_address,
                            std::span<data_type> values) const {
        peek_into(
            index_from_absolute_for_range(absolute_address, values.size()),
            values);
    }

    void poke_absolute(uint64_t absolute_address, data_type value) const {
        poke(index_from_absolute_for_range(absolute_address, 1), value);
    }

    void poke_absolute(uint64_t absolute_address,
                       std::span<const data_type> values) const {
        poke(index_from_absolute_for_range(absolute_address, values.size()),
             values);
    }

    uint64_t address(uint64_t index) const {
        require_range(index, 1);
        return address_after_range_check(index);
    }

    uint64_t address(uint64_t index,
                     const RegisterAddressMap<Master>& map) const {
        require_range(index, 1);
        return map.effective_address(*descriptor_, index);
    }

    [[nodiscard]] Slice slice(uint64_t first_index, std::size_t size) {
        require_range(first_index, size);
        return Slice{*this, first_index, size};
    }

    const RegisterMemoryDescriptor& descriptor() const noexcept {
        return *descriptor_;
    }

    [[nodiscard]] std::string_view name() const noexcept {
        return descriptor_->name;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return descriptor_->path;
    }

    [[nodiscard]] std::optional<std::string_view> hdl_path() const noexcept {
        if (descriptor_->hdl_path.empty()) return std::nullopt;
        return descriptor_->hdl_path;
    }

    [[nodiscard]] uint64_t size() const noexcept {
        return descriptor_->entries;
    }

    [[nodiscard]] uint16_t width() const noexcept {
        return descriptor_->width;
    }

    [[nodiscard]] uint64_t base_address() const noexcept {
        return base_address_;
    }

    [[nodiscard]] uint64_t end_address() const {
        return address_after_range_check(descriptor_->entries);
    }

    [[nodiscard]] uint64_t element_bytes() const noexcept {
        return descriptor_->width / 8u;
    }

    [[nodiscard]] uint64_t index_from_offset(uint64_t byte_offset) const {
        return index_from_offset_for_range(byte_offset, 1);
    }

    [[nodiscard]] uint64_t index_from_absolute(
        uint64_t absolute_address) const {
        return index_from_absolute_for_range(absolute_address, 1);
    }

    [[nodiscard]] bool contains_absolute(
        uint64_t absolute_address) const noexcept {
        if (absolute_address < base_address_) return false;
        const uint64_t offset = absolute_address - base_address_;
        const uint64_t bytes = element_bytes();
        return offset % bytes == 0 && offset / bytes < descriptor_->entries;
    }

   private:
    uint64_t index_from_offset_for_range(uint64_t byte_offset,
                                         std::size_t count) const {
        const uint64_t bytes = element_bytes();
        if (byte_offset % bytes != 0) fail_unaligned_offset(byte_offset);
        const uint64_t index = byte_offset / bytes;
        require_range(index, count);
        return index;
    }

    uint64_t index_from_absolute_for_range(uint64_t absolute_address,
                                           std::size_t count) const {
        if (absolute_address < base_address_) {
            fail_absolute_range(absolute_address);
        }
        const uint64_t byte_offset = absolute_address - base_address_;
        const uint64_t bytes = element_bytes();
        if (byte_offset % bytes != 0) {
            fail_unaligned_absolute(absolute_address);
        }
        const uint64_t index = byte_offset / bytes;
        if (index > descriptor_->entries ||
            count > descriptor_->entries - index ||
            (count != 0 && index == descriptor_->entries)) {
            fail_absolute_range(absolute_address);
        }
        return index;
    }

    uint64_t address_after_range_check(uint64_t index) const {
        if (index >
            (std::numeric_limits<uint64_t>::max() - base_address_) /
                element_bytes()) {
            fail_address_overflow();
        }
        return base_address_ + index * element_bytes();
    }

    uint64_t frontdoor_address(
        uint64_t index, const RegisterAddressMap<Master>* map) const {
        return map ? map->effective_address(*descriptor_, index)
                   : address_after_range_check(index);
    }

    coro::Task<read_response_type> frontdoor_read(
        uint64_t index, RegisterAddressMap<Master>* map = nullptr) {
        require_frontdoor_supported(map);
        read_response_type result{.path = AccessPath::Frontdoor};
        const auto request = typename Master::read_request_type{
            static_cast<address_type>(frontdoor_address(index, map))};
        if (map) {
            result.transport =
                co_await map->read(*descriptor_, index, request);
        } else {
            result.transport = co_await master_->read(request);
        }
        if (result.transport.okay()) {
            result.data = result.transport.data;
            result.transfers_completed = 1;
        } else {
            result.failed_index = index;
        }
        co_return result;
    }

    coro::Task<write_response_type> frontdoor_write(
        uint64_t index, data_type value,
        RegisterAddressMap<Master>* map = nullptr) {
        require_frontdoor_supported(map);
        write_response_type result{.path = AccessPath::Frontdoor};
        const auto request = typename Master::write_request_type{
            static_cast<address_type>(frontdoor_address(index, map)), value,
            all_bytes()};
        if (map) {
            result.transport =
                co_await map->write(*descriptor_, index, request);
        } else {
            result.transport = co_await master_->write(request);
        }
        if (result.transport.okay()) {
            result.transfers_completed = 1;
        } else {
            result.failed_index = index;
        }
        co_return result;
    }

    coro::Task<write_response_type> frontdoor_write(
        uint64_t first_index, std::span<const data_type> values,
        RegisterAddressMap<Master>* map = nullptr) {
        require_frontdoor_supported(map);
        write_response_type result{.path = AccessPath::Frontdoor};
        for (std::size_t offset = 0; offset < values.size(); ++offset) {
            const uint64_t index = first_index + offset;
            const auto request = typename Master::write_request_type{
                static_cast<address_type>(frontdoor_address(index, map)),
                values[offset], all_bytes()};
            if (map) {
                result.transport =
                    co_await map->write(*descriptor_, index, request);
            } else {
                result.transport = co_await master_->write(request);
            }
            if (!result.transport.okay()) {
                result.failed_index = index;
                co_return result;
            }
            ++result.transfers_completed;
        }
        co_return result;
    }

    coro::Task<read_response_type> frontdoor_read(
        uint64_t first_index, std::span<data_type> values,
        RegisterAddressMap<Master>* map = nullptr) {
        require_frontdoor_supported(map);
        read_response_type result{.path = AccessPath::Frontdoor};
        for (std::size_t offset = 0; offset < values.size(); ++offset) {
            const uint64_t index = first_index + offset;
            const auto request = typename Master::read_request_type{
                static_cast<address_type>(frontdoor_address(index, map))};
            if (map) {
                result.transport =
                    co_await map->read(*descriptor_, index, request);
            } else {
                result.transport = co_await master_->read(request);
            }
            if (!result.transport.okay()) {
                result.failed_index = index;
                co_return result;
            }
            values[offset] = result.transport.data;
            result.data = result.transport.data;
            ++result.transfers_completed;
        }
        co_return result;
    }

    void validate_element() const {
        if (descriptor_->width == 0 || descriptor_->width % 8 != 0 ||
            descriptor_->width > std::numeric_limits<data_type>::digits) {
            throw std::logic_error(
                "cpptb-vc: register memory element must be byte-aligned and "
                "fit the model data width: " +
                std::string{descriptor_->path});
        }
    }

    void require_frontdoor_supported(
        const RegisterAddressMap<Master>* map = nullptr) const {
        if (descriptor_->access_width != 0 &&
            descriptor_->access_width != descriptor_->width) {
            throw std::logic_error(
                "cpptb-vc: split register-memory accesses are not supported: " +
                std::string{descriptor_->path});
        }
        if constexpr (std::integral<address_type>) {
            if (descriptor_->entries != 0 &&
                frontdoor_address(descriptor_->entries - 1, map) >
                    static_cast<uint64_t>(
                        std::numeric_limits<address_type>::max())) {
                throw std::logic_error(
                    "cpptb-vc: register memory exceeds the frontdoor "
                    "address width: " +
                    std::string{descriptor_->path});
            }
        }
    }

    void require_read(uint64_t first_index, std::size_t count) const {
        require_range(first_index, count);
        if (!register_readable(descriptor_->access)) {
            throw std::logic_error(
                "cpptb-vc: register memory is not readable: " +
                std::string{descriptor_->path});
        }
    }

    void require_write(uint64_t first_index, std::size_t count) const {
        require_range(first_index, count);
        if (!register_writable(descriptor_->access)) {
            throw std::logic_error(
                "cpptb-vc: register memory is not writable: " +
                std::string{descriptor_->path});
        }
    }

    void require_range(uint64_t first_index, std::size_t count) const {
        if (first_index > descriptor_->entries ||
            count > descriptor_->entries - first_index ||
            (count != 0 && first_index == descriptor_->entries)) {
            fail_range();
        }
    }

    void require_backdoor() const {
        if (backdoor_) return;
        fail_missing_backdoor();
    }

    [[noreturn]] void fail_address_overflow() const {
        throw std::overflow_error(
            "cpptb-vc: register memory indexed address overflows: " +
            std::string{descriptor_->path});
    }

    [[noreturn]] void fail_range() const {
        throw std::out_of_range(
            "cpptb-vc: register memory range out of bounds: " +
            std::string{descriptor_->path});
    }

    [[noreturn]] void fail_missing_backdoor() const {
        throw std::logic_error(
            "cpptb-vc: register memory has no backdoor: " +
            std::string{descriptor_->path});
    }

    [[noreturn]] void fail_unaligned_offset(uint64_t byte_offset) const {
        throw std::invalid_argument(
            "cpptb-vc: register memory byte offset is not element-aligned: " +
            std::string{descriptor_->path} + " offset=" +
            std::to_string(byte_offset));
    }

    [[noreturn]] void fail_unaligned_absolute(
        uint64_t absolute_address) const {
        throw std::invalid_argument(
            "cpptb-vc: register memory absolute address is not "
            "element-aligned: " +
            std::string{descriptor_->path} + " address=" +
            std::to_string(absolute_address));
    }

    [[noreturn]] void fail_absolute_range(uint64_t absolute_address) const {
        throw std::out_of_range(
            "cpptb-vc: register memory absolute address is out of bounds: " +
            std::string{descriptor_->path} + " address=" +
            std::to_string(absolute_address));
    }

    byte_enable_type all_bytes() const noexcept {
        const uint64_t bytes = element_bytes();
        const uint64_t mask =
            bytes >= 64 ? std::numeric_limits<uint64_t>::max()
                        : (uint64_t{1} << bytes) - 1u;
        return static_cast<byte_enable_type>(mask);
    }

    Master* master_;
    const RegisterMemoryDescriptor* descriptor_;
    RegisterMemoryBackdoor<data_type>* backdoor_ = nullptr;
    uint64_t base_address_ = 0;
};

template <std::size_t Width, MemoryMappedMaster Master>
    requires std::unsigned_integral<typename Master::data_type>
class WideRegisterMemoryHandle {
    static_assert(Width > 64,
                  "WideRegisterMemoryHandle is for elements over 64 bits");

   public:
    using address_type = typename Master::address_type;
    using bus_data_type = typename Master::data_type;
    using byte_enable_type = typename Master::byte_enable_type;
    using data_type = Bits<Width>;
    using transport_read_response_type = typename Master::read_response_type;
    using transport_write_response_type = typename Master::write_response_type;
    using read_response_type =
        RegisterMemoryReadResponse<data_type, transport_read_response_type>;
    using write_response_type =
        RegisterMemoryWriteResponse<transport_write_response_type>;
    using read_operation_type = register_detail::ReadyOrTask<read_response_type>;
    using write_operation_type =
        register_detail::ReadyOrTask<write_response_type>;

    class Slice {
       public:
        [[nodiscard]] uint64_t first_index() const noexcept {
            return first_index_;
        }
        [[nodiscard]] std::size_t size() const noexcept { return size_; }
        [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
        [[nodiscard]] std::string_view name() const noexcept {
            return parent_->name();
        }
        [[nodiscard]] std::string_view path() const noexcept {
            return parent_->path();
        }
        [[nodiscard]] std::optional<std::string_view> hdl_path() const noexcept {
            return parent_->hdl_path();
        }
        [[nodiscard]] uint64_t address() const {
            return parent_->address_after_range_check(first_index_);
        }

        read_operation_type read(
            uint64_t offset, AccessPath path = AccessPath::Frontdoor) {
            return parent_->read(index(offset), path);
        }
        read_operation_type read(uint64_t offset,
                                 RegisterAddressMap<Master>& map) {
            return parent_->read(index(offset), map);
        }
        write_operation_type write(
            uint64_t offset, const data_type& value,
            AccessPath path = AccessPath::Frontdoor) {
            return parent_->write(index(offset), value, path);
        }
        write_operation_type write(uint64_t offset, const data_type& value,
                                   RegisterAddressMap<Master>& map) {
            return parent_->write(index(offset), value, map);
        }
        read_operation_type read_into(
            std::span<data_type> values,
            AccessPath path = AccessPath::Frontdoor) {
            require_size(values.size());
            return parent_->read_into(first_index_, values, path);
        }
        read_operation_type read_into(std::span<data_type> values,
                                      RegisterAddressMap<Master>& map) {
            require_size(values.size());
            return parent_->read_into(first_index_, values, map);
        }
        read_operation_type read(
            std::span<data_type> values,
            AccessPath path = AccessPath::Frontdoor) {
            return read_into(values, path);
        }
        write_operation_type write(
            std::span<const data_type> values,
            AccessPath path = AccessPath::Frontdoor) {
            require_size(values.size());
            return parent_->write(first_index_, values, path);
        }
        write_operation_type write(std::span<const data_type> values,
                                   RegisterAddressMap<Master>& map) {
            require_size(values.size());
            return parent_->write(first_index_, values, map);
        }
        [[nodiscard]] data_type peek(uint64_t offset) const {
            return parent_->peek(index(offset));
        }
        void poke(uint64_t offset, const data_type& value) const {
            parent_->poke(index(offset), value);
        }
        void peek_into(std::span<data_type> values) const {
            require_size(values.size());
            parent_->peek_into(first_index_, values);
        }
        void poke(std::span<const data_type> values) const {
            require_size(values.size());
            parent_->poke(first_index_, values);
        }

       private:
        friend class WideRegisterMemoryHandle;

        Slice(WideRegisterMemoryHandle& parent, uint64_t first_index,
              std::size_t size) noexcept
            : parent_(&parent), first_index_(first_index), size_(size) {}

        [[nodiscard]] uint64_t index(uint64_t offset) const {
            if (offset >= size_) {
                throw std::out_of_range(
                    "cpptb-vc: wide register memory slice offset out of bounds: " +
                    std::string{parent_->path()});
            }
            return first_index_ + offset;
        }
        void require_size(std::size_t size) const {
            if (size != size_) {
                throw std::invalid_argument(
                    "cpptb-vc: wide register memory slice span size does not match the slice: " +
                    std::string{parent_->path()});
            }
        }

        WideRegisterMemoryHandle* parent_;
        uint64_t first_index_ = 0;
        std::size_t size_ = 0;
    };

    WideRegisterMemoryHandle(
        Master& master, const RegisterMemoryDescriptor& descriptor,
        uint64_t base_address = 0,
        WideRegisterMemoryBackdoor* backdoor = nullptr)
        : master_(&master), descriptor_(&descriptor), backdoor_(backdoor) {
        if (descriptor.width != Width) {
            throw std::invalid_argument(
                "cpptb-vc: generated wide memory width does not match its descriptor: " +
                std::string{descriptor.path});
        }
        if (base_address >
            std::numeric_limits<uint64_t>::max() - descriptor.address) {
            throw std::invalid_argument(
                "cpptb-vc: wide register memory base plus offset overflows: " +
                std::string{descriptor.path});
        }
        base_address_ = base_address + descriptor.address;
        validate_element();
        if (descriptor.entries >
            (std::numeric_limits<uint64_t>::max() - base_address_) /
                element_bytes()) {
            throw std::invalid_argument(
                "cpptb-vc: wide register memory address range overflows: " +
                std::string{descriptor.path});
        }
    }

    read_operation_type read(
        uint64_t index, AccessPath path = AccessPath::Frontdoor) {
        require_range(index, 1);
        require_readable();
        read_response_type result{.path = path};
        if (path == AccessPath::Backdoor) {
            require_backdoor();
            typename data_type::word_array words{};
            backdoor_->peek_words(*descriptor_, index, address(index), words);
            result.data = data_type::from_words(words);
            result.transfers_completed = 1;
            return read_operation_type{std::move(result)};
        }
        return read_operation_type{frontdoor_read(index)};
    }

    read_operation_type read(uint64_t index,
                             RegisterAddressMap<Master>& map) {
        require_range(index, 1);
        require_readable();
        return read_operation_type{frontdoor_read(index, &map)};
    }

    write_operation_type write(
        uint64_t index, const data_type& value,
        AccessPath path = AccessPath::Frontdoor) {
        require_range(index, 1);
        require_writable();
        write_response_type result{.path = path};
        if (path == AccessPath::Backdoor) {
            require_backdoor();
            backdoor_->poke_words(*descriptor_, index, address(index),
                                  value.words());
            result.transfers_completed = 1;
            return write_operation_type{std::move(result)};
        }
        return write_operation_type{frontdoor_write(index, value)};
    }

    write_operation_type write(uint64_t index, const data_type& value,
                               RegisterAddressMap<Master>& map) {
        require_range(index, 1);
        require_writable();
        return write_operation_type{frontdoor_write(index, value, &map)};
    }

    read_operation_type read_into(
        uint64_t first_index, std::span<data_type> values,
        AccessPath path = AccessPath::Frontdoor) {
        require_range(first_index, values.size());
        require_readable();
        read_response_type result{.path = path};
        if (values.empty()) return read_operation_type{std::move(result)};
        if (path == AccessPath::Backdoor) {
            require_backdoor();
            std::vector<uint32_t> words(values.size() * data_type::word_count);
            backdoor_->peek_elements(
                *descriptor_, first_index, address(first_index), words,
                data_type::word_count);
            for (std::size_t offset = 0; offset < values.size(); ++offset) {
                typename data_type::word_array element{};
                std::copy_n(words.begin() +
                                static_cast<std::ptrdiff_t>(
                                    offset * data_type::word_count),
                            data_type::word_count, element.begin());
                values[offset] = data_type::from_words(element);
            }
            result.data = values.back();
            result.transfers_completed = values.size();
            return read_operation_type{std::move(result)};
        }
        return read_operation_type{frontdoor_read(first_index, values)};
    }

    read_operation_type read_into(uint64_t first_index,
                                  std::span<data_type> values,
                                  RegisterAddressMap<Master>& map) {
        require_range(first_index, values.size());
        require_readable();
        read_response_type result{.path = AccessPath::Frontdoor};
        if (values.empty()) return read_operation_type{std::move(result)};
        return read_operation_type{frontdoor_read(first_index, values, &map)};
    }

    read_operation_type read(
        uint64_t first_index, std::span<data_type> values,
        AccessPath path = AccessPath::Frontdoor) {
        return read_into(first_index, values, path);
    }

    write_operation_type write(
        uint64_t first_index, std::span<const data_type> values,
        AccessPath path = AccessPath::Frontdoor) {
        require_range(first_index, values.size());
        require_writable();
        write_response_type result{.path = path};
        if (values.empty()) return write_operation_type{std::move(result)};
        if (path == AccessPath::Backdoor) {
            require_backdoor();
            std::vector<uint32_t> words;
            words.reserve(values.size() * data_type::word_count);
            for (const auto& value : values) {
                words.insert(words.end(), value.words().begin(),
                             value.words().end());
            }
            backdoor_->poke_elements(
                *descriptor_, first_index, address(first_index), words,
                data_type::word_count);
            result.transfers_completed = values.size();
            return write_operation_type{std::move(result)};
        }
        return write_operation_type{frontdoor_write(first_index, values)};
    }

    write_operation_type write(uint64_t first_index,
                               std::span<const data_type> values,
                               RegisterAddressMap<Master>& map) {
        require_range(first_index, values.size());
        require_writable();
        write_response_type result{.path = AccessPath::Frontdoor};
        if (values.empty()) return write_operation_type{std::move(result)};
        return write_operation_type{frontdoor_write(first_index, values,
                                                    &map)};
    }

    read_operation_type read_offset(
        uint64_t byte_offset, AccessPath path = AccessPath::Frontdoor) {
        return read(index_from_offset_for_range(byte_offset, 1), path);
    }
    read_operation_type read_offset(
        uint64_t byte_offset, std::span<data_type> values,
        AccessPath path = AccessPath::Frontdoor) {
        return read_into(index_from_offset_for_range(byte_offset, values.size()),
                         values, path);
    }
    write_operation_type write_offset(
        uint64_t byte_offset, const data_type& value,
        AccessPath path = AccessPath::Frontdoor) {
        return write(index_from_offset_for_range(byte_offset, 1), value, path);
    }
    write_operation_type write_offset(
        uint64_t byte_offset, std::span<const data_type> values,
        AccessPath path = AccessPath::Frontdoor) {
        return write(index_from_offset_for_range(byte_offset, values.size()),
                     values, path);
    }
    read_operation_type read_absolute(
        uint64_t absolute_address,
        AccessPath path = AccessPath::Frontdoor) {
        return read(index_from_absolute_for_range(absolute_address, 1), path);
    }
    read_operation_type read_absolute(
        uint64_t absolute_address, std::span<data_type> values,
        AccessPath path = AccessPath::Frontdoor) {
        return read_into(
            index_from_absolute_for_range(absolute_address, values.size()),
            values, path);
    }
    write_operation_type write_absolute(
        uint64_t absolute_address, const data_type& value,
        AccessPath path = AccessPath::Frontdoor) {
        return write(index_from_absolute_for_range(absolute_address, 1), value,
                     path);
    }
    write_operation_type write_absolute(
        uint64_t absolute_address, std::span<const data_type> values,
        AccessPath path = AccessPath::Frontdoor) {
        return write(
            index_from_absolute_for_range(absolute_address, values.size()),
            values, path);
    }

    [[nodiscard]] data_type peek(uint64_t index) const {
        require_range(index, 1);
        require_backdoor();
        typename data_type::word_array words{};
        backdoor_->peek_words(*descriptor_, index, address(index), words);
        return data_type::from_words(words);
    }
    void poke(uint64_t index, const data_type& value) const {
        require_range(index, 1);
        require_backdoor();
        backdoor_->poke_words(*descriptor_, index, address(index),
                              value.words());
    }
    void peek_into(uint64_t first_index, std::span<data_type> values) const {
        require_range(first_index, values.size());
        if (values.empty()) return;
        require_backdoor();
        std::vector<uint32_t> words(values.size() * data_type::word_count);
        backdoor_->peek_elements(*descriptor_, first_index,
                                 address(first_index), words,
                                 data_type::word_count);
        for (std::size_t offset = 0; offset < values.size(); ++offset) {
            typename data_type::word_array element{};
            std::copy_n(words.begin() + static_cast<std::ptrdiff_t>(
                                            offset * data_type::word_count),
                        data_type::word_count, element.begin());
            values[offset] = data_type::from_words(element);
        }
    }
    void poke(uint64_t first_index,
              std::span<const data_type> values) const {
        require_range(first_index, values.size());
        if (values.empty()) return;
        require_backdoor();
        std::vector<uint32_t> words;
        words.reserve(values.size() * data_type::word_count);
        for (const auto& value : values) {
            words.insert(words.end(), value.words().begin(),
                         value.words().end());
        }
        backdoor_->poke_elements(*descriptor_, first_index,
                                 address(first_index), words,
                                 data_type::word_count);
    }

    [[nodiscard]] data_type peek_offset(uint64_t byte_offset) const {
        return peek(index_from_offset_for_range(byte_offset, 1));
    }
    void peek_offset_into(uint64_t byte_offset,
                          std::span<data_type> values) const {
        peek_into(index_from_offset_for_range(byte_offset, values.size()),
                  values);
    }
    void poke_offset(uint64_t byte_offset, const data_type& value) const {
        poke(index_from_offset_for_range(byte_offset, 1), value);
    }
    void poke_offset(uint64_t byte_offset,
                     std::span<const data_type> values) const {
        poke(index_from_offset_for_range(byte_offset, values.size()), values);
    }
    [[nodiscard]] data_type peek_absolute(uint64_t absolute_address) const {
        return peek(index_from_absolute_for_range(absolute_address, 1));
    }
    void peek_absolute_into(uint64_t absolute_address,
                            std::span<data_type> values) const {
        peek_into(index_from_absolute_for_range(absolute_address, values.size()),
                  values);
    }
    void poke_absolute(uint64_t absolute_address,
                       const data_type& value) const {
        poke(index_from_absolute_for_range(absolute_address, 1), value);
    }
    void poke_absolute(uint64_t absolute_address,
                       std::span<const data_type> values) const {
        poke(index_from_absolute_for_range(absolute_address, values.size()),
             values);
    }

    [[nodiscard]] uint64_t address(uint64_t index) const {
        require_range(index, 1);
        return address_after_range_check(index);
    }
    [[nodiscard]] uint64_t address(
        uint64_t index, const RegisterAddressMap<Master>& map) const {
        require_range(index, 1);
        return map.effective_address(*descriptor_, index);
    }
    [[nodiscard]] Slice slice(uint64_t first_index, std::size_t size) {
        require_range(first_index, size);
        return Slice{*this, first_index, size};
    }
    [[nodiscard]] const RegisterMemoryDescriptor& descriptor() const noexcept {
        return *descriptor_;
    }
    [[nodiscard]] std::string_view name() const noexcept {
        return descriptor_->name;
    }
    [[nodiscard]] std::string_view path() const noexcept {
        return descriptor_->path;
    }
    [[nodiscard]] std::optional<std::string_view> hdl_path() const noexcept {
        if (descriptor_->hdl_path.empty()) return std::nullopt;
        return descriptor_->hdl_path;
    }
    [[nodiscard]] bool has_backdoor() const noexcept {
        return backdoor_ != nullptr;
    }
    [[nodiscard]] uint64_t size() const noexcept {
        return descriptor_->entries;
    }
    [[nodiscard]] uint16_t width() const noexcept { return Width; }
    [[nodiscard]] uint64_t base_address() const noexcept {
        return base_address_;
    }
    [[nodiscard]] uint64_t end_address() const {
        return address_after_range_check(descriptor_->entries);
    }
    [[nodiscard]] uint64_t element_bytes() const noexcept {
        return Width / 8u;
    }
    [[nodiscard]] uint64_t index_from_offset(uint64_t byte_offset) const {
        return index_from_offset_for_range(byte_offset, 1);
    }
    [[nodiscard]] uint64_t index_from_absolute(
        uint64_t absolute_address) const {
        return index_from_absolute_for_range(absolute_address, 1);
    }
    [[nodiscard]] bool contains_absolute(
        uint64_t absolute_address) const noexcept {
        if (absolute_address < base_address_) return false;
        const uint64_t offset = absolute_address - base_address_;
        return offset % element_bytes() == 0 &&
               offset / element_bytes() < descriptor_->entries;
    }

   private:
    void require_readable() const {
        if (!register_readable(descriptor_->access)) {
            throw std::logic_error(
                "cpptb-vc: wide register memory is not readable: " +
                std::string{descriptor_->path});
        }
    }
    void require_writable() const {
        if (!register_writable(descriptor_->access)) {
            throw std::logic_error(
                "cpptb-vc: wide register memory is not writable: " +
                std::string{descriptor_->path});
        }
    }
    void validate_element() const {
        constexpr uint16_t bus_bits =
            std::numeric_limits<bus_data_type>::digits;
        const uint16_t access_width = descriptor_->access_width;
        if (Width % 8u != 0 || access_width == 0 || access_width % 8u != 0 ||
            access_width > bus_bits || Width % access_width != 0) {
            throw std::logic_error(
                "cpptb-vc: wide register memory requires a byte-aligned access width that divides the element and fits the frontdoor data width: " +
                std::string{descriptor_->path});
        }
        const uint16_t enabled_bytes = access_width / 8u;
        if (enabled_bytes >
            std::numeric_limits<byte_enable_type>::digits) {
            throw std::logic_error(
                "cpptb-vc: wide register memory access width exceeds the frontdoor byte-enable width: " +
                std::string{descriptor_->path});
        }
    }
    [[nodiscard]] uint32_t transfer_count() const noexcept {
        return Width / descriptor_->access_width;
    }
    [[nodiscard]] uint64_t transfer_bytes() const noexcept {
        return descriptor_->access_width / 8u;
    }
    [[nodiscard]] byte_enable_type all_transfer_bytes() const noexcept {
        const uint64_t bytes = transfer_bytes();
        return static_cast<byte_enable_type>((uint64_t{1} << bytes) - 1u);
    }
    [[nodiscard]] bus_data_type transfer_value(
        const data_type& value, uint32_t transfer) const noexcept {
        uint64_t result = 0;
        const std::size_t first = transfer * descriptor_->access_width;
        for (std::size_t bit = 0; bit < descriptor_->access_width; ++bit) {
            if (value.bit(first + bit)) result |= uint64_t{1} << bit;
        }
        return static_cast<bus_data_type>(result);
    }
    void set_transfer_value(data_type& value, uint32_t transfer,
                            bus_data_type source) const noexcept {
        const std::size_t first = transfer * descriptor_->access_width;
        for (std::size_t bit = 0; bit < descriptor_->access_width; ++bit) {
            value.set_bit(first + bit,
                          ((static_cast<uint64_t>(source) >> bit) & 1u) != 0);
        }
    }

    [[nodiscard]] uint64_t frontdoor_address(
        uint64_t index, const RegisterAddressMap<Master>* map) const {
        return map ? map->effective_address(*descriptor_, index)
                   : address_after_range_check(index);
    }

    coro::Task<read_response_type> frontdoor_read(
        uint64_t index, RegisterAddressMap<Master>* map = nullptr) {
        read_response_type result{.path = AccessPath::Frontdoor};
        data_type value;
        for (uint32_t transfer = 0; transfer < transfer_count(); ++transfer) {
            const uint64_t transfer_address =
                frontdoor_address(index, map) + transfer * transfer_bytes();
            const auto request = typename Master::read_request_type{
                static_cast<address_type>(transfer_address)};
            if (map) {
                result.transport =
                    co_await map->read(*descriptor_, index, request);
            } else {
                result.transport = co_await master_->read(request);
            }
            if (!result.transport.okay()) {
                result.failed_index = index;
                co_return result;
            }
            set_transfer_value(value, transfer, result.transport.data);
            ++result.transfers_completed;
        }
        result.data = value;
        co_return result;
    }
    coro::Task<write_response_type> frontdoor_write(
        uint64_t index, const data_type& value,
        RegisterAddressMap<Master>* map = nullptr) {
        write_response_type result{.path = AccessPath::Frontdoor};
        for (uint32_t transfer = 0; transfer < transfer_count(); ++transfer) {
            const uint64_t transfer_address =
                frontdoor_address(index, map) + transfer * transfer_bytes();
            const auto request = typename Master::write_request_type{
                static_cast<address_type>(transfer_address),
                transfer_value(value, transfer), all_transfer_bytes()};
            if (map) {
                result.transport =
                    co_await map->write(*descriptor_, index, request);
            } else {
                result.transport = co_await master_->write(request);
            }
            if (!result.transport.okay()) {
                result.failed_index = index;
                co_return result;
            }
            ++result.transfers_completed;
        }
        co_return result;
    }
    coro::Task<read_response_type> frontdoor_read(
        uint64_t first_index, std::span<data_type> values,
        RegisterAddressMap<Master>* map = nullptr) {
        read_response_type result{.path = AccessPath::Frontdoor};
        for (std::size_t offset = 0; offset < values.size(); ++offset) {
            const uint64_t index = first_index + offset;
            data_type value;
            for (uint32_t transfer = 0; transfer < transfer_count();
                 ++transfer) {
                const uint64_t transfer_address =
                    frontdoor_address(index, map) +
                    transfer * transfer_bytes();
                const auto request = typename Master::read_request_type{
                    static_cast<address_type>(transfer_address)};
                if (map) {
                    result.transport =
                        co_await map->read(*descriptor_, index, request);
                } else {
                    result.transport = co_await master_->read(request);
                }
                if (!result.transport.okay()) {
                    result.failed_index = index;
                    co_return result;
                }
                set_transfer_value(value, transfer, result.transport.data);
                ++result.transfers_completed;
            }
            values[offset] = value;
            result.data = value;
        }
        co_return result;
    }
    coro::Task<write_response_type> frontdoor_write(
        uint64_t first_index, std::span<const data_type> values,
        RegisterAddressMap<Master>* map = nullptr) {
        write_response_type result{.path = AccessPath::Frontdoor};
        for (std::size_t offset = 0; offset < values.size(); ++offset) {
            const uint64_t index = first_index + offset;
            for (uint32_t transfer = 0; transfer < transfer_count();
                 ++transfer) {
                const uint64_t transfer_address =
                    frontdoor_address(index, map) +
                    transfer * transfer_bytes();
                const auto request = typename Master::write_request_type{
                    static_cast<address_type>(transfer_address),
                    transfer_value(values[offset], transfer),
                    all_transfer_bytes()};
                if (map) {
                    result.transport =
                        co_await map->write(*descriptor_, index, request);
                } else {
                    result.transport = co_await master_->write(request);
                }
                if (!result.transport.okay()) {
                    result.failed_index = index;
                    co_return result;
                }
                ++result.transfers_completed;
            }
        }
        co_return result;
    }

    uint64_t index_from_offset_for_range(uint64_t byte_offset,
                                         std::size_t count) const {
        if (byte_offset % element_bytes() != 0) {
            throw std::invalid_argument(
                "cpptb-vc: wide register memory byte offset is not element-aligned: " +
                std::string{descriptor_->path});
        }
        const uint64_t index = byte_offset / element_bytes();
        require_range(index, count);
        return index;
    }
    uint64_t index_from_absolute_for_range(uint64_t absolute_address,
                                           std::size_t count) const {
        if (absolute_address < base_address_) fail_absolute(absolute_address);
        const uint64_t offset = absolute_address - base_address_;
        if (offset % element_bytes() != 0) {
            throw std::invalid_argument(
                "cpptb-vc: wide register memory absolute address is not element-aligned: " +
                std::string{descriptor_->path});
        }
        const uint64_t index = offset / element_bytes();
        if (index > descriptor_->entries ||
            count > descriptor_->entries - index ||
            (count != 0 && index == descriptor_->entries)) {
            fail_absolute(absolute_address);
        }
        return index;
    }
    uint64_t address_after_range_check(uint64_t index) const {
        if (index >
            (std::numeric_limits<uint64_t>::max() - base_address_) /
                element_bytes()) {
            throw std::overflow_error(
                "cpptb-vc: wide register memory indexed address overflows: " +
                std::string{descriptor_->path});
        }
        return base_address_ + index * element_bytes();
    }
    void require_range(uint64_t first_index, std::size_t count) const {
        if (first_index > descriptor_->entries ||
            count > descriptor_->entries - first_index ||
            (count != 0 && first_index == descriptor_->entries)) {
            throw std::out_of_range(
                "cpptb-vc: wide register memory range out of bounds: " +
                std::string{descriptor_->path});
        }
    }
    void require_backdoor() const {
        if (!backdoor_) {
            throw std::logic_error(
                "cpptb-vc: wide register memory has no backdoor: " +
                std::string{descriptor_->path});
        }
    }
    [[noreturn]] void fail_absolute(uint64_t absolute_address) const {
        throw std::out_of_range(
            "cpptb-vc: wide register memory absolute address is out of bounds: " +
            std::string{descriptor_->path} + " address=" +
            std::to_string(absolute_address));
    }

    Master* master_;
    const RegisterMemoryDescriptor* descriptor_;
    WideRegisterMemoryBackdoor* backdoor_ = nullptr;
    uint64_t base_address_ = 0;
};

template <MemoryMappedMaster Master>
    requires std::unsigned_integral<typename Master::data_type>
class RegisterHandle {
   public:
    using address_type = typename Master::address_type;
    using bus_data_type = typename Master::data_type;
    using data_type = uint64_t;
    using byte_enable_type = typename Master::byte_enable_type;
    using read_response_type =
        RegisterReadResponse<data_type, typename Master::read_response_type>;
    using write_response_type =
        RegisterWriteResponse<typename Master::write_response_type>;

    RegisterHandle(TestContext test, Master& master,
                   const RegisterDescriptor& descriptor,
                   RegisterBackdoor<data_type>* backdoor = nullptr,
                   RegisterUserEffectPolicy* user_effects = nullptr)
        : RegisterHandle(std::move(test), master, descriptor, 0, backdoor,
                         user_effects) {}

    RegisterHandle(TestContext test, Master& master,
                   const RegisterDescriptor& descriptor,
                   uint64_t base_address,
                   RegisterBackdoor<data_type>* backdoor = nullptr,
                   RegisterUserEffectPolicy* user_effects = nullptr)
        : test_(std::move(test)),
          master_(&master),
          descriptor_(&descriptor),
          backdoor_(backdoor),
          user_effects_(user_effects),
          desired_(static_cast<data_type>(
              descriptor.reset_value & descriptor.reset_mask &
              register_mask(descriptor.width))),
          mirrored_(desired_),
          desired_valid_mask_(static_cast<data_type>(
              descriptor.reset_mask & register_mask(descriptor.width))),
          mirrored_valid_mask_(desired_valid_mask_) {
        if (base_address >
            std::numeric_limits<uint64_t>::max() - descriptor.address) {
            throw std::invalid_argument(
                "cpptb-vc: register base plus offset overflows: " +
                std::string{descriptor.path});
        }
        effective_address_ = base_address + descriptor.address;
        register_detail::validate_field_layout(descriptor);
        if (descriptor.width % 8u == 0 &&
            descriptor.width / 8u >
                std::numeric_limits<uint64_t>::max() - effective_address_) {
            throw std::invalid_argument(
                "cpptb-vc: register address range overflows: " +
                std::string{descriptor.path});
        }
        if constexpr (std::integral<address_type>) {
            if (effective_address_ >
                static_cast<uint64_t>(std::numeric_limits<address_type>::max())) {
                throw std::invalid_argument(
                    "cpptb-vc: register address exceeds frontdoor address width: " +
                    std::string{descriptor.path});
            }
        }
        initialize_metadata();
        initialize_access_validation();
    }

    RegisterHandle(const RegisterHandle&) = delete;
    RegisterHandle& operator=(const RegisterHandle&) = delete;

    [[nodiscard]] bool has_user_effect_policy() const noexcept {
        return user_effects_ != nullptr;
    }

    void set_auto_predict(bool enabled) noexcept { auto_predict_ = enabled; }
    [[nodiscard]] bool auto_predict() const noexcept { return auto_predict_; }

    data_type desired() const {
        require_supported_access();
        return desired_;
    }
    data_type mirrored() const {
        require_supported_access();
        return mirrored_;
    }
    data_type desired_valid_mask() const {
        require_supported_access();
        return desired_valid_mask_;
    }
    data_type mirrored_valid_mask() const {
        require_supported_access();
        return mirrored_valid_mask_;
    }
    bool needs_update() const {
        require_supported_access();
        const data_type writable = static_cast<data_type>(writable_mask());
        return (desired_valid_mask_ & writable &
                ((desired_ ^ mirrored_) | ~mirrored_valid_mask_)) != 0;
    }

    void set_desired(data_type value) {
        require_supported_access();
        if (descriptor_->fields.empty()) {
            desired_ = static_cast<data_type>(value & width_mask_);
            desired_valid_mask_ = static_cast<data_type>(width_mask_);
            return;
        }
        uint64_t next = desired_;
        for (const auto& field : descriptor_->fields) {
            if (!register_writable(field.access)) continue;
            next = register_detail::insert_field(next, value >> field.lsb,
                                                 field);
        }
        desired_ = static_cast<data_type>(next);
        desired_valid_mask_ |= static_cast<data_type>(writable_mask());
    }

    void predict(data_type value,
                 RegisterPrediction prediction = RegisterPrediction::Direct) {
        require_supported_access();
        const uint64_t width_mask = width_mask_;
        uint64_t next = mirrored_;
        uint64_t desired_next = desired_;
        uint64_t next_valid = mirrored_valid_mask_;
        uint64_t desired_next_valid = desired_valid_mask_;
        if (prediction == RegisterPrediction::Direct ||
            (descriptor_->fields.empty() &&
             prediction == RegisterPrediction::Read)) {
            next = static_cast<uint64_t>(value) & width_mask;
            desired_next = next;
            next_valid = width_mask;
            desired_next_valid = width_mask;
        } else if (prediction == RegisterPrediction::Write) {
            predict_write_masked(value, width_mask);
            return;
        } else if (prediction == RegisterPrediction::Read) {
            predict_read_masked(value, width_mask);
            return;
        }
        mirrored_ = static_cast<data_type>(next & width_mask);
        desired_ = static_cast<data_type>(desired_next & width_mask);
        mirrored_valid_mask_ = static_cast<data_type>(next_valid & width_mask);
        desired_valid_mask_ =
            static_cast<data_type>(desired_next_valid & width_mask);
    }

    void predict_write(data_type value, byte_enable_type byte_enable) {
        require_supported_access();
        predict_write_masked(value, enabled_bit_mask(byte_enable));
    }

    uint64_t valid_byte_enable_mask() const {
        require_supported_access();
        const uint16_t bytes = access_width() / 8u;
        return bytes >= 64 ? std::numeric_limits<uint64_t>::max()
                           : (uint64_t{1} << bytes) - 1u;
    }

    [[nodiscard]] uint64_t end_address() const {
        require_supported_access();
        return effective_address_ + descriptor_->width / 8u;
    }

    [[nodiscard]] bool is_transfer_address(uint64_t address) const {
        require_supported_access();
        if (address < effective_address_ || address >= end_address()) {
            return false;
        }
        return (address - effective_address_) % (access_width() / 8u) == 0;
    }

    void predict_transfer_read(uint64_t address, bus_data_type value) {
        const uint32_t transfer = require_transfer_address(address);
        const uint16_t bit_offset = transfer_bit_offset(transfer);
        const uint64_t mask = chunk_mask_ << bit_offset;
        predict_read_masked(static_cast<uint64_t>(value) << bit_offset, mask);
    }

    void predict_transfer_write(uint64_t address, bus_data_type value,
                                byte_enable_type byte_enable) {
        const uint32_t transfer = require_transfer_address(address);
        const uint16_t bit_offset = transfer_bit_offset(transfer);
        const uint64_t mask = enabled_transfer_bit_mask(byte_enable)
                              << bit_offset;
        predict_write_masked(static_cast<uint64_t>(value) << bit_offset, mask);
    }

    void reset() {
        require_supported_access();
        desired_ = static_cast<data_type>(
            descriptor_->reset_value & descriptor_->reset_mask & width_mask_);
        mirrored_ = desired_;
        desired_valid_mask_ =
            static_cast<data_type>(descriptor_->reset_mask & width_mask_);
        mirrored_valid_mask_ = desired_valid_mask_;
        written_once_mask_ = 0;
    }

    coro::Task<read_response_type> read() {
        return read_locked(nullptr);
    }

    coro::Task<read_response_type> read(RegisterAddressMap<Master>& map) {
        return read_locked(&map);
    }

    coro::Task<write_response_type> write(data_type value) {
        co_await lock_.acquire();
        register_detail::LockGuard guard{lock_};
        co_return co_await write_unlocked(value, nullptr);
    }

    coro::Task<write_response_type> write(data_type value,
                                          RegisterAddressMap<Master>& map) {
        co_await lock_.acquire();
        register_detail::LockGuard guard{lock_};
        co_return co_await write_unlocked(value, &map);
    }

    coro::Task<write_response_type> update() {
        return update_locked(nullptr);
    }

    coro::Task<write_response_type> update(RegisterAddressMap<Master>& map) {
        return update_locked(&map);
    }

    coro::Task<read_response_type> mirror(
        MirrorCheck check = MirrorCheck::Enabled) {
        return mirror_locked(check, nullptr);
    }

    coro::Task<read_response_type> mirror(
        RegisterAddressMap<Master>& map,
        MirrorCheck check = MirrorCheck::Enabled) {
        return mirror_locked(check, &map);
    }

   private:
    coro::Task<write_response_type> update_locked(
        RegisterAddressMap<Master>* map) {
        co_await lock_.acquire();
        register_detail::LockGuard guard{lock_};
        if (!needs_update()) co_return write_response_type{};
        const auto unknown_desired = static_cast<data_type>(
            writable_mask() & ~static_cast<uint64_t>(desired_valid_mask_));
        if (unknown_desired != 0) {
            throw std::logic_error(
                "cpptb-vc: register update has unknown desired writable bits: " +
                std::string{descriptor_->path} + " mask=" +
                std::to_string(unknown_desired) +
                "; set the register, read it, or predict it first");
        }
        const data_type write_value = desired_write_value();
        const auto reachable_state = predicted_write_state(write_value);
        const data_type reachable = reachable_state.value;
        const data_type reachable_valid = reachable_state.valid_mask;
        const auto mask = static_cast<data_type>(
            writable_mask() & static_cast<uint64_t>(desired_valid_mask_));
        if (((reachable ^ desired_) & mask) != 0 ||
            (reachable_valid & mask) != mask) {
            test_.warn("register update cannot reach the requested desired "
                       "state: " +
                       std::string{descriptor_->path} + " requested=" +
                       std::to_string(desired_ & mask) + " reachable=" +
                       std::to_string(reachable & mask));
        }
        enforce_write_access();
        require_supported_access();
        write_response_type result;
        const uint64_t chunk_mask = chunk_mask_;
        for (uint32_t transfer = 0; transfer < transfer_count(); ++transfer) {
            const uint16_t bit_offset = transfer_bit_offset(transfer);
            const uint64_t address = transfer_address(transfer, map);
            const typename Master::write_request_type request{
                static_cast<address_type>(address),
                static_cast<bus_data_type>(
                    (write_value >> bit_offset) & chunk_mask),
                all_bytes()};
            typename Master::write_response_type response;
            if (map) {
                response = co_await map->write(*descriptor_, request);
            } else {
                response = co_await master_->write(request);
            }
            result.transport = response;
            if (!response.okay()) {
                result.failed_address = address;
                co_return result;
            }
            ++result.transfers_completed;
            if (!auto_predict_) continue;
            if (transfer_count() == 1) {
                commit_predicted_write_state(reachable_state);
            } else {
                predict_write_masked(write_value, chunk_mask << bit_offset);
            }
        }
        co_return result;
    }

    coro::Task<read_response_type> mirror_locked(
        MirrorCheck check, RegisterAddressMap<Master>* map) {
        co_await lock_.acquire();
        register_detail::LockGuard guard{lock_};
        const data_type previous = mirrored_;
        const data_type previous_valid = mirrored_valid_mask_;
        auto response = co_await frontdoor_read(map);
        if (check == MirrorCheck::Enabled) {
            const data_type compare_mask = static_cast<data_type>(
                nonvolatile_mask() & width_mask_ &
                previous_valid & response.valid_mask);
            if (compare_mask != 0) {
                test_.expect_eq(descriptor_->path,
                                response.data & compare_mask,
                                previous & compare_mask);
            }
        }
        if (auto_predict_) {
            predict_read_masked(response.data, response.valid_mask);
        }
        co_return response;
    }

   public:

    data_type peek() {
        require_backdoor();
        const data_type value =
            backdoor_->peek(*descriptor_, effective_address_);
        predict(value);
        return value;
    }

    void poke(data_type value) {
        require_backdoor();
        backdoor_->poke(*descriptor_, effective_address_, value);
        predict(value);
    }

    RegisterFieldHandle<Master> field(
        const RegisterFieldDescriptor& descriptor) {
        return RegisterFieldHandle<Master>{*this, descriptor};
    }

    const RegisterDescriptor& descriptor() const noexcept {
        return *descriptor_;
    }

    [[nodiscard]] std::string_view name() const noexcept {
        return descriptor_->name;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return descriptor_->path;
    }

    [[nodiscard]] std::span<const RegisterBackdoorSliceDescriptor>
    hdl_slices() const noexcept {
        return descriptor_->backdoor_slices;
    }

    [[nodiscard]] std::optional<std::string_view> hdl_path() const noexcept {
        if (descriptor_->backdoor_slices.size() != 1) return std::nullopt;
        return descriptor_->backdoor_slices.front().path;
    }

    [[nodiscard]] bool has_backdoor() const noexcept {
        return backdoor_ != nullptr;
    }

    [[nodiscard]] uint64_t address() const noexcept {
        return effective_address_;
    }

    [[nodiscard]] uint16_t width() const noexcept {
        return descriptor_->width;
    }

   private:
    friend class RegisterFieldHandle<Master>;

    void predict_write_masked(data_type value, uint64_t enabled_mask) {
        const uint64_t width_mask = width_mask_;
        enabled_mask &= width_mask;
        if (descriptor_->fields.empty()) {
            const uint64_t next =
                (static_cast<uint64_t>(mirrored_) & ~enabled_mask) |
                (static_cast<uint64_t>(value) & enabled_mask);
            const uint64_t next_valid =
                static_cast<uint64_t>(mirrored_valid_mask_) | enabled_mask;
            mirrored_ = static_cast<data_type>(next & width_mask);
            desired_ = mirrored_;
            mirrored_valid_mask_ =
                static_cast<data_type>(next_valid & width_mask);
            desired_valid_mask_ = mirrored_valid_mask_;
            return;
        }

        uint64_t next = mirrored_;
        uint64_t next_valid = mirrored_valid_mask_;
        for (const auto& field : descriptor_->fields) {
            const uint64_t field_enabled = register_field_mask(field) &
                                           enabled_mask & ~written_once_mask_;
            if (!register_writable(field.access) || field_enabled == 0) {
                continue;
            }
            uint64_t effected = next;
            uint64_t effected_valid = next_valid;
            if (field.write_effect == RegisterWriteEffect::User &&
                user_effects_) {
                const uint64_t field_mask = register_mask(field.width);
                const auto result = user_effects_->predict_write_field(
                    RegisterUserEffectFieldContext{
                        *descriptor_, field,
                        (next >> field.lsb) & field_mask,
                        (next_valid >> field.lsb) & field_mask,
                        (static_cast<uint64_t>(value) >> field.lsb) &
                            field_mask});
                effected = (effected & ~register_field_mask(field)) |
                            ((result.value & field_mask) << field.lsb);
                effected_valid =
                    (effected_valid & ~register_field_mask(field)) |
                    ((result.valid_mask & field_mask) << field.lsb);
            } else {
                effected =
                    register_detail::apply_write_effect(next, value, field);
                effected_valid = register_detail::apply_write_valid_mask(
                    next_valid, value, field);
            }
            next = (next & ~field_enabled) | (effected & field_enabled);
            next_valid = (next_valid & ~field_enabled) |
                         (effected_valid & field_enabled);
        }
        const uint64_t mask = writable_mask() & enabled_mask;
        const uint64_t desired_next =
            (static_cast<uint64_t>(desired_) & ~mask) | (next & mask);
        const uint64_t desired_next_valid =
            (static_cast<uint64_t>(desired_valid_mask_) & ~mask) |
            (next_valid & mask);
        mirrored_ = static_cast<data_type>(next & width_mask);
        desired_ = static_cast<data_type>(desired_next & width_mask);
        mirrored_valid_mask_ = static_cast<data_type>(next_valid & width_mask);
        desired_valid_mask_ =
            static_cast<data_type>(desired_next_valid & width_mask);
        written_once_mask_ |= write_once_mask() & enabled_mask;
    }

    void predict_read_masked(data_type value, uint64_t enabled_mask) {
        const uint64_t width_mask = width_mask_;
        enabled_mask &= width_mask;
        if (enabled_mask == 0) return;
        if (descriptor_->fields.empty()) {
            mirrored_ = (mirrored_ & ~enabled_mask) | (value & enabled_mask);
            desired_ = (desired_ & ~enabled_mask) | (mirrored_ & enabled_mask);
            mirrored_valid_mask_ |= enabled_mask;
            desired_valid_mask_ |= enabled_mask;
            return;
        }

        uint64_t next = mirrored_;
        uint64_t next_valid = mirrored_valid_mask_;
        uint64_t desired_next = desired_;
        uint64_t desired_next_valid = desired_valid_mask_;
        for (const auto& field : descriptor_->fields) {
            const uint64_t field_enabled =
                register_field_mask(field) & enabled_mask;
            if (!register_readable(field.access) || field_enabled == 0) continue;
            const uint64_t sampled =
                (next & ~field_enabled) | (value & field_enabled);
            uint64_t effected = sampled;
            uint64_t effected_valid = next_valid;
            if (field.read_effect == RegisterReadEffect::User &&
                user_effects_) {
                const uint64_t field_mask = register_mask(field.width);
                const auto result = user_effects_->predict_read_field(
                    RegisterUserEffectFieldContext{
                        *descriptor_, field,
                        (next >> field.lsb) & field_mask,
                        (next_valid >> field.lsb) & field_mask,
                        (static_cast<uint64_t>(value) >> field.lsb) &
                            field_mask});
                effected = (effected & ~register_field_mask(field)) |
                            ((result.value & field_mask) << field.lsb);
                effected_valid =
                    (effected_valid & ~register_field_mask(field)) |
                    ((result.valid_mask & field_mask) << field.lsb);
            } else {
                effected = register_detail::apply_read_effect(sampled, field);
                effected_valid = register_detail::apply_read_valid_mask(
                    next_valid, field);
            }
            next = (next & ~field_enabled) | (effected & field_enabled);
            next_valid = (next_valid & ~field_enabled) |
                         (effected_valid & field_enabled);
            desired_next = (desired_next & ~field_enabled) |
                           (next & field_enabled);
            desired_next_valid = (desired_next_valid & ~field_enabled) |
                                 (next_valid & field_enabled);
        }
        mirrored_ = next & width_mask;
        desired_ = desired_next & width_mask;
        mirrored_valid_mask_ = next_valid & width_mask;
        desired_valid_mask_ = desired_next_valid & width_mask;
    }

    uint64_t enabled_bit_mask(byte_enable_type byte_enable) const noexcept {
        uint64_t enabled = 0;
        const uint16_t bytes = descriptor_->width / 8u;
        for (uint16_t index = 0; index < bytes; ++index) {
            if (((static_cast<uint64_t>(byte_enable) >> index) & 1u) != 0) {
                enabled |= uint64_t{0xff} << (index * 8u);
            }
        }
        return enabled;
    }

    uint64_t enabled_transfer_bit_mask(
        byte_enable_type byte_enable) const noexcept {
        uint64_t enabled = 0;
        const uint16_t bytes = access_width() / 8u;
        for (uint16_t index = 0; index < bytes; ++index) {
            if (((static_cast<uint64_t>(byte_enable) >> index) & 1u) != 0) {
                enabled |= uint64_t{0xff} << (index * 8u);
            }
        }
        return enabled;
    }

    uint32_t require_transfer_address(uint64_t address) const {
        if (!is_transfer_address(address)) {
            throw std::logic_error(
                "cpptb-vc: register predictor transaction is not aligned to "
                "an accesswidth transfer: " +
                std::string{descriptor_->path} + " address=" +
                std::to_string(address));
        }
        return static_cast<uint32_t>(
            (address - effective_address_) / (access_width() / 8u));
    }

    void set_field_desired(data_type value,
                           const RegisterFieldDescriptor& field) {
        require_supported_access();
        desired_ = static_cast<data_type>(register_detail::insert_field(
            desired_, static_cast<uint64_t>(value), field));
        desired_valid_mask_ |= static_cast<data_type>(register_field_mask(field));
    }

    coro::Task<read_response_type> read_locked(
        RegisterAddressMap<Master>* map) {
        co_await lock_.acquire();
        register_detail::LockGuard guard{lock_};
        if (!has_readable_field()) {
            throw std::logic_error("cpptb-vc: register is not readable: " +
                                   std::string{descriptor_->path});
        }
        require_supported_access();
        read_response_type result;
        const uint64_t chunk_mask = chunk_mask_;
        for (uint32_t transfer = 0; transfer < transfer_count(); ++transfer) {
            const uint16_t bit_offset = transfer_bit_offset(transfer);
            const uint64_t address = transfer_address(transfer, map);
            const typename Master::read_request_type request{
                static_cast<address_type>(address)};
            typename Master::read_response_type response;
            if (map) {
                response = co_await map->read(*descriptor_, request);
            } else {
                response = co_await master_->read(request);
            }
            result.transport = response;
            if (!response.okay()) {
                result.failed_address = address;
                if (auto_predict_) {
                    predict_read_masked(result.data, result.valid_mask);
                }
                co_return result;
            }
            ++result.transfers_completed;
            const uint64_t enabled = chunk_mask << bit_offset;
            result.data =
                (result.data & ~enabled) |
                ((static_cast<uint64_t>(response.data) & chunk_mask)
                 << bit_offset);
            result.valid_mask |= enabled;
        }
        if (auto_predict_) {
            predict_read_masked(result.data, result.valid_mask);
        }
        co_return result;
    }

    coro::Task<write_response_type> write_unlocked(
        data_type value, RegisterAddressMap<Master>* map) {
        enforce_write_access();
        require_supported_access();
        value &= width_mask_;
        write_response_type result;
        const uint64_t chunk_mask = chunk_mask_;
        for (uint32_t transfer = 0; transfer < transfer_count(); ++transfer) {
            const uint16_t bit_offset = transfer_bit_offset(transfer);
            const uint64_t address =
                transfer_address(transfer, map);
            const typename Master::write_request_type request{
                static_cast<address_type>(address),
                static_cast<bus_data_type>((value >> bit_offset) & chunk_mask),
                all_bytes()};
            typename Master::write_response_type response;
            if (map) {
                response = co_await map->write(*descriptor_, request);
            } else {
                response = co_await master_->write(request);
            }
            result.transport = response;
            if (!response.okay()) {
                result.failed_address = address;
                co_return result;
            }
            ++result.transfers_completed;
            if (auto_predict_) {
                predict_write_masked(value, chunk_mask << bit_offset);
            }
        }
        co_return result;
    }

    coro::Task<read_response_type> frontdoor_read(
        RegisterAddressMap<Master>* map) {
        if (!has_readable_field()) {
            throw std::logic_error("cpptb-vc: register is not readable: " +
                                   std::string{descriptor_->path});
        }
        require_supported_access();
        read_response_type result;
        const uint64_t chunk_mask = chunk_mask_;
        for (uint32_t transfer = 0; transfer < transfer_count(); ++transfer) {
            const uint16_t bit_offset = transfer_bit_offset(transfer);
            const uint64_t address = transfer_address(transfer, map);
            const typename Master::read_request_type request{
                static_cast<address_type>(address)};
            typename Master::read_response_type response;
            if (map) {
                response = co_await map->read(*descriptor_, request);
            } else {
                response = co_await master_->read(request);
            }
            result.transport = response;
            if (!response.okay()) {
                result.failed_address = address;
                co_return result;
            }
            ++result.transfers_completed;
            const uint64_t enabled = chunk_mask << bit_offset;
            result.data = (result.data & ~enabled) |
                          ((static_cast<uint64_t>(response.data) & chunk_mask)
                           << bit_offset);
            result.valid_mask |= enabled;
        }
        co_return result;
    }

    byte_enable_type all_bytes() const {
        return all_bytes_;
    }

    uint16_t access_width() const noexcept {
        return access_width_;
    }

    uint32_t transfer_count() const noexcept {
        return transfer_count_;
    }

    uint16_t transfer_bit_offset(uint32_t transfer) const noexcept {
        const uint32_t logical =
            descriptor_->endianness == RegisterEndianness::Little
                ? transfer
                : transfer_count() - transfer - 1u;
        return static_cast<uint16_t>(logical * access_width());
    }

    uint64_t transfer_address(
        uint32_t transfer, RegisterAddressMap<Master>* map = nullptr) const {
        const uint64_t base =
            map ? map->effective_address(*descriptor_) : effective_address_;
        return base +
               static_cast<uint64_t>(transfer) * (access_width() / 8u);
    }

    bool has_readable_field() const noexcept {
        return has_readable_field_;
    }

    bool has_writable_field() const noexcept {
        return has_writable_field_;
    }

    bool write_once_only() const noexcept {
        return write_once_only_;
    }


    uint64_t write_once_mask() const noexcept {
        return write_once_field_mask_;
    }

    data_type desired_write_value() const {
        if (descriptor_->fields.empty()) return desired_;
        uint64_t written = 0;
        for (const auto& field : descriptor_->fields) {
            if (!register_writable(field.access)) continue;
            if (field.write_effect == RegisterWriteEffect::User &&
                user_effects_) {
                const uint64_t field_mask = register_mask(field.width);
                const uint64_t encoded = user_effects_->encode_write_field(
                    RegisterUserEffectFieldContext{
                        *descriptor_, field,
                        (mirrored_ >> field.lsb) & field_mask,
                        (mirrored_valid_mask_ >> field.lsb) & field_mask,
                        (desired_ >> field.lsb) & field_mask});
                written |= (encoded & field_mask) << field.lsb;
            } else {
                written |= register_detail::encode_desired_write(
                    mirrored_, mirrored_valid_mask_, desired_, field);
            }
        }
        return static_cast<data_type>(written);
    }

    struct PredictedWriteState {
        data_type value = 0;
        data_type valid_mask = 0;
    };

    PredictedWriteState predicted_write_state(data_type written) const {
        if (descriptor_->fields.empty()) {
            return {
                static_cast<data_type>(written & width_mask_),
                static_cast<data_type>(width_mask_),
            };
        }
        uint64_t next = mirrored_;
        uint64_t next_valid = mirrored_valid_mask_;
        for (const auto& field : descriptor_->fields) {
            if (register_writable(field.access) &&
                (written_once_mask_ & register_field_mask(field)) == 0) {
                if (field.write_effect == RegisterWriteEffect::User &&
                    user_effects_) {
                    const uint64_t field_mask = register_mask(field.width);
                    const auto result = user_effects_->predict_write_field(
                        RegisterUserEffectFieldContext{
                            *descriptor_, field,
                            (next >> field.lsb) & field_mask,
                            (next_valid >> field.lsb) & field_mask,
                            (static_cast<uint64_t>(written) >> field.lsb) &
                                field_mask});
                    const uint64_t positioned_mask = field_mask << field.lsb;
                    next = (next & ~positioned_mask) |
                           ((result.value & field_mask) << field.lsb);
                    next_valid =
                        (next_valid & ~positioned_mask) |
                        ((result.valid_mask & field_mask) << field.lsb);
                } else {
                    next = register_detail::apply_write_effect(next, written,
                                                               field);
                    next_valid = register_detail::apply_write_valid_mask(
                        next_valid, written, field);
                }
            }
        }
        return {
            static_cast<data_type>(next & width_mask_),
            static_cast<data_type>(next_valid & width_mask_),
        };
    }

    void commit_predicted_write_state(
        const PredictedWriteState& predicted) noexcept {
        const uint64_t mask = writable_mask() & width_mask_;
        desired_ = static_cast<data_type>(
            (static_cast<uint64_t>(desired_) & ~mask) |
            (static_cast<uint64_t>(predicted.value) & mask));
        desired_valid_mask_ = static_cast<data_type>(
            (static_cast<uint64_t>(desired_valid_mask_) & ~mask) |
            (static_cast<uint64_t>(predicted.valid_mask) & mask));
        mirrored_ = static_cast<data_type>(predicted.value & width_mask_);
        mirrored_valid_mask_ =
            static_cast<data_type>(predicted.valid_mask & width_mask_);
        written_once_mask_ |= write_once_mask() & width_mask_;
    }

    void enforce_write_access() const {
        if (!has_writable_field()) {
            throw std::logic_error("cpptb-vc: register is not writable: " +
                                   std::string{descriptor_->path});
        }
        if (written_once_mask_ != 0 && write_once_only()) {
            throw std::logic_error(
                "cpptb-vc: write-once register was already written: " +
                std::string{descriptor_->path});
        }
    }

    uint64_t nonvolatile_mask() const noexcept {
        return nonvolatile_field_mask_;
    }

    uint64_t writable_mask() const noexcept {
        return writable_field_mask_;
    }

    void initialize_metadata() noexcept {
        width_mask_ = register_mask(descriptor_->width);
        access_width_ = descriptor_->access_width == 0
                            ? descriptor_->width
                            : descriptor_->access_width;
        chunk_mask_ = register_mask(access_width_);
        const uint16_t bytes = access_width_ / 8u;
        const uint64_t byte_mask = bytes >= 64
                                       ? std::numeric_limits<uint64_t>::max()
                                       : (uint64_t{1} << bytes) - 1u;
        all_bytes_ = static_cast<byte_enable_type>(byte_mask);
        transfer_count_ = access_width_ == 0
                              ? 0
                              : descriptor_->width / access_width_;
        if (descriptor_->fields.empty()) {
            has_readable_field_ = true;
            has_writable_field_ = true;
            nonvolatile_field_mask_ = width_mask_;
            writable_field_mask_ = width_mask_;
            return;
        }
        bool saw_writable = false;
        bool only_write_once = true;
        for (const auto& field : descriptor_->fields) {
            const uint64_t mask = register_field_mask(field);
            if (register_readable(field.access)) {
                has_readable_field_ = true;
                if (!field.volatile_value) nonvolatile_field_mask_ |= mask;
            }
            if (!register_writable(field.access)) continue;
            has_writable_field_ = true;
            saw_writable = true;
            writable_field_mask_ |= mask;
            if (field.access == RegisterAccess::WriteOnce ||
                field.access == RegisterAccess::ReadWriteOnce) {
                write_once_field_mask_ |= mask;
            } else {
                only_write_once = false;
            }
        }
        write_once_only_ = saw_writable && only_write_once;
    }

    enum class AccessValidationError : uint8_t {
        None,
        RegisterWidth,
        TransferWidth,
        AddressWidth,
    };

    void initialize_access_validation() noexcept {
        if (descriptor_->width == 0 || descriptor_->width % 8 != 0 ||
            descriptor_->width > std::numeric_limits<data_type>::digits) {
            access_validation_error_ = AccessValidationError::RegisterWidth;
            return;
        }
        const uint16_t transfer_width = access_width_;
        if (transfer_width == 0 || transfer_width % 8 != 0 ||
            transfer_width > descriptor_->width ||
            descriptor_->width % transfer_width != 0 ||
            transfer_width > std::numeric_limits<bus_data_type>::digits) {
            access_validation_error_ = AccessValidationError::TransferWidth;
            return;
        }
        if constexpr (std::integral<address_type>) {
            if (transfer_address(transfer_count() - 1u) >
                static_cast<uint64_t>(
                    std::numeric_limits<address_type>::max())) {
                access_validation_error_ = AccessValidationError::AddressWidth;
            }
        }
    }

    void require_supported_access() const {
        if (access_validation_error_ != AccessValidationError::None) {
            throw_access_validation_error();
        }
    }

    [[noreturn]] void throw_access_validation_error() const {
        switch (access_validation_error_) {
            case AccessValidationError::RegisterWidth:
                throw std::logic_error(
                    "cpptb-vc: register must be byte-aligned and no wider than "
                    "64 bits: " +
                    std::string{descriptor_->path});
            case AccessValidationError::TransferWidth:
                throw std::logic_error(
                    "cpptb-vc: register accesswidth must be byte-aligned, divide "
                    "regwidth, and fit the frontdoor data width: " +
                    std::string{descriptor_->path});
            case AccessValidationError::AddressWidth:
                throw std::logic_error(
                    "cpptb-vc: split register transfer address exceeds the "
                    "frontdoor address width: " +
                    std::string{descriptor_->path});
            case AccessValidationError::None:
                break;
        }
        std::abort();
    }

    void require_backdoor() const {
        if (!backdoor_) {
            throw std::logic_error(
                "cpptb-vc: register has no backdoor adapter: " +
                std::string{descriptor_->path});
        }
    }

    TestContext test_;
    Master* master_;
    const RegisterDescriptor* descriptor_;
    RegisterBackdoor<data_type>* backdoor_;
    RegisterUserEffectPolicy* user_effects_ = nullptr;
    bool auto_predict_ = true;
    coro::Lock lock_;
    data_type desired_{};
    data_type mirrored_{};
    data_type desired_valid_mask_{};
    data_type mirrored_valid_mask_{};
    uint64_t effective_address_ = 0;
    uint64_t written_once_mask_ = 0;
    uint64_t width_mask_ = 0;
    uint64_t chunk_mask_ = 0;
    uint64_t nonvolatile_field_mask_ = 0;
    uint64_t writable_field_mask_ = 0;
    uint64_t write_once_field_mask_ = 0;
    uint32_t transfer_count_ = 0;
    uint16_t access_width_ = 0;
    byte_enable_type all_bytes_{};
    bool has_readable_field_ = false;
    bool has_writable_field_ = false;
    bool write_once_only_ = false;
    AccessValidationError access_validation_error_ =
        AccessValidationError::None;
};

template <std::size_t Width, MemoryMappedMaster Master>
    requires std::unsigned_integral<typename Master::data_type>
class WideRegisterHandle;

template <std::size_t RegisterWidth, std::size_t FieldWidth,
          MemoryMappedMaster Master>
    requires std::unsigned_integral<typename Master::data_type>
class WideRegisterFieldHandle {
   public:
    using data_type = Bits<FieldWidth>;
    using transport_read_response_type = typename Master::read_response_type;
    using parent_type = WideRegisterHandle<RegisterWidth, Master>;
    using read_response_type = RegisterReadResponse<
        data_type, typename Master::read_response_type>;
    using write_response_type =
        RegisterWriteResponse<typename Master::write_response_type>;

    WideRegisterFieldHandle(parent_type& parent,
                            const RegisterFieldDescriptor& descriptor)
        : parent_(&parent), descriptor_(&descriptor) {
        if (descriptor.width != FieldWidth ||
            descriptor.lsb + FieldWidth > RegisterWidth) {
            throw std::invalid_argument(
                "cpptb-vc: generated wide field width does not match its "
                "descriptor: " +
                std::string{descriptor.path});
        }
    }

    [[nodiscard]] data_type desired() const {
        return parent_->desired().template slice<FieldWidth>(descriptor_->lsb);
    }
    [[nodiscard]] data_type mirrored() const {
        return parent_->mirrored().template slice<FieldWidth>(descriptor_->lsb);
    }
    [[nodiscard]] data_type desired_valid_mask() const {
        return parent_->desired_valid_mask().template slice<FieldWidth>(
            descriptor_->lsb);
    }
    [[nodiscard]] data_type mirrored_valid_mask() const {
        return parent_->mirrored_valid_mask().template slice<FieldWidth>(
            descriptor_->lsb);
    }

    void set_desired(data_type value) {
        if (!register_writable(descriptor_->access)) {
            throw std::logic_error("cpptb-vc: field is not writable: " +
                                   std::string{descriptor_->path});
        }
        parent_->template set_field_desired<FieldWidth>(value, *descriptor_);
    }

    coro::Task<read_response_type> read() {
        const auto parent_response = co_await parent_->read();
        read_response_type response{
            .data = parent_response.data.template slice<FieldWidth>(
                descriptor_->lsb),
            .transport = parent_response.transport,
            .valid_mask = parent_response.valid_mask.template slice<FieldWidth>(
                descriptor_->lsb),
            .transfers_completed = parent_response.transfers_completed,
            .failed_address = parent_response.failed_address,
        };
        co_return response;
    }

    coro::Task<read_response_type> read(RegisterAddressMap<Master>& map) {
        const auto parent_response = co_await parent_->read(map);
        read_response_type response{
            .data = parent_response.data.template slice<FieldWidth>(
                descriptor_->lsb),
            .transport = parent_response.transport,
            .valid_mask = parent_response.valid_mask.template slice<FieldWidth>(
                descriptor_->lsb),
            .transfers_completed = parent_response.transfers_completed,
            .failed_address = parent_response.failed_address,
        };
        co_return response;
    }

    coro::Task<write_response_type> write(data_type value) {
        set_desired(value);
        co_return co_await parent_->update();
    }

    coro::Task<write_response_type> write(data_type value,
                                          RegisterAddressMap<Master>& map) {
        set_desired(value);
        co_return co_await parent_->update(map);
    }

    const RegisterFieldDescriptor& descriptor() const noexcept {
        return *descriptor_;
    }

    [[nodiscard]] std::string_view name() const noexcept {
        return descriptor_->name;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return descriptor_->path;
    }

    [[nodiscard]] std::span<const RegisterBackdoorSliceDescriptor>
    hdl_slices() const noexcept {
        return descriptor_->backdoor_slices;
    }

    [[nodiscard]] std::optional<std::string_view> hdl_path() const noexcept {
        if (descriptor_->backdoor_slices.size() != 1) return std::nullopt;
        return descriptor_->backdoor_slices.front().path;
    }

    [[nodiscard]] uint64_t address() const noexcept {
        return parent_->address();
    }

    [[nodiscard]] uint16_t lsb() const noexcept { return descriptor_->lsb; }

    [[nodiscard]] uint16_t width() const noexcept {
        return descriptor_->width;
    }

   private:
    parent_type* parent_;
    const RegisterFieldDescriptor* descriptor_;
};

template <typename Enum, typename RawFieldHandle>
    requires std::is_enum_v<Enum>
class RegisterEnumFieldHandle {
   public:
    using enum_type = Enum;
    using raw_field_type = RawFieldHandle;
    using raw_data_type = typename raw_field_type::data_type;
    using transport_read_response_type =
        typename raw_field_type::transport_read_response_type;
    using read_response_type = RegisterReadResponse<
        enum_type, transport_read_response_type, raw_data_type>;
    using write_response_type = typename raw_field_type::write_response_type;

    template <typename Parent>
    RegisterEnumFieldHandle(Parent& parent,
                            const RegisterFieldDescriptor& descriptor)
        : raw_(parent, descriptor) {}

    [[nodiscard]] enum_type desired() const {
        return decode(raw_.desired());
    }
    [[nodiscard]] enum_type mirrored() const {
        return decode(raw_.mirrored());
    }
    [[nodiscard]] raw_data_type desired_valid_mask() const {
        return raw_.desired_valid_mask();
    }
    [[nodiscard]] raw_data_type mirrored_valid_mask() const {
        return raw_.mirrored_valid_mask();
    }

    void set_desired(enum_type value) { raw_.set_desired(encode(value)); }

    coro::Task<read_response_type> read() {
        const auto raw_response = co_await raw_.read();
        co_return read_response_type{
            .data = decode(raw_response.data),
            .transport = raw_response.transport,
            .valid_mask = raw_response.valid_mask,
            .transfers_completed = raw_response.transfers_completed,
            .failed_address = raw_response.failed_address,
        };
    }

    template <typename Map>
        requires requires(raw_field_type& raw, Map& map) { raw.read(map); }
    coro::Task<read_response_type> read(Map& map) {
        const auto raw_response = co_await raw_.read(map);
        co_return read_response_type{
            .data = decode(raw_response.data),
            .transport = raw_response.transport,
            .valid_mask = raw_response.valid_mask,
            .transfers_completed = raw_response.transfers_completed,
            .failed_address = raw_response.failed_address,
        };
    }

    coro::Task<write_response_type> write(enum_type value) {
        co_return co_await raw_.write(encode(value));
    }

    template <typename Map>
        requires requires(raw_field_type& raw, Map& map,
                          raw_data_type value) { raw.write(value, map); }
    coro::Task<write_response_type> write(enum_type value, Map& map) {
        co_return co_await raw_.write(encode(value), map);
    }

    [[nodiscard]] raw_field_type& raw() noexcept { return raw_; }
    [[nodiscard]] const raw_field_type& raw() const noexcept { return raw_; }

    const RegisterFieldDescriptor& descriptor() const noexcept {
        return raw_.descriptor();
    }

    [[nodiscard]] std::string_view name() const noexcept {
        return raw_.name();
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return raw_.path();
    }

    [[nodiscard]] auto hdl_slices() const noexcept {
        return raw_.hdl_slices();
    }

    [[nodiscard]] std::optional<std::string_view> hdl_path() const noexcept {
        return raw_.hdl_path();
    }

    [[nodiscard]] uint64_t address() const noexcept {
        return raw_.address();
    }

    [[nodiscard]] uint16_t lsb() const noexcept { return raw_.lsb(); }

    [[nodiscard]] uint16_t width() const noexcept { return raw_.width(); }

   private:
    static enum_type decode(const raw_data_type& value) {
        if constexpr (std::integral<raw_data_type>) {
            return static_cast<enum_type>(value);
        } else {
            return static_cast<enum_type>(value.to_uint64());
        }
    }

    static raw_data_type encode(enum_type value) {
        const auto raw = static_cast<uint64_t>(value);
        if constexpr (std::integral<raw_data_type>) {
            return static_cast<raw_data_type>(raw);
        } else {
            return raw_data_type::from_uint(raw);
        }
    }

    raw_field_type raw_;
};

template <std::size_t Width, MemoryMappedMaster Master>
    requires std::unsigned_integral<typename Master::data_type>
class WideRegisterHandle {
    static_assert(Width > 64, "WideRegisterHandle is for registers over 64 bits");

   public:
    using address_type = typename Master::address_type;
    using bus_data_type = typename Master::data_type;
    using byte_enable_type = typename Master::byte_enable_type;
    using data_type = Bits<Width>;
    using read_response_type = RegisterReadResponse<
        data_type, typename Master::read_response_type>;
    using write_response_type =
        RegisterWriteResponse<typename Master::write_response_type>;

    WideRegisterHandle(TestContext test, Master& master,
                       const RegisterDescriptor& descriptor,
                       WideRegisterBackdoor* backdoor = nullptr,
                       RegisterUserEffectPolicy* user_effects = nullptr)
        : WideRegisterHandle(std::move(test), master, descriptor, 0,
                             backdoor, user_effects) {}

    WideRegisterHandle(TestContext test, Master& master,
                       const RegisterDescriptor& descriptor,
                       uint64_t base_address,
                       WideRegisterBackdoor* backdoor = nullptr,
                       RegisterUserEffectPolicy* user_effects = nullptr)
        : test_(std::move(test)),
          master_(&master),
          descriptor_(&descriptor),
          backdoor_(backdoor),
          user_effects_(user_effects) {
        if (descriptor.width != Width) {
            throw std::invalid_argument(
                "cpptb-vc: generated wide register width does not match its "
                "descriptor: " +
                std::string{descriptor.path});
        }
        register_detail::validate_field_layout(descriptor);
        if (base_address >
            std::numeric_limits<uint64_t>::max() - descriptor.address) {
            throw std::invalid_argument(
                "cpptb-vc: register base plus offset overflows: " +
                std::string{descriptor.path});
        }
        effective_address_ = base_address + descriptor.address;
        if (Width / 8u >
            std::numeric_limits<uint64_t>::max() - effective_address_) {
            throw std::invalid_argument(
                "cpptb-vc: wide register address range overflows: " +
                std::string{descriptor.path});
        }
        validate_access();
        reset();
    }

    WideRegisterHandle(const WideRegisterHandle&) = delete;
    WideRegisterHandle& operator=(const WideRegisterHandle&) = delete;

    [[nodiscard]] bool has_user_effect_policy() const noexcept {
        return user_effects_ != nullptr;
    }

    void set_auto_predict(bool enabled) noexcept { auto_predict_ = enabled; }
    [[nodiscard]] bool auto_predict() const noexcept { return auto_predict_; }

    [[nodiscard]] data_type desired() const noexcept { return desired_; }
    [[nodiscard]] data_type mirrored() const noexcept { return mirrored_; }
    [[nodiscard]] data_type desired_valid_mask() const noexcept {
        return desired_valid_mask_;
    }
    [[nodiscard]] data_type mirrored_valid_mask() const noexcept {
        return mirrored_valid_mask_;
    }

    [[nodiscard]] bool needs_update() const noexcept {
        const auto writable = writable_mask();
        for (std::size_t bit = 0; bit < Width; ++bit) {
            if (writable.bit(bit) && desired_valid_mask_.bit(bit) &&
                (!mirrored_valid_mask_.bit(bit) ||
                 desired_.bit(bit) != mirrored_.bit(bit))) {
                return true;
            }
        }
        return false;
    }

    void set_desired(const data_type& value) {
        if (descriptor_->fields.empty()) {
            desired_ = value;
            desired_valid_mask_ = ones();
            return;
        }
        for (const auto& field : descriptor_->fields) {
            if (!register_writable(field.access)) continue;
            for_each_field_bit(field, [&](std::size_t bit) {
                desired_.set_bit(bit, value.bit(bit));
                desired_valid_mask_.set_bit(bit, true);
            });
        }
    }

    void predict(const data_type& value,
                 RegisterPrediction prediction = RegisterPrediction::Direct) {
        if (prediction == RegisterPrediction::Direct) {
            desired_ = value;
            mirrored_ = value;
            desired_valid_mask_ = ones();
            mirrored_valid_mask_ = ones();
        } else if (prediction == RegisterPrediction::Read) {
            predict_read_masked(value, ones());
        } else {
            predict_write_masked(value, ones());
        }
    }

    void reset() {
        desired_ = descriptor_bits(descriptor_->reset_value_words,
                                   descriptor_->reset_value);
        mirrored_ = desired_;
        desired_valid_mask_ = descriptor_bits(
            descriptor_->reset_mask_words, descriptor_->reset_mask);
        mirrored_valid_mask_ = desired_valid_mask_;
        written_once_mask_ = {};
    }

    coro::Task<read_response_type> read() {
        co_await lock_.acquire();
        register_detail::LockGuard guard{lock_};
        auto response = co_await frontdoor_read(nullptr);
        if (auto_predict_) {
            predict_read_masked(response.data, response.valid_mask);
        }
        co_return response;
    }

    coro::Task<read_response_type> read(RegisterAddressMap<Master>& map) {
        co_await lock_.acquire();
        register_detail::LockGuard guard{lock_};
        auto response = co_await frontdoor_read(&map);
        if (auto_predict_) {
            predict_read_masked(response.data, response.valid_mask);
        }
        co_return response;
    }

    coro::Task<write_response_type> write(const data_type& value) {
        co_await lock_.acquire();
        register_detail::LockGuard guard{lock_};
        co_return co_await write_unlocked(value, nullptr);
    }

    coro::Task<write_response_type> write(const data_type& value,
                                          RegisterAddressMap<Master>& map) {
        co_await lock_.acquire();
        register_detail::LockGuard guard{lock_};
        co_return co_await write_unlocked(value, &map);
    }

    coro::Task<write_response_type> update() {
        co_await lock_.acquire();
        register_detail::LockGuard guard{lock_};
        co_return co_await update_unlocked(nullptr);
    }

    coro::Task<write_response_type> update(RegisterAddressMap<Master>& map) {
        co_await lock_.acquire();
        register_detail::LockGuard guard{lock_};
        co_return co_await update_unlocked(&map);
    }

    coro::Task<read_response_type> mirror(
        MirrorCheck check = MirrorCheck::Enabled) {
        co_await lock_.acquire();
        register_detail::LockGuard guard{lock_};
        co_return co_await mirror_unlocked(check, nullptr);
    }

    coro::Task<read_response_type> mirror(
        RegisterAddressMap<Master>& map,
        MirrorCheck check = MirrorCheck::Enabled) {
        co_await lock_.acquire();
        register_detail::LockGuard guard{lock_};
        co_return co_await mirror_unlocked(check, &map);
    }

   private:
    coro::Task<write_response_type> update_unlocked(
        RegisterAddressMap<Master>* map) {
        if (!needs_update()) co_return write_response_type{};
        const auto writable = writable_mask();
        for (std::size_t bit = 0; bit < Width; ++bit) {
            if (writable.bit(bit) && !desired_valid_mask_.bit(bit)) {
                throw std::logic_error(
                    "cpptb-vc: register update has unknown desired writable "
                    "bits: " +
                    std::string{descriptor_->path} +
                    "; set the register, read it, or predict it first");
            }
        }
        const data_type write_value = desired_write_value();
        const data_type reachable = predicted_write_value(write_value);
        const data_type reachable_valid =
            predicted_write_valid_mask(write_value);
        const data_type mask = writable_mask();
        bool unreachable = false;
        for (std::size_t bit = 0; bit < Width; ++bit) {
            if (mask.bit(bit) &&
                (reachable.bit(bit) != desired_.bit(bit) ||
                 !reachable_valid.bit(bit))) {
                unreachable = true;
                break;
            }
        }
        if (unreachable) {
            test_.warn("register update cannot reach the requested desired "
                       "state: " +
                       std::string{descriptor_->path} + " requested=" +
                       cpptb::detail::format_bits(desired_) + " reachable=" +
                       cpptb::detail::format_bits(reachable));
        }
        co_return co_await write_unlocked(write_value, map);
    }

    coro::Task<read_response_type> mirror_unlocked(
        MirrorCheck check, RegisterAddressMap<Master>* map) {
        const auto previous = mirrored_;
        const auto previous_valid = mirrored_valid_mask_;
        auto response = co_await frontdoor_read(map);
        if (check == MirrorCheck::Enabled) {
            data_type actual;
            data_type expected;
            bool compared = false;
            const auto stable = nonvolatile_mask();
            for (std::size_t bit = 0; bit < Width; ++bit) {
                if (!stable.bit(bit) || !previous_valid.bit(bit) ||
                    !response.valid_mask.bit(bit)) {
                    continue;
                }
                compared = true;
                actual.set_bit(bit, response.data.bit(bit));
                expected.set_bit(bit, previous.bit(bit));
            }
            if (compared) test_.expect_eq(descriptor_->path, actual, expected);
        }
        if (auto_predict_) {
            predict_read_masked(response.data, response.valid_mask);
        }
        co_return response;
    }

   public:

    data_type peek() {
        require_backdoor();
        typename data_type::word_array words{};
        backdoor_->peek_words(*descriptor_, effective_address_, words);
        const auto value = data_type::from_words(words);
        predict(value);
        return value;
    }

    void poke(const data_type& value) {
        require_backdoor();
        backdoor_->poke_words(*descriptor_, effective_address_, value.words());
        predict(value);
    }

    template <std::size_t FieldWidth>
    WideRegisterFieldHandle<Width, FieldWidth, Master> field(
        const RegisterFieldDescriptor& descriptor) {
        return {*this, descriptor};
    }

    const RegisterDescriptor& descriptor() const noexcept {
        return *descriptor_;
    }

    [[nodiscard]] std::string_view name() const noexcept {
        return descriptor_->name;
    }

    [[nodiscard]] std::string_view path() const noexcept {
        return descriptor_->path;
    }

    [[nodiscard]] std::span<const RegisterBackdoorSliceDescriptor>
    hdl_slices() const noexcept {
        return descriptor_->backdoor_slices;
    }

    [[nodiscard]] std::optional<std::string_view> hdl_path() const noexcept {
        if (descriptor_->backdoor_slices.size() != 1) return std::nullopt;
        return descriptor_->backdoor_slices.front().path;
    }

    [[nodiscard]] bool has_backdoor() const noexcept {
        return backdoor_ != nullptr;
    }

    [[nodiscard]] uint64_t address() const noexcept {
        return effective_address_;
    }

    [[nodiscard]] uint16_t width() const noexcept {
        return descriptor_->width;
    }

    [[nodiscard]] uint64_t end_address() const noexcept {
        return effective_address_ + Width / 8u;
    }

    [[nodiscard]] uint64_t valid_byte_enable_mask() const noexcept {
        const uint16_t bytes = access_width() / 8u;
        return bytes >= 64 ? std::numeric_limits<uint64_t>::max()
                           : (uint64_t{1} << bytes) - 1u;
    }

    [[nodiscard]] bool is_transfer_address(uint64_t address) const noexcept {
        if (address < effective_address_ || address >= end_address()) {
            return false;
        }
        return (address - effective_address_) % (access_width() / 8u) == 0;
    }

    void predict_transfer_read(uint64_t address, bus_data_type value) {
        const uint32_t transfer = require_transfer_address(address);
        const std::size_t bit_offset = transfer_bit_offset(transfer);
        data_type expanded;
        data_type enabled;
        for (std::size_t bit = 0; bit < access_width(); ++bit) {
            expanded.set_bit(bit_offset + bit,
                             (static_cast<uint64_t>(value) >> bit) & 1u);
            enabled.set_bit(bit_offset + bit, true);
        }
        predict_read_masked(expanded, enabled);
    }

    void predict_transfer_write(uint64_t address, bus_data_type value,
                                byte_enable_type byte_enable) {
        const uint32_t transfer = require_transfer_address(address);
        const std::size_t bit_offset = transfer_bit_offset(transfer);
        data_type expanded;
        data_type enabled;
        for (std::size_t bit = 0; bit < access_width(); ++bit) {
            const std::size_t byte = bit / 8u;
            if (((static_cast<uint64_t>(byte_enable) >> byte) & 1u) == 0) {
                continue;
            }
            expanded.set_bit(bit_offset + bit,
                             (static_cast<uint64_t>(value) >> bit) & 1u);
            enabled.set_bit(bit_offset + bit, true);
        }
        predict_write_masked(expanded, enabled);
    }

   private:
    template <std::size_t, std::size_t, MemoryMappedMaster OtherMaster>
        requires std::unsigned_integral<typename OtherMaster::data_type>
    friend class WideRegisterFieldHandle;

    static data_type ones() {
        data_type result;
        for (std::size_t bit = 0; bit < Width; ++bit) result.set_bit(bit, true);
        return result;
    }

    static data_type descriptor_bits(std::span<const uint32_t> words,
                                     uint64_t fallback) {
        typename data_type::word_array value{};
        if (words.empty()) {
            value[0] = static_cast<uint32_t>(fallback);
            if constexpr (data_type::word_count > 1) {
                value[1] = static_cast<uint32_t>(fallback >> 32u);
            }
        } else {
            const std::size_t count =
                std::min(words.size(), data_type::word_count);
            for (std::size_t index = 0; index < count; ++index) {
                value[index] = words[index];
            }
        }
        return data_type::from_words(value);
    }

    template <typename Function>
    static void for_each_field_bit(const RegisterFieldDescriptor& field,
                                   Function&& function) {
        const std::size_t end =
            std::min<std::size_t>(Width, field.lsb + field.width);
        for (std::size_t bit = field.lsb; bit < end; ++bit) function(bit);
    }

    data_type writable_mask() const noexcept {
        if (descriptor_->fields.empty()) return ones();
        data_type result;
        for (const auto& field : descriptor_->fields) {
            if (!register_writable(field.access)) continue;
            for_each_field_bit(field,
                               [&](std::size_t bit) { result.set_bit(bit, true); });
        }
        return result;
    }

    data_type nonvolatile_mask() const noexcept {
        if (descriptor_->fields.empty()) return ones();
        data_type result;
        for (const auto& field : descriptor_->fields) {
            if (!register_readable(field.access) || field.volatile_value) continue;
            for_each_field_bit(field,
                               [&](std::size_t bit) { result.set_bit(bit, true); });
        }
        return result;
    }

    static bool write_effect_bit(bool previous, bool written,
                                 RegisterWriteEffect effect) noexcept {
        switch (effect) {
            case RegisterWriteEffect::None:
            case RegisterWriteEffect::User:
                return written;
            case RegisterWriteEffect::WriteOneSet:
                return previous || written;
            case RegisterWriteEffect::WriteOneClear:
                return previous && !written;
            case RegisterWriteEffect::WriteOneToggle:
                return previous != written;
            case RegisterWriteEffect::WriteZeroSet:
                return previous || !written;
            case RegisterWriteEffect::WriteZeroClear:
                return previous && written;
            case RegisterWriteEffect::WriteZeroToggle:
                return previous == written;
            case RegisterWriteEffect::Clear:
                return false;
            case RegisterWriteEffect::Set:
                return true;
        }
        return written;
    }

    static bool write_valid_bit(bool previous_valid, bool written,
                                RegisterWriteEffect effect) noexcept {
        switch (effect) {
            case RegisterWriteEffect::None:
            case RegisterWriteEffect::Clear:
            case RegisterWriteEffect::Set:
                return true;
            case RegisterWriteEffect::WriteOneSet:
            case RegisterWriteEffect::WriteOneClear:
                return previous_valid || written;
            case RegisterWriteEffect::WriteOneToggle:
            case RegisterWriteEffect::WriteZeroToggle:
                return previous_valid;
            case RegisterWriteEffect::WriteZeroSet:
            case RegisterWriteEffect::WriteZeroClear:
                return previous_valid || !written;
            case RegisterWriteEffect::User:
                return false;
        }
        return false;
    }

    static bool desired_write_bit(bool previous, bool previous_valid,
                                  bool desired,
                                  RegisterWriteEffect effect) noexcept {
        switch (effect) {
            case RegisterWriteEffect::None:
            case RegisterWriteEffect::User:
                return desired;
            case RegisterWriteEffect::WriteOneSet:
                return desired && (!previous || !previous_valid);
            case RegisterWriteEffect::WriteOneClear:
                return !desired && (previous || !previous_valid);
            case RegisterWriteEffect::WriteOneToggle:
                return previous_valid && previous != desired;
            case RegisterWriteEffect::WriteZeroSet:
                return (previous && previous_valid) || !desired;
            case RegisterWriteEffect::WriteZeroClear:
                return (!previous && previous_valid) || desired;
            case RegisterWriteEffect::WriteZeroToggle:
                return (previous_valid && previous == desired) ||
                       !previous_valid;
            case RegisterWriteEffect::Clear:
            case RegisterWriteEffect::Set:
                return false;
        }
        return desired;
    }

    void predict_write_masked(const data_type& value, const data_type& enabled) {
        if (descriptor_->fields.empty()) {
            for (std::size_t bit = 0; bit < Width; ++bit) {
                if (!enabled.bit(bit)) continue;
                mirrored_.set_bit(bit, value.bit(bit));
                desired_.set_bit(bit, value.bit(bit));
                mirrored_valid_mask_.set_bit(bit, true);
                desired_valid_mask_.set_bit(bit, true);
            }
            return;
        }
        for (const auto& field : descriptor_->fields) {
            if (!register_writable(field.access)) continue;
            for_each_field_bit(field, [&](std::size_t bit) {
                if (!enabled.bit(bit) || written_once_mask_.bit(bit)) return;
                bool next;
                bool valid;
                if (field.write_effect == RegisterWriteEffect::User &&
                    user_effects_) {
                    const auto result = user_effects_->predict_write(
                        RegisterUserEffectBitContext{
                            *descriptor_, field,
                            static_cast<uint16_t>(bit - field.lsb),
                            mirrored_.bit(bit),
                            mirrored_valid_mask_.bit(bit), value.bit(bit)});
                    next = result.value;
                    valid = result.valid;
                } else {
                    next = write_effect_bit(mirrored_.bit(bit),
                                            value.bit(bit),
                                            field.write_effect);
                    valid = write_valid_bit(mirrored_valid_mask_.bit(bit),
                                            value.bit(bit),
                                            field.write_effect);
                }
                mirrored_.set_bit(bit, next);
                desired_.set_bit(bit, next);
                mirrored_valid_mask_.set_bit(bit, valid);
                desired_valid_mask_.set_bit(bit, valid);
                if (field.access == RegisterAccess::WriteOnce ||
                    field.access == RegisterAccess::ReadWriteOnce) {
                    written_once_mask_.set_bit(bit, true);
                }
            });
        }
    }

    void predict_read_masked(const data_type& value, const data_type& enabled) {
        if (descriptor_->fields.empty()) {
            for (std::size_t bit = 0; bit < Width; ++bit) {
                if (!enabled.bit(bit)) continue;
                mirrored_.set_bit(bit, value.bit(bit));
                desired_.set_bit(bit, value.bit(bit));
                mirrored_valid_mask_.set_bit(bit, true);
                desired_valid_mask_.set_bit(bit, true);
            }
            return;
        }
        for (const auto& field : descriptor_->fields) {
            if (!register_readable(field.access)) continue;
            for_each_field_bit(field, [&](std::size_t bit) {
                if (!enabled.bit(bit)) return;
                bool next;
                bool valid;
                if (field.read_effect == RegisterReadEffect::User &&
                    user_effects_) {
                    const auto result = user_effects_->predict_read(
                        RegisterUserEffectBitContext{
                            *descriptor_, field,
                            static_cast<uint16_t>(bit - field.lsb),
                            mirrored_.bit(bit),
                            mirrored_valid_mask_.bit(bit), value.bit(bit)});
                    next = result.value;
                    valid = result.valid;
                } else {
                    next = value.bit(bit);
                    if (field.read_effect == RegisterReadEffect::Clear)
                        next = false;
                    if (field.read_effect == RegisterReadEffect::Set)
                        next = true;
                    valid = field.read_effect != RegisterReadEffect::User;
                }
                mirrored_.set_bit(bit, next);
                desired_.set_bit(bit, next);
                mirrored_valid_mask_.set_bit(bit, valid);
                desired_valid_mask_.set_bit(bit, valid);
            });
        }
    }

    data_type desired_write_value() const {
        if (descriptor_->fields.empty()) return desired_;
        data_type result;
        for (const auto& field : descriptor_->fields) {
            if (!register_writable(field.access)) continue;
            for_each_field_bit(field, [&](std::size_t bit) {
                if (field.write_effect == RegisterWriteEffect::User &&
                    user_effects_) {
                    result.set_bit(
                        bit, user_effects_->encode_write(
                                 RegisterUserEffectBitContext{
                                     *descriptor_, field,
                                     static_cast<uint16_t>(bit - field.lsb),
                                     mirrored_.bit(bit),
                                     mirrored_valid_mask_.bit(bit),
                                     desired_.bit(bit)}));
                } else {
                    result.set_bit(
                        bit,
                        desired_write_bit(mirrored_.bit(bit),
                                          mirrored_valid_mask_.bit(bit),
                                          desired_.bit(bit),
                                          field.write_effect));
                }
            });
        }
        return result;
    }

    data_type predicted_write_value(const data_type& written) const {
        if (descriptor_->fields.empty()) return written;
        data_type result = mirrored_;
        for (const auto& field : descriptor_->fields) {
            if (!register_writable(field.access)) continue;
            for_each_field_bit(field, [&](std::size_t bit) {
                if (written_once_mask_.bit(bit)) return;
                if (field.write_effect == RegisterWriteEffect::User &&
                    user_effects_) {
                    result.set_bit(
                        bit, user_effects_
                                 ->predict_write(RegisterUserEffectBitContext{
                                     *descriptor_, field,
                                     static_cast<uint16_t>(bit - field.lsb),
                                     mirrored_.bit(bit),
                                     mirrored_valid_mask_.bit(bit),
                                     written.bit(bit)})
                                 .value);
                } else {
                    result.set_bit(bit,
                                   write_effect_bit(mirrored_.bit(bit),
                                                    written.bit(bit),
                                                    field.write_effect));
                }
            });
        }
        return result;
    }

    data_type predicted_write_valid_mask(
        const data_type& written) const {
        if (descriptor_->fields.empty()) return ones();
        data_type result = mirrored_valid_mask_;
        for (const auto& field : descriptor_->fields) {
            if (!register_writable(field.access)) continue;
            for_each_field_bit(field, [&](std::size_t bit) {
                if (written_once_mask_.bit(bit)) return;
                if (field.write_effect == RegisterWriteEffect::User &&
                    user_effects_) {
                    result.set_bit(
                        bit, user_effects_
                                 ->predict_write(RegisterUserEffectBitContext{
                                     *descriptor_, field,
                                     static_cast<uint16_t>(bit - field.lsb),
                                     mirrored_.bit(bit),
                                     mirrored_valid_mask_.bit(bit),
                                     written.bit(bit)})
                                 .valid);
                } else {
                    result.set_bit(
                        bit, write_valid_bit(mirrored_valid_mask_.bit(bit),
                                             written.bit(bit),
                                             field.write_effect));
                }
            });
        }
        return result;
    }

    bool write_once_only() const noexcept {
        bool saw_writable = false;
        for (const auto& field : descriptor_->fields) {
            if (!register_writable(field.access)) continue;
            saw_writable = true;
            if (field.access != RegisterAccess::WriteOnce &&
                field.access != RegisterAccess::ReadWriteOnce) {
                return false;
            }
        }
        return saw_writable;
    }

    void enforce_write_access() const {
        if (!has_writable_field()) {
            throw std::logic_error("cpptb-vc: register is not writable: " +
                                   std::string{descriptor_->path});
        }
        bool was_written = false;
        for (std::size_t bit = 0; bit < Width; ++bit) {
            if (written_once_mask_.bit(bit)) {
                was_written = true;
                break;
            }
        }
        if (was_written && write_once_only()) {
            throw std::logic_error(
                "cpptb-vc: write-once register was already written: " +
                std::string{descriptor_->path});
        }
    }

    uint32_t require_transfer_address(uint64_t address) const {
        if (!is_transfer_address(address)) {
            throw std::out_of_range(
                "cpptb-vc: wide register predictor address is not a transfer "
                "of: " +
                std::string{descriptor_->path} + " address=" +
                std::to_string(address));
        }
        return static_cast<uint32_t>(
            (address - effective_address_) / (access_width() / 8u));
    }

    void require_backdoor() const {
        if (backdoor_) return;
        throw std::logic_error(
            "cpptb-vc: wide register has no backdoor: " +
            std::string{descriptor_->path});
    }

    template <std::size_t FieldWidth>
    void set_field_desired(const Bits<FieldWidth>& value,
                           const RegisterFieldDescriptor& field) {
        for (std::size_t offset = 0; offset < FieldWidth; ++offset) {
            const std::size_t bit = field.lsb + offset;
            desired_.set_bit(bit, value.bit(offset));
            desired_valid_mask_.set_bit(bit, true);
        }
    }

    coro::Task<write_response_type> write_unlocked(
        const data_type& value, RegisterAddressMap<Master>* map) {
        enforce_write_access();
        write_response_type result;
        for (uint32_t transfer = 0; transfer < transfer_count(); ++transfer) {
            const std::size_t bit_offset = transfer_bit_offset(transfer);
            uint64_t chunk = 0;
            for (std::size_t bit = 0; bit < access_width(); ++bit) {
                if (value.bit(bit_offset + bit)) chunk |= uint64_t{1} << bit;
            }
            const uint64_t address = transfer_address(transfer, map);
            const typename Master::write_request_type request{
                static_cast<address_type>(address),
                static_cast<bus_data_type>(chunk), all_bytes()};
            typename Master::write_response_type response;
            if (map) {
                response = co_await map->write(*descriptor_, request);
            } else {
                response = co_await master_->write(request);
            }
            result.transport = response;
            if (!response.okay()) {
                result.failed_address = address;
                co_return result;
            }
            ++result.transfers_completed;
            data_type enabled;
            for (std::size_t bit = 0; bit < access_width(); ++bit) {
                enabled.set_bit(bit_offset + bit, true);
            }
            if (auto_predict_) predict_write_masked(value, enabled);
        }
        co_return result;
    }

    coro::Task<read_response_type> frontdoor_read(
        RegisterAddressMap<Master>* map) {
        if (!has_readable_field()) {
            throw std::logic_error("cpptb-vc: register is not readable: " +
                                   std::string{descriptor_->path});
        }
        read_response_type result;
        for (uint32_t transfer = 0; transfer < transfer_count(); ++transfer) {
            const uint64_t address = transfer_address(transfer, map);
            const typename Master::read_request_type request{
                static_cast<address_type>(address)};
            typename Master::read_response_type response;
            if (map) {
                response = co_await map->read(*descriptor_, request);
            } else {
                response = co_await master_->read(request);
            }
            result.transport = response;
            if (!response.okay()) {
                result.failed_address = address;
                co_return result;
            }
            ++result.transfers_completed;
            const std::size_t bit_offset = transfer_bit_offset(transfer);
            const uint64_t chunk = static_cast<uint64_t>(response.data);
            for (std::size_t bit = 0; bit < access_width(); ++bit) {
                result.data.set_bit(bit_offset + bit, (chunk >> bit) & 1u);
                result.valid_mask.set_bit(bit_offset + bit, true);
            }
        }
        co_return result;
    }

    bool has_readable_field() const noexcept {
        if (descriptor_->fields.empty()) return true;
        return std::ranges::any_of(descriptor_->fields, [](const auto& field) {
            return register_readable(field.access);
        });
    }

    bool has_writable_field() const noexcept {
        if (descriptor_->fields.empty()) return true;
        return std::ranges::any_of(descriptor_->fields, [](const auto& field) {
            return register_writable(field.access);
        });
    }

    uint16_t access_width() const noexcept {
        return descriptor_->access_width == 0 ? descriptor_->width
                                              : descriptor_->access_width;
    }
    uint32_t transfer_count() const noexcept { return Width / access_width(); }
    std::size_t transfer_bit_offset(uint32_t transfer) const noexcept {
        const uint32_t logical =
            descriptor_->endianness == RegisterEndianness::Little
                ? transfer
                : transfer_count() - transfer - 1u;
        return static_cast<std::size_t>(logical) * access_width();
    }
    uint64_t transfer_address(
        uint32_t transfer, RegisterAddressMap<Master>* map = nullptr) const {
        const uint64_t base =
            map ? map->effective_address(*descriptor_) : effective_address_;
        return base +
               static_cast<uint64_t>(transfer) * (access_width() / 8u);
    }
    byte_enable_type all_bytes() const noexcept {
        return static_cast<byte_enable_type>(
            (uint64_t{1} << (access_width() / 8u)) - 1u);
    }

    void validate_access() const {
        const uint16_t transfer_width = access_width();
        if (Width % 8u != 0 || transfer_width == 0 ||
            transfer_width % 8u != 0 || Width % transfer_width != 0 ||
            transfer_width > 64 ||
            transfer_width > std::numeric_limits<bus_data_type>::digits) {
            throw std::logic_error(
                "cpptb-vc: wide register accesswidth must be byte-aligned, "
                "divide regwidth, be at most 64 bits, and fit the frontdoor "
                "data width: " +
                std::string{descriptor_->path});
        }
        if constexpr (std::integral<address_type>) {
            if (transfer_address(transfer_count() - 1u) >
                static_cast<uint64_t>(
                    std::numeric_limits<address_type>::max())) {
                throw std::logic_error(
                    "cpptb-vc: wide register transfer address exceeds the "
                    "frontdoor address width: " +
                    std::string{descriptor_->path});
            }
        }
    }

    TestContext test_;
    Master* master_;
    const RegisterDescriptor* descriptor_;
    WideRegisterBackdoor* backdoor_ = nullptr;
    RegisterUserEffectPolicy* user_effects_ = nullptr;
    bool auto_predict_ = true;
    coro::Lock lock_;
    data_type desired_{};
    data_type mirrored_{};
    data_type desired_valid_mask_{};
    data_type mirrored_valid_mask_{};
    data_type written_once_mask_{};
    uint64_t effective_address_ = 0;
};

template <typename... Elements>
class RegisterViewArray {
   public:
    static constexpr std::size_t size = sizeof...(Elements);

    explicit constexpr RegisterViewArray(Elements... elements)
        : elements_(elements...) {}

    template <std::size_t Index>
    [[nodiscard]] constexpr decltype(auto) at() {
        static_assert(Index < size, "register-model array index is out of range");
        return std::get<Index>(elements_);
    }

    template <std::size_t Index>
    [[nodiscard]] constexpr decltype(auto) at() const {
        static_assert(Index < size, "register-model array index is out of range");
        return std::get<Index>(elements_);
    }

    template <typename Function>
    constexpr void for_each(Function&& function) {
        std::apply(
            [&](auto&... element) { (function(element), ...); }, elements_);
    }

    template <typename Function>
    constexpr void for_each(Function&& function) const {
        std::apply(
            [&](const auto&... element) { (function(element), ...); },
            elements_);
    }

    template <std::size_t First, std::size_t Count, typename Function>
    constexpr void for_each_slice(Function&& function) {
        static_assert(First <= size,
                      "register-model array slice starts out of range");
        static_assert(Count <= size - First,
                      "register-model array slice extends out of range");
        for_each_slice_impl<First>(std::forward<Function>(function),
                                   std::make_index_sequence<Count>{});
    }

    template <std::size_t First, std::size_t Count, typename Function>
    constexpr void for_each_slice(Function&& function) const {
        static_assert(First <= size,
                      "register-model array slice starts out of range");
        static_assert(Count <= size - First,
                      "register-model array slice extends out of range");
        for_each_slice_impl<First>(std::forward<Function>(function),
                                   std::make_index_sequence<Count>{});
    }

   private:
    template <std::size_t First, typename Function, std::size_t... Offset>
    constexpr void for_each_slice_impl(Function&& function,
                                       std::index_sequence<Offset...>) {
        (function(std::get<First + Offset>(elements_)), ...);
    }

    template <std::size_t First, typename Function, std::size_t... Offset>
    constexpr void for_each_slice_impl(
        Function&& function, std::index_sequence<Offset...>) const {
        (function(std::get<First + Offset>(elements_)), ...);
    }

    std::tuple<Elements...> elements_;
};

template <typename... Elements>
RegisterViewArray(Elements&...) -> RegisterViewArray<Elements&...>;

template <MemoryMappedMaster Master>
    requires std::unsigned_integral<typename Master::data_type>
class RegisterPredictor {
   public:
    using handle_type = RegisterHandle<Master>;

    RegisterPredictor(TestContext test,
                      std::span<handle_type* const> registers)
        : test_(std::move(test)) {
        entries_.reserve(registers.size());
        for (handle_type* handle : registers) {
            if (!handle) {
                throw std::invalid_argument(
                    "cpptb-vc: register predictor received a null handle");
            }
            entries_.push_back(
                Entry{handle->address(), handle->end_address(),
                      static_cast<uint16_t>(
                          (handle->descriptor().access_width == 0
                               ? handle->descriptor().width
                               : handle->descriptor().access_width) /
                          8u),
                      handle});
        }
        sort_and_validate();
    }

    RegisterPredictor& add_alias(handle_type& handle,
                                 RegisterAddressMap<Master>& map) {
        const uint64_t address = map.effective_address(handle.descriptor());
        const uint64_t bytes = handle.descriptor().width / 8u;
        if (bytes > std::numeric_limits<uint64_t>::max() - address) {
            throw std::overflow_error(
                "cpptb-vc: register predictor alias range overflows for " +
                std::string{handle.path()} + " in map " +
                std::string{map.name()});
        }
        auto previous_entries = entries_;
        entries_.push_back(Entry{
            address, address + bytes,
            static_cast<uint16_t>(
                (handle.descriptor().access_width == 0
                     ? handle.descriptor().width
                     : handle.descriptor().access_width) /
                8u),
            &handle});
        try {
            sort_and_validate();
        } catch (...) {
            entries_ = std::move(previous_entries);
            throw;
        }
        return *this;
    }

   private:
    void sort_and_validate() {
        std::sort(entries_.begin(), entries_.end(),
                  [](const Entry& left, const Entry& right) {
                      return left.address < right.address;
                  });
        for (std::size_t index = 1; index < entries_.size(); ++index) {
            if (entries_[index - 1].end_address > entries_[index].address) {
                throw std::invalid_argument(
                    "cpptb-vc: register predictor has duplicate address or overlapping range at " +
                    std::to_string(entries_[index].address));
            }
        }
    }

   public:

    template <std::integral Address, std::unsigned_integral Data,
              std::unsigned_integral ByteEnable>
    void write(const TransactionObservation<
               MemoryTransaction<Address, Data, ByteEnable>>& observation) {
        if (observation.disposition == TransactionDisposition::Completed) {
            write(observation.value);
        }
    }

    template <std::integral Address, std::unsigned_integral Data,
              std::unsigned_integral ByteEnable>
    void write(const MemoryTransaction<Address, Data, ByteEnable>& transaction) {
        if (!transaction_okay(transaction.status)) return;
        const auto address = normalized_address(transaction.address);
        if (!address.has_value()) {
            ++unmapped_;
            return;
        }
        Entry* entry = find(*address);
        if (!entry) {
            ++unmapped_;
            return;
        }
        handle_type* handle = entry->handle;
        const uint64_t handle_address =
            handle->address() + (*address - entry->address);
        if (transaction.operation == MemoryOperation::Read) {
            handle->predict_transfer_read(
                handle_address,
                static_cast<typename Master::data_type>(transaction.data));
            ++reads_;
            return;
        }

        const uint64_t byte_enable =
            static_cast<uint64_t>(transaction.byte_enable);
        const uint64_t allowed = handle->valid_byte_enable_mask();
        if ((byte_enable & ~allowed) != 0) {
            ++invalid_byte_enables_;
            test_.warn("register predictor ignored byte-enable bits outside "
                       "the register width: " +
                       std::string{handle->descriptor().path} + " mask=" +
                       std::to_string(byte_enable & ~allowed));
        }
        handle->predict_transfer_write(
            handle_address,
            static_cast<typename Master::data_type>(transaction.data),
            static_cast<typename Master::byte_enable_type>(byte_enable &
                                                            allowed));
        ++writes_;
    }

    uint64_t reads() const noexcept { return reads_; }
    uint64_t writes() const noexcept { return writes_; }
    uint64_t failed() const noexcept { return failed_; }
    uint64_t unmapped() const noexcept { return unmapped_; }
    uint64_t invalid_byte_enables() const noexcept {
        return invalid_byte_enables_;
    }

   private:
    struct Entry {
        uint64_t address;
        uint64_t end_address;
        uint16_t transfer_bytes;
        handle_type* handle;
    };

    bool transaction_okay(MemoryStatus status) noexcept {
        if (memory_okay(status)) return true;
        ++failed_;
        return false;
    }

    template <std::integral Address>
    static std::optional<uint64_t> normalized_address(Address address) {
        if constexpr (std::signed_integral<Address>) {
            if (address < 0) return std::nullopt;
        }
        return static_cast<uint64_t>(address);
    }

    Entry* find(uint64_t address) noexcept {
        const auto found = std::lower_bound(
            entries_.begin(), entries_.end(), address,
            [](const Entry& entry, uint64_t target) {
                return entry.address < target;
            });
        if (found != entries_.end() && found->address == address) {
            return &*found;
        }
        if (found == entries_.begin()) return nullptr;
        auto& previous = *std::prev(found);
        return address < previous.end_address && previous.transfer_bytes != 0 &&
                       (address - previous.address) % previous.transfer_bytes == 0
                   ? &previous
                   : nullptr;
    }

    TestContext test_;
    std::vector<Entry> entries_;
    uint64_t reads_ = 0;
    uint64_t writes_ = 0;
    uint64_t failed_ = 0;
    uint64_t unmapped_ = 0;
    uint64_t invalid_byte_enables_ = 0;
};

template <MemoryMappedMaster Master>
RegisterPredictor(TestContext, std::span<RegisterHandle<Master>* const>)
    -> RegisterPredictor<Master>;

template <std::size_t Width, MemoryMappedMaster Master>
    requires std::unsigned_integral<typename Master::data_type>
class WideRegisterPredictor {
   public:
    using handle_type = WideRegisterHandle<Width, Master>;

    WideRegisterPredictor(TestContext test,
                          std::span<handle_type* const> registers)
        : test_(std::move(test)) {
        entries_.reserve(registers.size());
        for (handle_type* handle : registers) {
            if (!handle) {
                throw std::invalid_argument(
                    "cpptb-vc: wide register predictor received a null handle");
            }
            entries_.push_back(
                Entry{handle->address(), handle->end_address(),
                      static_cast<uint16_t>(
                          (handle->descriptor().access_width == 0
                               ? handle->descriptor().width
                               : handle->descriptor().access_width) /
                          8u),
                      handle});
        }
        sort_and_validate();
    }

    WideRegisterPredictor& add_alias(handle_type& handle,
                                     RegisterAddressMap<Master>& map) {
        const uint64_t address = map.effective_address(handle.descriptor());
        const uint64_t bytes = Width / 8u;
        if (bytes > std::numeric_limits<uint64_t>::max() - address) {
            throw std::overflow_error(
                "cpptb-vc: wide register predictor alias range overflows for " +
                std::string{handle.path()} + " in map " +
                std::string{map.name()});
        }
        auto previous_entries = entries_;
        entries_.push_back(Entry{
            address, address + bytes,
            static_cast<uint16_t>(
                (handle.descriptor().access_width == 0
                     ? handle.descriptor().width
                     : handle.descriptor().access_width) /
                8u),
            &handle});
        try {
            sort_and_validate();
        } catch (...) {
            entries_ = std::move(previous_entries);
            throw;
        }
        return *this;
    }

   private:
    void sort_and_validate() {
        std::sort(entries_.begin(), entries_.end(),
                  [](const Entry& left, const Entry& right) {
                      return left.address < right.address;
                  });
        for (std::size_t index = 1; index < entries_.size(); ++index) {
            if (entries_[index - 1].end_address > entries_[index].address) {
                throw std::invalid_argument(
                    "cpptb-vc: wide register predictor has duplicate address or overlapping range at " +
                    std::to_string(entries_[index].address));
            }
        }
    }

   public:

    template <std::integral Address, std::unsigned_integral Data,
              std::unsigned_integral ByteEnable>
    void write(const TransactionObservation<
               MemoryTransaction<Address, Data, ByteEnable>>& observation) {
        if (observation.disposition == TransactionDisposition::Completed) {
            write(observation.value);
        }
    }

    template <std::integral Address, std::unsigned_integral Data,
              std::unsigned_integral ByteEnable>
    void write(const MemoryTransaction<Address, Data, ByteEnable>& transaction) {
        if (!transaction_okay(transaction.status)) return;
        const auto address = normalized_address(transaction.address);
        if (!address.has_value()) {
            ++unmapped_;
            return;
        }
        Entry* entry = find(*address);
        if (!entry) {
            ++unmapped_;
            return;
        }
        handle_type* handle = entry->handle;
        const uint64_t handle_address =
            handle->address() + (*address - entry->address);
        if (transaction.operation == MemoryOperation::Read) {
            handle->predict_transfer_read(
                handle_address,
                static_cast<typename Master::data_type>(transaction.data));
            ++reads_;
            return;
        }

        const uint64_t byte_enable =
            static_cast<uint64_t>(transaction.byte_enable);
        const uint64_t allowed = handle->valid_byte_enable_mask();
        if ((byte_enable & ~allowed) != 0) {
            ++invalid_byte_enables_;
            test_.warn("wide register predictor ignored byte-enable bits "
                       "outside the transfer width: " +
                       std::string{handle->descriptor().path} + " mask=" +
                       std::to_string(byte_enable & ~allowed));
        }
        handle->predict_transfer_write(
            handle_address,
            static_cast<typename Master::data_type>(transaction.data),
            static_cast<typename Master::byte_enable_type>(byte_enable &
                                                            allowed));
        ++writes_;
    }

    [[nodiscard]] uint64_t reads() const noexcept { return reads_; }
    [[nodiscard]] uint64_t writes() const noexcept { return writes_; }
    [[nodiscard]] uint64_t failed() const noexcept { return failed_; }
    [[nodiscard]] uint64_t unmapped() const noexcept { return unmapped_; }
    [[nodiscard]] uint64_t invalid_byte_enables() const noexcept {
        return invalid_byte_enables_;
    }

   private:
    struct Entry {
        uint64_t address;
        uint64_t end_address;
        uint16_t transfer_bytes;
        handle_type* handle;
    };

    bool transaction_okay(MemoryStatus status) noexcept {
        if (memory_okay(status)) return true;
        ++failed_;
        return false;
    }

    template <std::integral Address>
    static std::optional<uint64_t> normalized_address(Address address) {
        if constexpr (std::signed_integral<Address>) {
            if (address < 0) return std::nullopt;
        }
        return static_cast<uint64_t>(address);
    }

    Entry* find(uint64_t address) noexcept {
        const auto found = std::lower_bound(
            entries_.begin(), entries_.end(), address,
            [](const Entry& entry, uint64_t target) {
                return entry.address < target;
            });
        if (found != entries_.end() && found->address == address) {
            return &*found;
        }
        if (found == entries_.begin()) return nullptr;
        auto& previous = *std::prev(found);
        return address < previous.end_address && previous.transfer_bytes != 0 &&
                       (address - previous.address) % previous.transfer_bytes == 0
                   ? &previous
                   : nullptr;
    }

    TestContext test_;
    std::vector<Entry> entries_;
    uint64_t reads_ = 0;
    uint64_t writes_ = 0;
    uint64_t failed_ = 0;
    uint64_t unmapped_ = 0;
    uint64_t invalid_byte_enables_ = 0;
};

template <std::size_t Width, MemoryMappedMaster Master>
WideRegisterPredictor(
    TestContext,
    std::span<WideRegisterHandle<Width, Master>* const>)
    -> WideRegisterPredictor<Width, Master>;

}  // namespace cpptb::vc
