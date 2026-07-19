#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "cpptb_vc/register_model.hpp"

namespace {

bool expect(const char* label, bool condition) {
    if (condition) return true;
    std::fprintf(stderr, "FAILED: %s\n", label);
    return false;
}

struct FakeMaster {
    using address_type = uint32_t;
    using data_type = uint32_t;
    using byte_enable_type = uint8_t;
    using write_request_type =
        cpptb::vc::MemoryWriteRequest<address_type, data_type,
                                      byte_enable_type>;
    using read_request_type = cpptb::vc::MemoryReadRequest<address_type>;
    using write_response_type = cpptb::vc::MemoryWriteResponse;
    using read_response_type = cpptb::vc::MemoryReadResponse<data_type>;

    cpptb::coro::Task<write_response_type> write(write_request_type request) {
        writes.push_back(request);
        if (!write_responses.empty()) {
            const auto response = write_responses.front();
            write_responses.erase(write_responses.begin());
            co_return response;
        }
        co_return next_write;
    }

    cpptb::coro::Task<read_response_type> read(read_request_type request) {
        reads.push_back(request);
        if (!read_responses.empty()) {
            const auto response = read_responses.front();
            read_responses.erase(read_responses.begin());
            co_return response;
        }
        co_return next_read;
    }

    write_response_type next_write{};
    read_response_type next_read{};
    std::vector<write_request_type> writes;
    std::vector<read_request_type> reads;
    std::vector<write_response_type> write_responses;
    std::vector<read_response_type> read_responses;
};

static_assert(cpptb::vc::MemoryMappedMaster<FakeMaster>);

class FakeBackdoor : public cpptb::vc::RegisterBackdoor<uint64_t> {
   public:
    uint64_t peek(const cpptb::vc::RegisterDescriptor& descriptor,
                  uint64_t effective_address) override {
        last_path = descriptor.path;
        last_address = effective_address;
        return value;
    }

    void poke(const cpptb::vc::RegisterDescriptor& descriptor,
              uint64_t effective_address, uint64_t next) override {
        last_path = descriptor.path;
        last_address = effective_address;
        value = next;
    }

    uint64_t value = 0;
    std::string_view last_path;
    uint64_t last_address = 0;
};

class FakeWideBackdoor : public cpptb::vc::WideRegisterBackdoor {
   public:
    void peek_words(const cpptb::vc::RegisterDescriptor& descriptor,
                    uint64_t effective_address,
                    std::span<uint32_t> destination) override {
        last_path = descriptor.path;
        last_address = effective_address;
        std::copy_n(words.begin(), destination.size(), destination.begin());
    }

    void poke_words(const cpptb::vc::RegisterDescriptor& descriptor,
                    uint64_t effective_address,
                    std::span<const uint32_t> source) override {
        last_path = descriptor.path;
        last_address = effective_address;
        std::copy(source.begin(), source.end(), words.begin());
    }

    std::array<uint32_t, 8> words{};
    std::string_view last_path;
    uint64_t last_address = 0;
};

class FakeRegisterFrontdoor
    : public cpptb::vc::RegisterFrontdoor<FakeMaster> {
   public:
    cpptb::coro::Task<write_response_type> write(
        FakeMaster&, const cpptb::vc::RegisterDescriptor& descriptor,
        write_request_type request) override {
        ++writes;
        last_path = descriptor.path;
        last_address = request.address;
        storage = request.data;
        co_return write_response_type{};
    }

    cpptb::coro::Task<read_response_type> read(
        FakeMaster&, const cpptb::vc::RegisterDescriptor& descriptor,
        read_request_type request) override {
        ++reads;
        last_path = descriptor.path;
        last_address = request.address;
        co_return read_response_type{.data = storage};
    }

    uint32_t storage = 0;
    uint32_t reads = 0;
    uint32_t writes = 0;
    uint64_t last_address = 0;
    std::string_view last_path;
};

class FakeRegisterMemoryFrontdoor
    : public cpptb::vc::RegisterMemoryFrontdoor<FakeMaster> {
   public:
    cpptb::coro::Task<write_response_type> write(
        FakeMaster&, const cpptb::vc::RegisterMemoryDescriptor& descriptor,
        uint64_t index, write_request_type request) override {
        ++writes;
        last_path = descriptor.path;
        last_index = index;
        last_address = request.address;
        storage.at(index) = request.data;
        co_return write_response_type{};
    }

    cpptb::coro::Task<read_response_type> read(
        FakeMaster&, const cpptb::vc::RegisterMemoryDescriptor& descriptor,
        uint64_t index, read_request_type request) override {
        ++reads;
        last_path = descriptor.path;
        last_index = index;
        last_address = request.address;
        co_return read_response_type{.data = storage.at(index)};
    }

    std::array<uint32_t, 16> storage{};
    uint32_t reads = 0;
    uint32_t writes = 0;
    uint64_t last_index = 0;
    uint64_t last_address = 0;
    std::string_view last_path;
};

class FakeUserEffectPolicy : public cpptb::vc::RegisterUserEffectPolicy {
   public:
    bool encode_write(
        const cpptb::vc::RegisterUserEffectBitContext& context) override {
        ++encodes;
        remember(context);
        return context.previous_valid ? context.previous != context.value
                                      : context.value;
    }

    cpptb::vc::RegisterUserEffectBitResult predict_write(
        const cpptb::vc::RegisterUserEffectBitContext& context) override {
        ++write_predictions;
        remember(context);
        return {
            .value = context.previous != context.value,
            .valid = context.previous_valid,
        };
    }

    cpptb::vc::RegisterUserEffectBitResult predict_read(
        const cpptb::vc::RegisterUserEffectBitContext& context) override {
        ++read_predictions;
        remember(context);
        return {.value = !context.value, .valid = true};
    }

    void remember(const cpptb::vc::RegisterUserEffectBitContext& context) {
        last_register_path = context.register_descriptor.path;
        last_field_path = context.field_descriptor.path;
        last_field_bit = context.field_bit;
    }

    uint64_t encodes = 0;
    uint64_t write_predictions = 0;
    uint64_t read_predictions = 0;
    uint16_t last_field_bit = 0;
    std::string_view last_register_path;
    std::string_view last_field_path;
};

class FakeFieldUserEffectPolicy final : public FakeUserEffectPolicy {
   public:
    uint64_t encode_write_field(
        const cpptb::vc::RegisterUserEffectFieldContext& context) override {
        ++field_encodes;
        remember(context);
        return context.previous ^ context.value;
    }

    cpptb::vc::RegisterUserEffectFieldResult predict_write_field(
        const cpptb::vc::RegisterUserEffectFieldContext& context) override {
        ++field_write_predictions;
        remember(context);
        return {.value = context.previous ^ context.value,
                .valid_mask = context.previous_valid_mask};
    }

    cpptb::vc::RegisterUserEffectFieldResult predict_read_field(
        const cpptb::vc::RegisterUserEffectFieldContext& context) override {
        ++field_read_predictions;
        remember(context);
        return {.value = ~context.value,
                .valid_mask = cpptb::vc::register_mask(
                    context.field_descriptor.width)};
    }

    void remember(const cpptb::vc::RegisterUserEffectFieldContext& context) {
        last_register_path = context.register_descriptor.path;
        last_field_path = context.field_descriptor.path;
        last_previous = context.previous;
        last_value = context.value;
    }

    uint64_t field_encodes = 0;
    uint64_t field_write_predictions = 0;
    uint64_t field_read_predictions = 0;
    uint64_t last_previous = 0;
    uint64_t last_value = 0;
};

class FakeMemoryBackdoor
    : public cpptb::vc::RegisterMemoryBackdoor<uint32_t> {
   public:
    uint32_t peek(const cpptb::vc::RegisterMemoryDescriptor& descriptor,
                  uint64_t index, uint64_t effective_address) override {
        ++scalar_peeks;
        remember(descriptor, index, effective_address);
        return values.at(static_cast<std::size_t>(index));
    }

    void poke(const cpptb::vc::RegisterMemoryDescriptor& descriptor,
              uint64_t index, uint64_t effective_address,
              uint32_t value) override {
        ++scalar_pokes;
        remember(descriptor, index, effective_address);
        values.at(static_cast<std::size_t>(index)) = value;
    }

    void peek_into(const cpptb::vc::RegisterMemoryDescriptor& descriptor,
                   uint64_t first_index, uint64_t first_effective_address,
                   std::span<uint32_t> destination) override {
        ++bulk_peeks;
        remember(descriptor, first_index, first_effective_address);
        std::copy_n(values.begin() + static_cast<std::ptrdiff_t>(first_index),
                    destination.size(), destination.begin());
    }

    void poke(const cpptb::vc::RegisterMemoryDescriptor& descriptor,
              uint64_t first_index, uint64_t first_effective_address,
              std::span<const uint32_t> source) override {
        ++bulk_pokes;
        remember(descriptor, first_index, first_effective_address);
        std::copy(source.begin(), source.end(),
                  values.begin() + static_cast<std::ptrdiff_t>(first_index));
    }

    void remember(const cpptb::vc::RegisterMemoryDescriptor& descriptor,
                  uint64_t index, uint64_t effective_address) {
        last_path = descriptor.path;
        last_index = index;
        last_address = effective_address;
    }

    std::array<uint32_t, 32> values{};
    std::string_view last_path;
    uint64_t last_index = 0;
    uint64_t last_address = 0;
    uint32_t scalar_peeks = 0;
    uint32_t scalar_pokes = 0;
    uint32_t bulk_peeks = 0;
    uint32_t bulk_pokes = 0;
};

class FakeWideMemoryBackdoor : public cpptb::vc::WideRegisterMemoryBackdoor {
   public:
    void peek_words(const cpptb::vc::RegisterMemoryDescriptor& descriptor,
                    uint64_t index, uint64_t effective_address,
                    std::span<uint32_t> destination) override {
        ++peeks;
        remember(descriptor, index, effective_address);
        std::copy_n(words.begin() + static_cast<std::ptrdiff_t>(
                                        index * kWordsPerElement),
                    destination.size(), destination.begin());
    }

    void poke_words(const cpptb::vc::RegisterMemoryDescriptor& descriptor,
                    uint64_t index, uint64_t effective_address,
                    std::span<const uint32_t> source) override {
        ++pokes;
        remember(descriptor, index, effective_address);
        std::copy(source.begin(), source.end(),
                  words.begin() + static_cast<std::ptrdiff_t>(
                                      index * kWordsPerElement));
    }

    void remember(const cpptb::vc::RegisterMemoryDescriptor& descriptor,
                  uint64_t index, uint64_t effective_address) {
        last_path = descriptor.path;
        last_index = index;
        last_address = effective_address;
    }

    static constexpr std::size_t kWordsPerElement = 4;
    std::array<uint32_t, 16> words{};
    std::string_view last_path;
    uint64_t last_index = 0;
    uint64_t last_address = 0;
    uint32_t peeks = 0;
    uint32_t pokes = 0;
};

constexpr std::array kFields{
    cpptb::vc::RegisterFieldDescriptor{
        .name = "control",
        .path = "block.command.control",
        .lsb = 0,
        .width = 8,
        .access = cpptb::vc::RegisterAccess::ReadWrite,
        .reset_value = 0x12,
        .reset_mask = 0xff,
    },
    cpptb::vc::RegisterFieldDescriptor{
        .name = "pending",
        .path = "block.command.pending",
        .lsb = 8,
        .width = 8,
        .access = cpptb::vc::RegisterAccess::ReadWrite,
        .write_effect = cpptb::vc::RegisterWriteEffect::WriteOneClear,
        .reset_value = 0xff,
        .reset_mask = 0xff,
    },
    cpptb::vc::RegisterFieldDescriptor{
        .name = "status",
        .path = "block.command.status",
        .lsb = 16,
        .width = 8,
        .access = cpptb::vc::RegisterAccess::ReadOnly,
        .read_effect = cpptb::vc::RegisterReadEffect::Clear,
        .reset_value = 0xaa,
        .reset_mask = 0xff,
        .volatile_value = true,
    },
};

constexpr cpptb::vc::RegisterDescriptor kCommand{
    .name = "command",
    .path = "block.command",
    .address = 0x20,
    .width = 32,
    .access_width = 32,
    .reset_value = 0x00aa'ff12,
    .reset_mask = 0x00ff'ffff,
    .fields = kFields,
};

constexpr std::array kOnceFields{
    cpptb::vc::RegisterFieldDescriptor{
        .name = "key",
        .path = "block.key.key",
        .lsb = 0,
        .width = 32,
        .access = cpptb::vc::RegisterAccess::WriteOnce,
    },
};

constexpr cpptb::vc::RegisterDescriptor kWriteOnce{
    .name = "key",
    .path = "block.key",
    .address = 0x24,
    .width = 32,
    .access_width = 32,
    .fields = kOnceFields,
};

constexpr cpptb::vc::RegisterMemoryDescriptor kBuffer{
    .name = "buffer",
    .path = "block.buffer",
    .address = 0x100,
    .entries = 16,
    .width = 32,
    .access_width = 32,
    .access = cpptb::vc::RegisterAccess::ReadWrite,
    .hdl_path = "u_block.buffer_storage",
};

constexpr cpptb::vc::RegisterMemoryDescriptor kWideBuffer{
    .name = "wide_buffer",
    .path = "block.wide_buffer",
    .address = 0x300,
    .entries = 4,
    .width = 128,
    .access_width = 32,
    .access = cpptb::vc::RegisterAccess::ReadWrite,
    .hdl_path = "u_block.wide_buffer_storage",
};

constexpr cpptb::vc::RegisterDescriptor kFieldless{
    .name = "fieldless",
    .path = "block.fieldless",
    .address = 0x28,
    .width = 8,
    .access_width = 8,
};

constexpr std::array kUserEffectFields{
    cpptb::vc::RegisterFieldDescriptor{
        .name = "custom",
        .path = "block.user_effect.custom",
        .lsb = 0,
        .width = 4,
        .access = cpptb::vc::RegisterAccess::ReadWrite,
        .read_effect = cpptb::vc::RegisterReadEffect::User,
        .write_effect = cpptb::vc::RegisterWriteEffect::User,
    },
};

constexpr cpptb::vc::RegisterDescriptor kUserEffectRegister{
    .name = "user_effect",
    .path = "block.user_effect",
    .address = 0x2c,
    .width = 8,
    .access_width = 8,
    .fields = kUserEffectFields,
};

constexpr std::array kWideUserEffectFields{
    cpptb::vc::RegisterFieldDescriptor{
        .name = "custom",
        .path = "block.wide_user_effect.custom",
        .lsb = 0,
        .width = 128,
        .access = cpptb::vc::RegisterAccess::ReadWrite,
        .read_effect = cpptb::vc::RegisterReadEffect::User,
        .write_effect = cpptb::vc::RegisterWriteEffect::User,
    },
};

constexpr cpptb::vc::RegisterDescriptor kWideUserEffectRegister{
    .name = "wide_user_effect",
    .path = "block.wide_user_effect",
    .address = 0x400,
    .width = 128,
    .access_width = 32,
    .fields = kWideUserEffectFields,
};

constexpr std::array kPartialResetFields{
    cpptb::vc::RegisterFieldDescriptor{
        .name = "known",
        .path = "block.partial.known",
        .lsb = 0,
        .width = 8,
        .access = cpptb::vc::RegisterAccess::ReadWrite,
        .reset_value = 0x5a,
        .reset_mask = 0xff,
    },
    cpptb::vc::RegisterFieldDescriptor{
        .name = "unknown",
        .path = "block.partial.unknown",
        .lsb = 8,
        .width = 8,
        .access = cpptb::vc::RegisterAccess::ReadWrite,
    },
};

constexpr cpptb::vc::RegisterDescriptor kPartialReset{
    .name = "partial",
    .path = "block.partial",
    .address = 0x2a,
    .width = 16,
    .access_width = 16,
    .reset_value = 0x005a,
    .reset_mask = 0x00ff,
    .fields = kPartialResetFields,
};

constexpr cpptb::vc::RegisterDescriptor kUnknownFieldless{
    .name = "unknown_fieldless",
    .path = "block.unknown_fieldless",
    .address = 0x2b,
    .width = 8,
    .access_width = 8,
};

constexpr std::array kUnknownPairFields{
    cpptb::vc::RegisterFieldDescriptor{
        .name = "low",
        .path = "block.unknown_pair.low",
        .lsb = 0,
        .width = 8,
        .access = cpptb::vc::RegisterAccess::ReadWrite,
    },
    cpptb::vc::RegisterFieldDescriptor{
        .name = "high",
        .path = "block.unknown_pair.high",
        .lsb = 8,
        .width = 8,
        .access = cpptb::vc::RegisterAccess::ReadWrite,
    },
};

constexpr cpptb::vc::RegisterDescriptor kUnknownPair{
    .name = "unknown_pair",
    .path = "block.unknown_pair",
    .address = 0x2c,
    .width = 16,
    .access_width = 16,
    .fields = kUnknownPairFields,
};

constexpr std::array kMixedFields{
    cpptb::vc::RegisterFieldDescriptor{
        .name = "command",
        .path = "block.mixed.command",
        .lsb = 0,
        .width = 8,
        .access = cpptb::vc::RegisterAccess::WriteOnly,
        .reset_value = 0x5a,
        .reset_mask = 0xff,
    },
    cpptb::vc::RegisterFieldDescriptor{
        .name = "status",
        .path = "block.mixed.status",
        .lsb = 8,
        .width = 8,
        .access = cpptb::vc::RegisterAccess::ReadOnly,
        .reset_value = 0x12,
        .reset_mask = 0xff,
    },
    cpptb::vc::RegisterFieldDescriptor{
        .name = "control",
        .path = "block.mixed.control",
        .lsb = 16,
        .width = 8,
        .access = cpptb::vc::RegisterAccess::ReadWrite,
        .reset_value = 0x34,
        .reset_mask = 0xff,
    },
};

constexpr cpptb::vc::RegisterDescriptor kMixed{
    .name = "mixed",
    .path = "block.mixed",
    .address = 0x2c,
    .width = 32,
    .access_width = 32,
    .reset_value = 0x0034'125a,
    .reset_mask = 0x00ff'ffff,
    .fields = kMixedFields,
};

constexpr std::array kSetOnlyFields{
    cpptb::vc::RegisterFieldDescriptor{
        .name = "sticky",
        .path = "block.set_only.sticky",
        .lsb = 0,
        .width = 4,
        .access = cpptb::vc::RegisterAccess::ReadWrite,
        .write_effect = cpptb::vc::RegisterWriteEffect::WriteOneSet,
        .reset_value = 1,
        .reset_mask = 0xf,
    },
};

constexpr cpptb::vc::RegisterDescriptor kSetOnly{
    .name = "set_only",
    .path = "block.set_only",
    .address = 0x30,
    .width = 32,
    .access_width = 32,
    .reset_value = 1,
    .reset_mask = 0xf,
    .fields = kSetOnlyFields,
};

constexpr cpptb::vc::RegisterDescriptor kSplitRegister{
    .name = "split",
    .path = "block.split",
    .address = 0x34,
    .width = 32,
    .access_width = 16,
};

constexpr std::array kSplitWriteOnceFields{
    cpptb::vc::RegisterFieldDescriptor{
        .name = "value",
        .path = "block.split_write_once.value",
        .lsb = 0,
        .width = 32,
        .access = cpptb::vc::RegisterAccess::WriteOnce,
    },
};

constexpr cpptb::vc::RegisterDescriptor kSplitWriteOnceRegister{
    .name = "split_write_once",
    .path = "block.split_write_once",
    .address = 0x48,
    .width = 32,
    .access_width = 16,
    .fields = kSplitWriteOnceFields,
};

constexpr cpptb::vc::RegisterDescriptor kWideRegister{
    .name = "wide",
    .path = "block.wide",
    .address = 0x38,
    .width = 64,
    .access_width = 64,
};

constexpr cpptb::vc::RegisterDescriptor kBigEndianSplitRegister{
    .name = "big_split",
    .path = "block.big_split",
    .address = 0x3c,
    .width = 32,
    .access_width = 16,
    .endianness = cpptb::vc::RegisterEndianness::Big,
};

constexpr std::array<uint32_t, 4> kWide128ResetWords{
    0x89ab'cdef, 0x0123'4567, 0x7654'3210, 0xfedc'ba98};
constexpr std::array<uint32_t, 4> kWide128ResetMaskWords{
    0xffff'ffff, 0xffff'ffff, 0xffff'ffff, 0xffff'ffff};
constexpr std::array kWide128Fields{
    cpptb::vc::RegisterFieldDescriptor{
        .name = "value",
        .path = "block.wide128.value",
        .lsb = 0,
        .width = 128,
        .access = cpptb::vc::RegisterAccess::ReadWrite,
    },
};
constexpr cpptb::vc::RegisterDescriptor kWide128Register{
    .name = "wide128",
    .path = "block.wide128",
    .address = 0x80,
    .width = 128,
    .access_width = 32,
    .reset_value_words = kWide128ResetWords,
    .reset_mask_words = kWide128ResetMaskWords,
    .fields = kWide128Fields,
};

constexpr std::array kWide128WriteOnceFields{
    cpptb::vc::RegisterFieldDescriptor{
        .name = "value",
        .path = "block.wide128_write_once.value",
        .lsb = 0,
        .width = 128,
        .access = cpptb::vc::RegisterAccess::WriteOnce,
    },
};
constexpr cpptb::vc::RegisterDescriptor kWide128WriteOnceRegister{
    .name = "wide128_write_once",
    .path = "block.wide128_write_once",
    .address = 0xa0,
    .width = 128,
    .access_width = 32,
    .fields = kWide128WriteOnceFields,
};

constexpr std::array<uint32_t, 4> kWide128SetResetWords{1, 0, 0, 0};
constexpr std::array<uint32_t, 4> kWide128SetResetMaskWords{
    0xffff'ffff, 0xffff'ffff, 0xffff'ffff, 0xffff'ffff};
constexpr std::array kWide128SetFields{
    cpptb::vc::RegisterFieldDescriptor{
        .name = "value",
        .path = "block.wide128_set.value",
        .lsb = 0,
        .width = 128,
        .access = cpptb::vc::RegisterAccess::ReadWrite,
        .write_effect = cpptb::vc::RegisterWriteEffect::WriteOneSet,
    },
};
constexpr cpptb::vc::RegisterDescriptor kWide128SetRegister{
    .name = "wide128_set",
    .path = "block.wide128_set",
    .address = 0xc0,
    .width = 128,
    .access_width = 32,
    .reset_value_words = kWide128SetResetWords,
    .reset_mask_words = kWide128SetResetMaskWords,
    .fields = kWide128SetFields,
};

constexpr cpptb::vc::RegisterMemoryDescriptor kReadOnlyBuffer{
    .name = "read_only_buffer",
    .path = "block.read_only_buffer",
    .address = 0x200,
    .entries = 2,
    .width = 32,
    .access_width = 32,
    .access = cpptb::vc::RegisterAccess::ReadOnly,
};

constexpr cpptb::vc::RegisterMemoryDescriptor kWriteOnlyBuffer{
    .name = "write_only_buffer",
    .path = "block.write_only_buffer",
    .address = 0x220,
    .entries = 2,
    .width = 32,
    .access_width = 32,
    .access = cpptb::vc::RegisterAccess::WriteOnly,
};

constexpr cpptb::vc::RegisterMemoryDescriptor kSplitBuffer{
    .name = "split_buffer",
    .path = "block.split_buffer",
    .address = 0x240,
    .entries = 2,
    .width = 32,
    .access_width = 16,
    .access = cpptb::vc::RegisterAccess::ReadWrite,
};

cpptb::coro::Task<void> exercise_register(
    cpptb::vc::RegisterHandle<FakeMaster>& reg, FakeMaster& master,
    bool& passed) {
    using namespace cpptb::vc;
    passed &= expect("reset initializes desired and mirrored state",
                     reg.desired() == 0x00aa'ff12 &&
                         reg.mirrored() == 0x00aa'ff12 &&
                         !reg.needs_update());

    reg.set_desired(0x00aa'0f34);
    passed &= expect("set_desired does not access the DUT",
                     reg.needs_update() && master.writes.empty());
    const auto update = co_await reg.update();
    passed &= expect("update performs one frontdoor write",
                     update.okay() && master.writes.size() == 1 &&
                         master.writes[0].address == 0x1020 &&
                         master.writes[0].data == 0x0000'f034 &&
                         master.writes[0].byte_enable == 0xf);
    passed &= expect("update encodes desired W1C state into bus data",
                     reg.mirrored() == 0x00aa'0f34 &&
                         reg.desired() == reg.mirrored());

    master.next_read = {0x0055'aa77, MemoryStatus::Okay, 0};
    const auto read = co_await reg.read();
    passed &= expect("read returns sampled frontdoor value",
                     read.data == 0x0055'aa77 && master.reads.size() == 1 &&
                         master.reads[0].address == 0x1020);
    passed &= expect("read-clear prediction tracks post-read state",
                     reg.mirrored() == 0x0000'aa77);

    auto control = reg.field(kFields[0]);
    control.set_desired(0x5a);
    passed &= expect("field desired state is explicit",
                     control.desired() == 0x5a && reg.needs_update());
    static_cast<void>(co_await control.write(0x66));
    passed &= expect("field write uses a whole-register frontdoor update",
                     master.writes.back().data == 0x0000'0066 &&
                         control.mirrored() == 0x66);

    master.next_read = {0x00cc'1122, MemoryStatus::Okay, 0};
    const auto field_read = co_await control.read();
    passed &= expect("field read returns only the selected field",
                     field_read.data == 0x22);
    passed &= expect("field read predicts sibling read side effects",
                     reg.mirrored() == 0x0000'1122);
}

cpptb::coro::Task<void> exercise_fieldless(
    cpptb::vc::RegisterHandle<FakeMaster>& reg, FakeMaster& master,
    bool& passed) {
    const auto writes_before = master.writes.size();
    static_cast<void>(co_await reg.update());
    passed &= expect("no-op update does not access the frontdoor",
                     master.writes.size() == writes_before);

    static_cast<void>(co_await reg.write(0x1ab));
    passed &= expect("fieldless write reaches the mirror",
                     reg.mirrored() == 0xab && reg.desired() == 0xab);
    passed &= expect("narrow fieldless write masks data and byte enables",
                     master.writes.back().data == 0xab &&
                         master.writes.back().byte_enable == 0x1);

    reg.predict(0x1ff, cpptb::vc::RegisterPrediction::Direct);
    passed &= expect("direct prediction masks to the register width",
                     reg.mirrored() == 0xff);
}

cpptb::coro::Task<void> exercise_split_registers(
    cpptb::vc::RegisterHandle<FakeMaster>& little,
    cpptb::vc::RegisterHandle<FakeMaster>& big, FakeMaster& master,
    bool& passed) {
    using cpptb::vc::MemoryStatus;
    const auto writes_before = master.writes.size();
    const auto little_write = co_await little.write(0x1122'3344);
    passed &= expect(
        "little-endian split write emits low-address low word first",
        little_write.okay() && little_write.transfers_completed == 2 &&
            master.writes.size() == writes_before + 2 &&
            master.writes[writes_before].address == 0x34 &&
            master.writes[writes_before].data == 0x3344 &&
            master.writes[writes_before].byte_enable == 0x3 &&
            master.writes[writes_before + 1].address == 0x36 &&
            master.writes[writes_before + 1].data == 0x1122 &&
            little.mirrored() == 0x1122'3344);

    master.read_responses = {
        {.data = 0xa1b2, .status = MemoryStatus::Okay},
        {.data = 0xc3d4, .status = MemoryStatus::Okay},
    };
    const auto little_read = co_await little.read();
    passed &= expect(
        "little-endian split read assembles all transfer data",
        little_read.okay() && little_read.transfers_completed == 2 &&
            little_read.valid_mask == 0xffff'ffff &&
            little_read.data == 0xc3d4'a1b2 &&
            little.mirrored() == 0xc3d4'a1b2);

    const auto big_writes_before = master.writes.size();
    static_cast<void>(co_await big.write(0x1122'3344));
    passed &= expect(
        "big-endian split write maps the high word to the low address",
        master.writes[big_writes_before].address == 0x3c &&
            master.writes[big_writes_before].data == 0x1122 &&
            master.writes[big_writes_before + 1].address == 0x3e &&
            master.writes[big_writes_before + 1].data == 0x3344 &&
            big.mirrored() == 0x1122'3344);

    little.reset();
    master.write_responses = {
        {.status = MemoryStatus::Okay},
        {.status = MemoryStatus::SlaveError},
    };
    const auto partial = co_await little.write(0x5566'7788);
    passed &= expect(
        "split write reports and predicts only completed transfers",
        !partial.okay() && partial.transfers_completed == 1 &&
            partial.failed_address == 0x36 && little.mirrored() == 0x7788 &&
            little.mirrored_valid_mask() == 0x0000'ffff);

    little.reset();
    master.read_responses = {
        {.data = 0xabcd, .status = MemoryStatus::Okay},
        {.data = 0, .status = MemoryStatus::SlaveError},
    };
    const auto partial_read = co_await little.read();
    passed &= expect(
        "split read reports and predicts only completed transfers",
        !partial_read.okay() && partial_read.transfers_completed == 1 &&
            partial_read.failed_address == 0x36 &&
            partial_read.data == 0xabcd &&
            partial_read.valid_mask == 0x0000'ffff &&
            little.mirrored() == 0xabcd &&
            little.mirrored_valid_mask() == 0x0000'ffff);
}

cpptb::coro::Task<void> exercise_wide_register(
    cpptb::vc::WideRegisterHandle<128, FakeMaster>& reg, FakeMaster& master,
    bool& passed) {
    using Wide = cpptb::Bits<128>;
    using cpptb::vc::MemoryStatus;
    const auto reset =
        Wide::from_hex("0xfedcba98765432100123456789abcdef");
    passed &= expect("wide reset words initialize complete model state",
                     reg.desired() == reset && reg.mirrored() == reset);

    const auto value =
        Wide::from_hex("0x112233445566778899aabbccddeeff00");
    const auto writes_before = master.writes.size();
    const auto write = co_await reg.write(value);
    passed &= expect(
        "wide frontdoor splits every logical word",
        write.okay() && write.transfers_completed == 4 &&
            master.writes.size() == writes_before + 4 &&
            master.writes[writes_before + 0].address == 0x80 &&
            master.writes[writes_before + 0].data == 0xddeeff00 &&
            master.writes[writes_before + 1].data == 0x99aabbcc &&
            master.writes[writes_before + 2].data == 0x55667788 &&
            master.writes[writes_before + 3].data == 0x11223344 &&
            reg.mirrored() == value);

    master.read_responses = {
        {.data = 0x76543210, .status = MemoryStatus::Okay},
        {.data = 0xfedcba98, .status = MemoryStatus::Okay},
        {.data = 0x01234567, .status = MemoryStatus::Okay},
        {.data = 0x89abcdef, .status = MemoryStatus::Okay},
    };
    const auto read = co_await reg.read();
    const auto sampled =
        Wide::from_hex("0x89abcdef01234567fedcba9876543210");
    passed &= expect("wide read assembles arbitrary-width data",
                     read.okay() && read.transfers_completed == 4 &&
                         read.data == sampled && reg.mirrored() == sampled);

    const auto deposited =
        Wide::from_hex("0xa55aa55a01234567fedcba980f0ff0f0");
    reg.poke(deposited);
    passed &= expect("wide raw backdoor round trips complete values",
                     reg.has_backdoor() && reg.peek() == deposited &&
                         reg.mirrored() == deposited);

    auto field = reg.template field<128>(kWide128Fields[0]);
    field.set_desired(value);
    passed &= expect("wide named field exposes typed desired state",
                     field.desired() == value && reg.needs_update());
}

cpptb::coro::Task<void> exercise_wide_register_policies(
    cpptb::vc::WideRegisterHandle<128, FakeMaster>& write_once,
    cpptb::vc::WideRegisterHandle<128, FakeMaster>& set_only,
    cpptb::TestResult& result, bool& passed) {
    using Wide = cpptb::Bits<128>;
    static_cast<void>(co_await write_once.write(Wide::from_hex("0x1234")));
    bool second_write_rejected = false;
    try {
        static_cast<void>(co_await write_once.write(Wide::from_hex("0x5678")));
    } catch (const std::logic_error& error) {
        second_write_rejected =
            std::string_view{error.what()}.find("block.wide128_write_once") !=
            std::string_view::npos;
    }
    passed &= expect("wide write-once policy rejects a second write",
                     second_write_rejected);

    set_only.set_desired(Wide{});
    const auto warnings_before = result.warnings;
    static_cast<void>(co_await set_only.update());
    passed &= expect(
        "wide unreachable desired state records a path-qualified warning",
        result.warnings == warnings_before + 1 &&
            !result.warning_records.empty() &&
            result.warning_records.back().label.find("block.wide128_set") !=
                std::string::npos);
    passed &= expect("wide unreachable desired state converges",
                     set_only.mirrored().bit(0) &&
                         set_only.desired().bit(0));
}

cpptb::coro::Task<void> exercise_prediction_validity(
    cpptb::vc::RegisterHandle<FakeMaster>& partial,
    cpptb::vc::RegisterHandle<FakeMaster>& unknown_fieldless,
    cpptb::vc::RegisterHandle<FakeMaster>& unknown_pair,
    FakeMaster& master, cpptb::TestResult& result, bool& passed) {
    passed &= expect("reset validity follows the descriptor reset mask",
                     partial.desired() == 0x005a &&
                         partial.mirrored() == 0x005a &&
                         partial.desired_valid_mask() == 0x00ff &&
                         partial.mirrored_valid_mask() == 0x00ff);

    auto known = partial.field(kPartialResetFields[0]);
    auto unknown = partial.field(kPartialResetFields[1]);
    passed &= expect("field validity masks are field-local",
                     known.desired_valid_mask() == 0xff &&
                         unknown.desired_valid_mask() == 0);

    unknown.set_desired(0x12);
    passed &= expect("field set only validates the selected desired field",
                     partial.desired() == 0x125a &&
                         partial.desired_valid_mask() == 0xffff &&
                         partial.mirrored_valid_mask() == 0x00ff &&
                         partial.needs_update());
    static_cast<void>(co_await partial.update());
    passed &= expect("successful write prediction validates written fields",
                     master.writes.back().data == 0x125a &&
                         partial.mirrored() == 0x125a &&
                         partial.mirrored_valid_mask() == 0xffff &&
                         !partial.needs_update());

    const auto checks_before = result.checks;
    const auto failures_before = result.failures;
    master.next_read = {0xa5, cpptb::vc::MemoryStatus::Okay, 0};
    static_cast<void>(co_await unknown_fieldless.mirror(
        cpptb::vc::MirrorCheck::Enabled));
    passed &= expect("mirror skips comparison for an unknown prediction",
                     result.checks == checks_before &&
                         result.failures == failures_before);
    passed &= expect("a successful read establishes prediction validity",
                     unknown_fieldless.mirrored() == 0xa5 &&
                         unknown_fieldless.mirrored_valid_mask() == 0xff &&
                         unknown_fieldless.desired_valid_mask() == 0xff);

    unknown_pair.field(kUnknownPairFields[0]).set_desired(0x33);
    bool rejected_unknown_sibling = false;
    try {
        static_cast<void>(co_await unknown_pair.update());
    } catch (const std::logic_error& error) {
        const std::string_view message{error.what()};
        rejected_unknown_sibling =
            message.find("block.unknown_pair") != std::string_view::npos &&
            message.find("unknown desired writable bits") !=
                std::string_view::npos;
    }
    passed &= expect("update rejects unknown writable sibling state",
                     rejected_unknown_sibling);
}

cpptb::coro::Task<void> exercise_mixed_access(
    cpptb::vc::RegisterHandle<FakeMaster>& reg, FakeMaster& master,
    cpptb::TestResult& result, bool& passed) {
    auto command = reg.field(kMixedFields[0]);
    command.set_desired(0xa5);
    master.next_read = {0x0056'7800, cpptb::vc::MemoryStatus::Okay, 0};
    static_cast<void>(co_await reg.read());
    passed &= expect("read prediction preserves write-only mirror state",
                     command.mirrored() == 0x5a);
    passed &= expect("read prediction preserves pending write-only intent",
                     command.desired() == 0xa5 && reg.needs_update());

    reg.predict(0x0011'225a);
    const auto failures_before = result.failures;
    master.next_read = {0x0011'22ff, cpptb::vc::MemoryStatus::Okay, 0};
    static_cast<void>(co_await reg.mirror(cpptb::vc::MirrorCheck::Enabled));
    passed &= expect("mirror comparison excludes write-only fields",
                     result.failures == failures_before);
}

cpptb::coro::Task<void> exercise_unreachable_update(
    cpptb::vc::RegisterHandle<FakeMaster>& reg, cpptb::TestResult& result,
    bool& passed) {
    reg.set_desired(0);
    const auto warnings_before = result.warnings;
    static_cast<void>(co_await reg.update());
    passed &= expect("unreachable desired state records a warning",
                     result.warnings == warnings_before + 1 &&
                         !result.warning_records.empty() &&
                         result.warning_records.back().label.find(
                             "block.set_only") != std::string::npos);
    passed &= expect("unreachable desired state converges to hardware state",
                     reg.mirrored() == 1 && reg.desired() == 1);
}

cpptb::coro::Task<void> exercise_memory_access_errors(
    cpptb::vc::RegisterMemoryHandle<FakeMaster>& read_only,
    cpptb::vc::RegisterMemoryHandle<FakeMaster>& write_only, bool& passed) {
    bool write_rejected = false;
    try {
        static_cast<void>(co_await read_only.write(0, 1));
    } catch (const std::logic_error& error) {
        write_rejected =
            std::string_view{error.what()}.find("read_only_buffer") !=
            std::string_view::npos;
    }
    passed &= expect("read-only register memory rejects writes",
                     write_rejected);

    bool read_rejected = false;
    try {
        static_cast<void>(co_await write_only.read(0));
    } catch (const std::logic_error& error) {
        read_rejected =
            std::string_view{error.what()}.find("write_only_buffer") !=
            std::string_view::npos;
    }
    passed &= expect("write-only register memory rejects reads", read_rejected);
}

bool exercise_write_effect_algebra() {
    using cpptb::vc::RegisterFieldDescriptor;
    using cpptb::vc::RegisterWriteEffect;
    using cpptb::vc::register_detail::apply_write_effect;
    using cpptb::vc::register_detail::apply_read_valid_mask;
    using cpptb::vc::register_detail::apply_write_valid_mask;
    using cpptb::vc::register_detail::encode_desired_write;

    bool passed = true;
    auto field = [](RegisterWriteEffect effect) {
        return RegisterFieldDescriptor{.name = "effect",
                                       .path = "block.effect",
                                       .lsb = 0,
                                       .width = 4,
                                       .write_effect = effect};
    };
    const auto check_effect = [&](const char* label, RegisterWriteEffect effect,
                                  uint64_t previous, uint64_t written,
                                  uint64_t expected) {
        passed &= expect(label,
                         apply_write_effect(previous, written, field(effect)) ==
                             expected);
    };
    check_effect("plain write effect", RegisterWriteEffect::None, 0x5, 0xa,
                 0xa);
    check_effect("write-one-set effect", RegisterWriteEffect::WriteOneSet, 0x5,
                 0x3, 0x7);
    check_effect("write-one-clear effect", RegisterWriteEffect::WriteOneClear,
                 0x5, 0x3, 0x4);
    check_effect("write-one-toggle effect", RegisterWriteEffect::WriteOneToggle,
                 0x5, 0x3, 0x6);
    check_effect("write-zero-set effect", RegisterWriteEffect::WriteZeroSet,
                 0x5, 0xa, 0x5);
    check_effect("write-zero-clear effect", RegisterWriteEffect::WriteZeroClear,
                 0x5, 0xa, 0x0);
    check_effect("write-zero-toggle effect",
                 RegisterWriteEffect::WriteZeroToggle, 0x5, 0xa, 0x0);
    check_effect("write-clear effect", RegisterWriteEffect::Clear, 0x5, 0xf,
                 0x0);
    check_effect("write-set effect", RegisterWriteEffect::Set, 0x5, 0x0, 0xf);

    const auto check_encoding = [&](const char* label,
                                    RegisterWriteEffect effect,
                                    uint64_t previous, uint64_t desired) {
        const auto descriptor = field(effect);
        const auto encoded =
            encode_desired_write(previous, desired, descriptor);
        passed &= expect(
            label,
            apply_write_effect(previous, encoded, descriptor) == desired);
    };
    check_encoding("plain desired encoding", RegisterWriteEffect::None, 0x5,
                   0xa);
    check_encoding("write-one-set desired encoding",
                   RegisterWriteEffect::WriteOneSet, 0x5, 0xf);
    check_encoding("write-one-clear desired encoding",
                   RegisterWriteEffect::WriteOneClear, 0x5, 0x1);
    check_encoding("write-one-toggle desired encoding",
                   RegisterWriteEffect::WriteOneToggle, 0x5, 0xa);
    check_encoding("write-zero-set desired encoding",
                   RegisterWriteEffect::WriteZeroSet, 0x5, 0xf);
    check_encoding("write-zero-clear desired encoding",
                   RegisterWriteEffect::WriteZeroClear, 0x5, 0x1);
    check_encoding("write-zero-toggle desired encoding",
                   RegisterWriteEffect::WriteZeroToggle, 0x5, 0xa);
    check_encoding("write-clear desired encoding", RegisterWriteEffect::Clear,
                   0x5, 0x0);
    check_encoding("write-set desired encoding", RegisterWriteEffect::Set, 0x5,
                   0xf);

    passed &= expect(
        "write-one-clear validity includes deterministically cleared bits",
        apply_write_valid_mask(0x1, 0x6,
                               field(RegisterWriteEffect::WriteOneClear)) ==
            0x7);
    passed &= expect(
        "toggle validity requires a known previous value",
        apply_write_valid_mask(0x5, 0xf,
                               field(RegisterWriteEffect::WriteOneToggle)) ==
            0x5);
    passed &= expect(
        "write-zero-set validity includes deterministically set bits",
        apply_write_valid_mask(0x1, 0x9,
                               field(RegisterWriteEffect::WriteZeroSet)) ==
            0x7);
    passed &= expect(
        "user write effects invalidate their predicted result",
        apply_write_valid_mask(0xf, 0xa,
                               field(RegisterWriteEffect::User)) == 0);

    auto user_read = field(RegisterWriteEffect::None);
    user_read.read_effect = cpptb::vc::RegisterReadEffect::User;
    passed &= expect("user read effects invalidate post-read prediction",
                     apply_read_valid_mask(0xf, user_read) == 0);

    auto plain_read = field(RegisterWriteEffect::None);
    plain_read.read_effect = cpptb::vc::RegisterReadEffect::None;
    passed &= expect("ordinary reads establish prediction validity",
                     apply_read_valid_mask(0, plain_read) == 0xf);

    const auto unknown_w1s = field(RegisterWriteEffect::WriteOneSet);
    passed &= expect(
        "unknown write-one-set state emits deterministic set commands",
        encode_desired_write(0, 0, 0xa, unknown_w1s) == 0xa);
    return passed;
}

bool exercise_register_predictor(cpptb::TestContext test,
                                 cpptb::TestResult& result,
                                 FakeMaster& master) {
    using Transaction =
        cpptb::vc::MemoryTransaction<uint32_t, uint32_t, uint8_t>;
    bool passed = true;
    cpptb::vc::RegisterHandle command{test, master, kCommand, 0x1000};
    cpptb::vc::RegisterHandle partial{test, master, kPartialReset, 0x1000};
    std::array<cpptb::vc::RegisterHandle<FakeMaster>*, 2> handles{
        &partial, &command};
    cpptb::vc::RegisterPredictor predictor{
        test,
        std::span<cpptb::vc::RegisterHandle<FakeMaster>* const>{handles}};

    predictor.write(Transaction{
        .operation = cpptb::vc::MemoryOperation::Write,
        .address = 0x1020,
        .data = 0x1234'5678,
        .byte_enable = 0x1,
    });
    passed &= expect("predictor applies enabled write bytes only",
                     command.mirrored() == 0x00aa'ff78 &&
                         command.mirrored_valid_mask() == 0x00ff'ffff);

    predictor.write(Transaction{
        .operation = cpptb::vc::MemoryOperation::Write,
        .address = 0x1020,
        .data = 0x0000'0f00,
        .byte_enable = 0x2,
    });
    passed &= expect("partial predictor write preserves field effects",
                     command.mirrored() == 0x00aa'f078);

    predictor.write(Transaction{
        .operation = cpptb::vc::MemoryOperation::Read,
        .address = 0x1020,
        .data = 0x0055'aa33,
    });
    passed &= expect("predictor applies read data and read effects",
                     command.mirrored() == 0x0000'aa33);

    predictor.write(Transaction{
        .operation = cpptb::vc::MemoryOperation::Write,
        .address = 0x102a,
        .data = 0x0000'1200,
        .byte_enable = 0x2,
    });
    passed &= expect("predictor establishes validity for a partial write",
                     partial.mirrored() == 0x125a &&
                         partial.mirrored_valid_mask() == 0xffff);

    const auto warnings_before = result.warnings;
    predictor.write(Transaction{
        .operation = cpptb::vc::MemoryOperation::Write,
        .address = 0x102a,
        .data = 0xffff'ffff,
        .byte_enable = 0x82,
    });
    predictor.write(Transaction{
        .operation = cpptb::vc::MemoryOperation::Read,
        .address = 0x1ffc,
        .data = 0,
    });
    predictor.write(Transaction{
        .operation = cpptb::vc::MemoryOperation::Write,
        .address = 0x1020,
        .data = 0,
        .byte_enable = 0xf,
        .status = cpptb::vc::MemoryStatus::SlaveError,
    });
    passed &= expect("predictor reports transaction disposition",
                     predictor.reads() == 1 && predictor.writes() == 4 &&
                         predictor.unmapped() == 1 && predictor.failed() == 1 &&
                         predictor.invalid_byte_enables() == 1 &&
                         result.warnings == warnings_before + 1 &&
                         result.warning_records.back().label.find(
                             "block.partial") != std::string::npos);

    bool duplicate_rejected = false;
    std::array<cpptb::vc::RegisterHandle<FakeMaster>*, 2> duplicate{
        &command, &command};
    try {
        cpptb::vc::RegisterPredictor<FakeMaster> bad{
            test,
            std::span<cpptb::vc::RegisterHandle<FakeMaster>* const>{duplicate}};
    } catch (const std::invalid_argument& error) {
        duplicate_rejected =
            std::string_view{error.what()}.find("duplicate address") !=
            std::string_view::npos;
    }
    passed &= expect("predictor rejects duplicate register addresses",
                     duplicate_rejected);

    cpptb::vc::RegisterHandle split{test, master, kSplitRegister, 0x1000};
    std::array<cpptb::vc::RegisterHandle<FakeMaster>*, 1> split_handles{
        &split};
    cpptb::vc::RegisterPredictor split_predictor{
        test,
        std::span<cpptb::vc::RegisterHandle<FakeMaster>* const>{split_handles}};
    split_predictor.write(Transaction{
        .operation = cpptb::vc::MemoryOperation::Write,
        .address = 0x1034,
        .data = 0xabcd,
        .byte_enable = 0x3,
    });
    split_predictor.write(Transaction{
        .operation = cpptb::vc::MemoryOperation::Write,
        .address = 0x1036,
        .data = 0x1234,
        .byte_enable = 0x3,
    });
    passed &= expect("predictor assembles split write transactions",
                     split.mirrored() == 0x1234'abcd &&
                         split.mirrored_valid_mask() == 0xffff'ffff &&
                         split_predictor.writes() == 2);
    return passed;
}

bool exercise_wide_register_predictor(cpptb::TestContext test,
                                      cpptb::TestResult& result,
                                      FakeMaster& master) {
    using Transaction =
        cpptb::vc::MemoryTransaction<uint32_t, uint32_t, uint8_t>;
    using Wide = cpptb::Bits<128>;
    bool passed = true;
    cpptb::vc::WideRegisterHandle<128, FakeMaster> wide{
        test, master, kWide128Register, 0x1000};
    std::array<cpptb::vc::WideRegisterHandle<128, FakeMaster>*, 1> handles{
        &wide};
    cpptb::vc::WideRegisterPredictor predictor{
        test,
        std::span<cpptb::vc::WideRegisterHandle<128, FakeMaster>* const>{
            handles}};

    predictor.write(Transaction{
        .operation = cpptb::vc::MemoryOperation::Write,
        .address = 0x1080,
        .data = 0x1122'3344,
        .byte_enable = 0xf,
    });
    predictor.write(Transaction{
        .operation = cpptb::vc::MemoryOperation::Write,
        .address = 0x1084,
        .data = 0x5566'7788,
        .byte_enable = 0x3,
    });
    predictor.write(Transaction{
        .operation = cpptb::vc::MemoryOperation::Read,
        .address = 0x108c,
        .data = 0xaabb'ccdd,
    });
    const auto expected =
        Wide::from_hex("0xaabbccdd765432100123778811223344");
    passed &= expect(
        "wide predictor assembles split reads and byte-enabled writes",
        wide.mirrored() == expected &&
            wide.mirrored_valid_mask() ==
                Wide::from_hex("0xffffffffffffffffffffffffffffffff"));

    const auto warnings_before = result.warnings;
    predictor.write(Transaction{
        .operation = cpptb::vc::MemoryOperation::Write,
        .address = 0x1088,
        .data = 0x1234'5678,
        .byte_enable = 0x8f,
    });
    predictor.write(Transaction{
        .operation = cpptb::vc::MemoryOperation::Read,
        .address = 0x1082,
    });
    predictor.write(Transaction{
        .operation = cpptb::vc::MemoryOperation::Write,
        .address = 0x1080,
        .status = cpptb::vc::MemoryStatus::SlaveError,
    });
    passed &= expect(
        "wide predictor reports invalid, unmapped, and failed traffic",
        predictor.reads() == 1 && predictor.writes() == 3 &&
            predictor.unmapped() == 1 && predictor.failed() == 1 &&
            predictor.invalid_byte_enables() == 1 &&
            result.warnings == warnings_before + 1 &&
            result.warning_records.back().label.find("block.wide128") !=
                std::string::npos);

    bool duplicate_rejected = false;
    std::array<cpptb::vc::WideRegisterHandle<128, FakeMaster>*, 2> duplicate{
        &wide, &wide};
    try {
        cpptb::vc::WideRegisterPredictor<128, FakeMaster> bad{
            test,
            std::span<cpptb::vc::WideRegisterHandle<128, FakeMaster>* const>{
                duplicate}};
    } catch (const std::invalid_argument& error) {
        duplicate_rejected =
            std::string_view{error.what()}.find("duplicate address") !=
            std::string_view::npos;
    }
    passed &= expect("wide predictor rejects duplicate register ranges",
                     duplicate_rejected);
    return passed;
}

cpptb::coro::Task<void> exercise_register_address_maps(
    cpptb::TestContext test, FakeMaster& master, bool& passed) {
    cpptb::vc::RegisterHandle fieldless{test, master, kFieldless};
    FakeRegisterFrontdoor custom;
    cpptb::vc::RegisterAddressMap primary{"primary", master, 0x1000};
    cpptb::vc::RegisterAddressMap debug{"debug", master, 0x8000};
    debug.route(kFieldless, 0x40, &custom);

    const auto master_writes_before = master.writes.size();
    static_cast<void>(co_await fieldless.write(0x12, primary));
    passed &= expect(
        "primary address map relocates the logical register",
        master.writes.size() == master_writes_before + 1 &&
            master.writes.back().address == 0x1028 &&
            primary.effective_address(kFieldless) == 0x1028);

    static_cast<void>(co_await fieldless.write(0x34, debug));
    const auto debug_read = co_await fieldless.read(debug);
    passed &= expect(
        "per-register custom frontdoor owns routed map traffic",
        master.writes.size() == master_writes_before + 1 &&
            custom.writes == 1 && custom.reads == 1 &&
            custom.last_address == 0x8040 &&
            custom.last_path == "block.fieldless" &&
            debug_read.data == 0x34 && fieldless.mirrored() == 0x34);

    cpptb::vc::RegisterAddressMap alias{"legacy_alias", master, 0x9000};
    alias.route(kFieldless, 0x08);
    static_cast<void>(co_await fieldless.write(0x56, alias));
    passed &= expect(
        "an alias map reuses one logical mirror at another address",
        master.writes.back().address == 0x9008 &&
            fieldless.mirrored() == 0x56 && alias.name() == "legacy_alias");

    using Transaction =
        cpptb::vc::MemoryTransaction<uint32_t, uint32_t, uint8_t>;
    fieldless.reset();
    fieldless.set_auto_predict(false);
    std::array<cpptb::vc::RegisterHandle<FakeMaster>*, 1> alias_handles{
        &fieldless};
    cpptb::vc::RegisterPredictor alias_predictor{
        test,
        std::span<cpptb::vc::RegisterHandle<FakeMaster>* const>{alias_handles}};
    alias_predictor.add_alias(fieldless, alias);
    bool duplicate_predictor_alias_rejected = false;
    try {
        alias_predictor.add_alias(fieldless, alias);
    } catch (const std::invalid_argument&) {
        duplicate_predictor_alias_rejected = true;
    }
    alias_predictor.write(Transaction{
        .operation = cpptb::vc::MemoryOperation::Write,
        .address = 0x9008,
        .data = 0xa5,
        .byte_enable = 0x1,
    });
    passed &= expect("passive predictor recognizes an address-map alias",
                     fieldless.mirrored() == 0xa5 &&
                         alias_predictor.writes() == 1 &&
                         alias_predictor.unmapped() == 0 &&
                         duplicate_predictor_alias_rejected);

    fieldless.set_desired(0x78);
    static_cast<void>(co_await fieldless.update(debug));
    custom.storage = 0x78;
    static_cast<void>(co_await fieldless.mirror(
        debug, cpptb::vc::MirrorCheck::Enabled));
    passed &= expect("update and mirror preserve explicit map selection",
                     custom.storage == 0x78 && custom.writes == 2 &&
                         custom.reads == 2);

    cpptb::vc::WideRegisterHandle<128, FakeMaster> wide{
        test, master, kWide128Register};
    cpptb::vc::RegisterAddressMap wide_map{"wide", master, 0x2000};
    wide_map.route(kWide128Register, 0x100);
    const auto wide_value = cpptb::Bits<128>::from_hex(
        "0x112233445566778899aabbccddeeff00");
    const auto wide_writes_before = master.writes.size();
    static_cast<void>(co_await wide.write(wide_value, wide_map));
    passed &= expect(
        "wide registers use the same alternate-map contract",
        master.writes.size() == wide_writes_before + 4 &&
            master.writes[wide_writes_before].address == 0x2100 &&
            master.writes[wide_writes_before + 3].address == 0x210c);

    cpptb::vc::RegisterMemoryHandle memory{master, kBuffer};
    FakeRegisterMemoryFrontdoor memory_frontdoor;
    cpptb::vc::RegisterAddressMap memory_map{"memory_debug", master, 0xa000};
    memory_map.route(kBuffer, 0x200, &memory_frontdoor);
    std::array<uint32_t, 3> memory_values{0x10, 0x20, 0x30};
    static_cast<void>(co_await memory.write(4, memory_values, memory_map));
    std::array<uint32_t, 3> memory_readback{};
    static_cast<void>(
        co_await memory.read_into(4, memory_readback, memory_map));
    passed &= expect(
        "memory windows support routed bulk traffic and custom frontdoors",
        memory_values == memory_readback && memory_frontdoor.writes == 3 &&
            memory_frontdoor.reads == 3 &&
            memory_frontdoor.last_path == "block.buffer" &&
            memory_frontdoor.last_index == 6 &&
            memory_frontdoor.last_address == 0xa218 &&
            memory.address(6, memory_map) == 0xa218);

    cpptb::vc::RegisterAddressMap memory_alias{"memory_alias", master, 0xb000};
    memory_alias.route(kBuffer, 0x80);
    const auto memory_writes_before = master.writes.size();
    static_cast<void>(co_await memory.write(2, 0xabcdef01, memory_alias));
    passed &= expect("memory aliases use the selected bus view",
                     master.writes.size() == memory_writes_before + 1 &&
                         master.writes.back().address == 0xb088);

    cpptb::vc::WideRegisterMemoryHandle<128, FakeMaster> wide_memory{
        master, kWideBuffer};
    cpptb::vc::RegisterAddressMap wide_memory_map{
        "wide_memory", master, 0xc000};
    wide_memory_map.route(kWideBuffer, 0x400);
    const auto wide_memory_writes_before = master.writes.size();
    static_cast<void>(
        co_await wide_memory.write(1, wide_value, wide_memory_map));
    passed &= expect(
        "wide memory maps preserve split-transfer addresses",
        master.writes.size() == wide_memory_writes_before + 4 &&
            master.writes[wide_memory_writes_before].address == 0xc410 &&
            master.writes[wide_memory_writes_before + 3].address == 0xc41c &&
            wide_memory.address(1, wide_memory_map) == 0xc410);

    bool empty_name_reported = false;
    try {
        cpptb::vc::RegisterAddressMap unnamed{"", master};
    } catch (const std::invalid_argument& error) {
        empty_name_reported =
            std::string_view{error.what()}.find("name is empty") !=
            std::string_view::npos;
    }
    passed &= expect("address maps require a useful name",
                     empty_name_reported);

    bool overflow_reported = false;
    try {
        cpptb::vc::RegisterAddressMap overflowing{
            "overflowing", master, std::numeric_limits<uint64_t>::max()};
        static_cast<void>(overflowing.effective_address(kFieldless));
    } catch (const std::overflow_error& error) {
        const std::string_view diagnostic{error.what()};
        overflow_reported =
            diagnostic.find("block.fieldless") != std::string_view::npos &&
            diagnostic.find("overflowing") != std::string_view::npos;
    }
    passed &= expect("address-map overflow identifies map and register",
                     overflow_reported);

    cpptb::vc::RegisterAddressMap stable{"stable", master};
    stable.route(kFieldless, 0x10);
    bool register_route_rolled_back = false;
    try {
        stable.route(kFieldless, std::numeric_limits<uint64_t>::max());
    } catch (const std::overflow_error&) {
        register_route_rolled_back =
            stable.effective_address(kFieldless) == 0x10;
    }
    passed &= expect("failed register remap preserves the previous route",
                     register_route_rolled_back);

    stable.route(kBuffer, 0x20);
    bool memory_route_rolled_back = false;
    try {
        stable.route(kBuffer, std::numeric_limits<uint64_t>::max());
    } catch (const std::overflow_error&) {
        memory_route_rolled_back =
            stable.effective_address(kBuffer, 0) == 0x20;
    }
    passed &= expect("failed memory remap preserves the previous route",
                     memory_route_rolled_back);
}

bool exercise_descriptor_validation(cpptb::TestContext test,
                                    FakeMaster& master) {
    bool passed = true;

    constexpr std::array outside_fields{
        cpptb::vc::RegisterFieldDescriptor{
            .name = "outside",
            .path = "invalid.narrow.outside",
            .lsb = 64,
            .width = 1,
        },
    };
    const cpptb::vc::RegisterDescriptor outside_register{
        .name = "invalid_narrow",
        .path = "invalid.narrow",
        .width = 64,
        .access_width = 32,
        .fields = outside_fields,
    };
    bool outside_rejected = false;
    try {
        cpptb::vc::RegisterHandle invalid{test, master, outside_register};
    } catch (const std::invalid_argument& error) {
        outside_rejected =
            std::string_view{error.what()}.find("invalid.narrow.outside") !=
            std::string_view::npos;
    }
    passed &= expect("narrow register rejects a field outside its width",
                     outside_rejected);

    constexpr std::array overlapping_fields{
        cpptb::vc::RegisterFieldDescriptor{
            .name = "low",
            .path = "invalid.overlap.low",
            .lsb = 0,
            .width = 8,
        },
        cpptb::vc::RegisterFieldDescriptor{
            .name = "high",
            .path = "invalid.overlap.high",
            .lsb = 4,
            .width = 8,
        },
    };
    const cpptb::vc::RegisterDescriptor overlapping_register{
        .name = "invalid_overlap",
        .path = "invalid.overlap",
        .width = 32,
        .access_width = 32,
        .fields = overlapping_fields,
    };
    bool overlap_rejected = false;
    try {
        cpptb::vc::RegisterHandle invalid{test, master,
                                          overlapping_register};
    } catch (const std::invalid_argument& error) {
        const std::string_view diagnostic{error.what()};
        overlap_rejected =
            diagnostic.find("invalid.overlap.low") != std::string_view::npos &&
            diagnostic.find("invalid.overlap.high") != std::string_view::npos;
    }
    passed &= expect("register rejects overlapping fields with both paths",
                     overlap_rejected);

    constexpr std::array wide_outside_fields{
        cpptb::vc::RegisterFieldDescriptor{
            .name = "outside",
            .path = "invalid.wide.outside",
            .lsb = 127,
            .width = 2,
        },
    };
    const cpptb::vc::RegisterDescriptor wide_outside_register{
        .name = "invalid_wide",
        .path = "invalid.wide",
        .width = 128,
        .access_width = 32,
        .fields = wide_outside_fields,
    };
    bool wide_outside_rejected = false;
    try {
        cpptb::vc::WideRegisterHandle<128, FakeMaster> invalid{
            test, master, wide_outside_register};
    } catch (const std::invalid_argument& error) {
        wide_outside_rejected =
            std::string_view{error.what()}.find("invalid.wide.outside") !=
            std::string_view::npos;
    }
    passed &= expect("wide register rejects a field outside its width",
                     wide_outside_rejected);

    cpptb::vc::RegisterHandle partial_write_once{
        test, master, kSplitWriteOnceRegister};
    partial_write_once.predict_transfer_write(
        partial_write_once.address(), 0x0000'bbaa, 0x1);
    partial_write_once.predict_transfer_write(
        partial_write_once.address(), 0x0000'bbaa, 0x2);
    partial_write_once.predict_transfer_write(
        partial_write_once.address(), 0x0000'00cc, 0x1);
    passed &= expect(
        "write-once prediction tracks enabled bytes independently",
        partial_write_once.mirrored() == 0xbbaa &&
            partial_write_once.mirrored_valid_mask() == 0xffff);

    const cpptb::vc::RegisterDescriptor overflowing_register{
        .name = "overflowing",
        .path = "invalid.overflowing_register",
        .address = std::numeric_limits<uint64_t>::max() - 1,
        .width = 32,
        .access_width = 32,
    };
    bool register_range_rejected = false;
    try {
        cpptb::vc::RegisterHandle invalid{test, master,
                                          overflowing_register};
    } catch (const std::invalid_argument& error) {
        register_range_rejected =
            std::string_view{error.what()}.find(
                "invalid.overflowing_register") != std::string_view::npos;
    }
    passed &= expect("register rejects an overflowing complete address range",
                     register_range_rejected);

    const cpptb::vc::RegisterMemoryDescriptor overflowing_memory{
        .name = "overflowing_memory",
        .path = "invalid.overflowing_memory",
        .address = std::numeric_limits<uint64_t>::max() - 3,
        .entries = 2,
        .width = 32,
        .access_width = 32,
    };
    bool memory_range_rejected = false;
    try {
        cpptb::vc::RegisterMemoryHandle invalid{master, overflowing_memory};
    } catch (const std::invalid_argument& error) {
        memory_range_rejected =
            std::string_view{error.what()}.find(
                "invalid.overflowing_memory") != std::string_view::npos;
    }
    passed &= expect("memory rejects an overflowing complete address range",
                     memory_range_rejected);
    return passed;
}

cpptb::coro::Task<void> exercise_user_effect_policy(
    cpptb::TestContext test, FakeMaster& master, bool& passed) {
    FakeUserEffectPolicy policy;
    cpptb::vc::RegisterHandle custom{
        test, master, kUserEffectRegister, nullptr, &policy};
    custom.predict(0, cpptb::vc::RegisterPrediction::Direct);
    custom.set_desired(0x0a);
    const auto write = co_await custom.update();
    passed &= expect(
        "user write policy encodes and predicts update traffic",
        write.okay() && master.writes.back().address == 0x2c &&
            master.writes.back().data == 0x0a && custom.mirrored() == 0x0a &&
            custom.mirrored_valid_mask() == 0xff && policy.encodes >= 4 &&
            policy.write_predictions >= 4 && custom.has_user_effect_policy());

    custom.predict(0x05, cpptb::vc::RegisterPrediction::Read);
    passed &= expect(
        "user read policy controls value and validity prediction",
        custom.mirrored() == 0x0a && custom.mirrored_valid_mask() == 0xff &&
            policy.read_predictions == 4 &&
            policy.last_register_path == "block.user_effect" &&
            policy.last_field_path == "block.user_effect.custom" &&
            policy.last_field_bit == 3);

    FakeFieldUserEffectPolicy field_policy;
    cpptb::vc::RegisterHandle packed_custom{
        test, master, kUserEffectRegister, nullptr, &field_policy};
    packed_custom.predict(0, cpptb::vc::RegisterPrediction::Direct);
    packed_custom.set_desired(0x0a);
    const auto packed_write = co_await packed_custom.update();
    packed_custom.predict(0x05, cpptb::vc::RegisterPrediction::Read);
    passed &= expect(
        "packed user-effect callbacks run once per field",
        packed_write.okay() && packed_custom.mirrored() == 0x0a &&
            packed_custom.mirrored_valid_mask() == 0xff &&
            field_policy.field_encodes == 1 &&
            field_policy.field_write_predictions == 1 &&
            field_policy.field_read_predictions == 1 &&
            field_policy.encodes == 0 && field_policy.write_predictions == 0 &&
            field_policy.read_predictions == 0 &&
            field_policy.last_register_path == "block.user_effect" &&
            field_policy.last_field_path == "block.user_effect.custom" &&
            field_policy.last_previous == 0x0a &&
            field_policy.last_value == 0x05);

    cpptb::vc::RegisterHandle missing_policy{
        test, master, kUserEffectRegister};
    missing_policy.predict(0x0f, cpptb::vc::RegisterPrediction::Read);
    passed &= expect(
        "user effects without a policy remain explicitly unknown",
        !missing_policy.has_user_effect_policy() &&
            missing_policy.mirrored_valid_mask() == 0);

    cpptb::vc::WideRegisterHandle<128, FakeMaster> wide{
        test, master, kWideUserEffectRegister, nullptr, &policy};
    const auto wide_value = cpptb::Bits<128>::from_hex(
        "0xfedcba98765432100123456789abcdef");
    const auto wide_valid = cpptb::Bits<128>::from_hex(
        "0xffffffffffffffffffffffffffffffff");
    wide.predict(cpptb::Bits<128>{}, cpptb::vc::RegisterPrediction::Direct);
    wide.set_desired(wide_value);
    const auto wide_write = co_await wide.update();
    passed &= expect(
        "wide registers use the same user-effect policy",
        wide_write.okay() && wide.mirrored() == wide_value &&
            wide.mirrored_valid_mask() == wide_valid &&
            wide.has_user_effect_policy() &&
            policy.last_register_path == "block.wide_user_effect" &&
            policy.last_field_bit == 127);
}

cpptb::coro::Task<void> exercise_passive_prediction_mode(
    cpptb::TestContext test, FakeMaster& master, bool& passed) {
    using Transaction =
        cpptb::vc::MemoryTransaction<uint32_t, uint32_t, uint8_t>;
    FakeUserEffectPolicy policy;
    cpptb::vc::RegisterHandle reg{
        test, master, kUserEffectRegister, nullptr, &policy};
    reg.set_auto_predict(false);
    std::array<cpptb::vc::RegisterHandle<FakeMaster>*, 1> handles{&reg};
    cpptb::vc::RegisterPredictor predictor{
        test,
        std::span<cpptb::vc::RegisterHandle<FakeMaster>* const>{handles}};

    static_cast<void>(co_await reg.write(0x0a));
    passed &= expect(
        "disabled auto prediction leaves frontdoor traffic to the monitor",
        !reg.auto_predict() && policy.write_predictions == 0);

    const auto& request = master.writes.back();
    predictor.write(Transaction{
        .operation = cpptb::vc::MemoryOperation::Write,
        .address = request.address,
        .data = request.data,
        .byte_enable = request.byte_enable,
    });
    passed &= expect("passive predictor applies user effects exactly once",
                     predictor.writes() == 1 &&
                         policy.write_predictions == 4);
}

cpptb::coro::Task<void> exercise_mirror(
    cpptb::vc::RegisterHandle<FakeMaster>& reg, FakeMaster& master,
    cpptb::TestResult& result, bool& passed) {
    const auto failures_before = result.failures;
    const auto checks_before = result.checks;
    master.next_read = {0x00fe'ff12, cpptb::vc::MemoryStatus::Okay, 0};
    static_cast<void>(co_await reg.mirror(cpptb::vc::MirrorCheck::Enabled));
    passed &= expect("mirror ignores volatile field mismatches",
                     result.failures == failures_before &&
                         result.checks == checks_before + 1);

    master.next_read = {0x00fe'0012, cpptb::vc::MemoryStatus::Okay, 0};
    static_cast<void>(co_await reg.mirror(cpptb::vc::MirrorCheck::Enabled));
    passed &= expect("mirror checks stable fields before prediction",
                     result.failures == failures_before + 1 &&
                         result.checks == checks_before + 2);
}

cpptb::coro::Task<void> exercise_write_once(
    cpptb::vc::RegisterHandle<FakeMaster>& reg, bool& rejected) {
    static_cast<void>(co_await reg.write(0x1234));
    try {
        static_cast<void>(co_await reg.write(0x5678));
    } catch (const std::logic_error& error) {
        rejected = std::string_view{error.what()}.find("already written") !=
                   std::string_view::npos;
    }
}

cpptb::coro::Task<void> exercise_split_write_once(
    cpptb::vc::RegisterHandle<FakeMaster>& reg, bool& passed) {
    const auto write = co_await reg.write(0x1122'3344);
    passed &= expect(
        "split write-once prediction commits every completed transfer",
        write.okay() && write.transfers_completed == 2 &&
            reg.mirrored() == 0x1122'3344 &&
            reg.mirrored_valid_mask() == 0xffff'ffff);

    bool second_write_rejected = false;
    try {
        static_cast<void>(co_await reg.write(0x5566'7788));
    } catch (const std::logic_error& error) {
        second_write_rejected =
            std::string_view{error.what()}.find("already written") !=
            std::string_view::npos;
    }
    passed &= expect("split write-once rejects a second logical write",
                     second_write_rejected);
}

cpptb::coro::Task<void> exercise_register_memory(
    cpptb::vc::RegisterMemoryHandle<FakeMaster>& memory, FakeMaster& master,
    FakeMemoryBackdoor& backdoor, bool& passed) {
    using cpptb::vc::AccessPath;
    using cpptb::vc::MemoryStatus;

    const auto write = co_await memory.write(3, 0x1234'5678);
    passed &= expect("register memory write uses indexed frontdoor address",
                     write.okay() && write.path == AccessPath::Frontdoor &&
                         write.transfers_completed == 1 &&
                         master.writes.back().address == 0x110c &&
                         master.writes.back().byte_enable == 0xf);

    master.next_read = {0xa5a5'5a5a, cpptb::vc::MemoryStatus::Okay, 0};
    const auto read = co_await memory.read(3);
    passed &= expect("register memory read returns frontdoor response",
                     read.okay() && read.path == AccessPath::Frontdoor &&
                         read.data == 0xa5a5'5a5a &&
                         master.reads.back().address == 0x110c);

    const auto frontdoor_writes = master.writes.size();
    const auto semantic_write =
        co_await memory.write(4, 0xcafe'babe, AccessPath::Backdoor);
    const auto semantic_read =
        co_await memory.read(4, AccessPath::Backdoor);
    passed &= expect(
        "register memory selects semantic backdoor per operation",
        semantic_write.okay() && semantic_read.okay() &&
            semantic_write.path == AccessPath::Backdoor &&
            semantic_read.path == AccessPath::Backdoor &&
            semantic_read.data == 0xcafe'babe &&
            semantic_write.transfers_completed == 1 &&
            master.writes.size() == frontdoor_writes &&
            backdoor.last_path == "block.buffer" &&
            backdoor.last_index == 4 && backdoor.last_address == 0x1110);

    memory.poke(2, 0xfeed'1234);
    passed &= expect("raw memory poke and peek are synchronous",
                     memory.peek(2) == 0xfeed'1234 &&
                         backdoor.last_address == 0x1108);

    const std::array<uint32_t, 3> frontdoor_values{
        0x1111'1111, 0x2222'2222, 0x3333'3333};
    const auto writes_before_bulk = master.writes.size();
    const auto block_write = co_await memory.write_offset(
        0x14, std::span<const uint32_t>{frontdoor_values});
    passed &= expect("offset bulk frontdoor write uses consecutive addresses",
                     block_write.okay() &&
                         block_write.transfers_completed == 3 &&
                         master.writes.size() == writes_before_bulk + 3 &&
                         master.writes[writes_before_bulk].address == 0x1114 &&
                         master.writes[writes_before_bulk + 2].address ==
                             0x111c);

    master.read_responses = {
        {.data = 0xaaaa'0001, .status = MemoryStatus::Okay},
        {.data = 0xbbbb'0002, .status = MemoryStatus::Okay},
        {.data = 0xcccc'0003, .status = MemoryStatus::Okay},
    };
    std::array<uint32_t, 3> frontdoor_readback{};
    const auto block_read = co_await memory.read_absolute(
        0x1114, std::span{frontdoor_readback});
    passed &= expect("absolute bulk frontdoor read fills caller-owned storage",
                     block_read.okay() &&
                         block_read.transfers_completed == 3 &&
                         frontdoor_readback[0] == 0xaaaa'0001 &&
                         frontdoor_readback[2] == 0xcccc'0003);

    master.write_responses = {
        {.status = MemoryStatus::Okay},
        {.status = MemoryStatus::SlaveError},
    };
    const auto partial = co_await memory.write(
        9, std::span<const uint32_t>{frontdoor_values});
    passed &= expect("bulk frontdoor stops and reports the failing entry",
                     !partial.okay() && partial.transfers_completed == 1 &&
                         partial.failed_index == 10);

    const std::array<uint32_t, 4> backdoor_values{
        0x10, 0x20, 0x30, 0x40};
    const auto backdoor_block_write = co_await memory.write(
        8, std::span<const uint32_t>{backdoor_values},
        AccessPath::Backdoor);
    std::array<uint32_t, 4> backdoor_readback{};
    const auto backdoor_block_read = co_await memory.read(
        8, std::span{backdoor_readback}, AccessPath::Backdoor);
    passed &= expect("bulk backdoor uses the allocation-free span adapter",
                     backdoor_block_write.okay() &&
                         backdoor_block_read.okay() &&
                         backdoor_block_read.transfers_completed == 4 &&
                         backdoor_readback == backdoor_values &&
                         backdoor.bulk_pokes == 1 && backdoor.bulk_peeks == 1);

    const std::array<uint32_t, 4> addressed_values{
        0x51, 0x62, 0x73, 0x84};
    std::array<uint32_t, 4> addressed_readback{};
    const auto offset_write = co_await memory.write_offset(
        0x20, addressed_values, AccessPath::Backdoor);
    const auto absolute_read = co_await memory.read_absolute(
        0x1120, addressed_readback, AccessPath::Backdoor);
    passed &= expect(
        "relative byte offsets and absolute addresses select the same chunk",
        offset_write.transfers_completed == 4 &&
            absolute_read.transfers_completed == 4 &&
            addressed_readback == addressed_values &&
            memory.base_address() == 0x1100 &&
            memory.end_address() == 0x1140 && memory.element_bytes() == 4 &&
            memory.index_from_offset(0x20) == 8 &&
            memory.index_from_absolute(0x1120) == 8 &&
            memory.contains_absolute(0x1120) &&
            !memory.contains_absolute(0x1121) &&
            !memory.contains_absolute(0x1140));

    const std::array<uint32_t, 4> raw_addressed_values{
        0x91, 0xa2, 0xb3, 0xc4};
    memory.poke_absolute(0x1130, raw_addressed_values);
    memory.peek_offset_into(0x30, addressed_readback);
    passed &= expect(
        "raw absolute and relative chunk backdoors share entry mapping",
        addressed_readback == raw_addressed_values &&
            memory.peek_absolute(0x1138) == 0xb3 &&
            memory.peek_offset(0x3c) == 0xc4);

    auto window = memory.slice(8, backdoor_values.size());
    std::array<uint32_t, 4> window_readback{};
    window.poke(backdoor_values);
    window.peek_into(window_readback);
    passed &= expect(
        "memory slice preserves relative indexing and path metadata",
        window_readback == backdoor_values && window.peek(2) == 0x30 &&
            window.first_index() == 8 && window.size() == 4 &&
            window.address() == 0x1120 && window.path() == "block.buffer" &&
            window.hdl_path() == "u_block.buffer_storage" &&
            memory.name() == "buffer" && memory.size() == 16 &&
            memory.width() == 32);

    const auto window_write =
        co_await window.write(backdoor_values, AccessPath::Backdoor);
    const auto window_read =
        co_await window.read_into(window_readback, AccessPath::Backdoor);
    passed &= expect("memory slice keeps semantic access-path selection",
                     window_write.transfers_completed == 4 &&
                         window_read.transfers_completed == 4 &&
                         window_readback == backdoor_values);

    auto immediate_write =
        memory.write(12, 0x1357'9bdf, AccessPath::Backdoor);
    auto immediate_write_awaiter =
        std::move(immediate_write).operator co_await();
    const bool write_ready = immediate_write_awaiter.await_ready();
    const auto immediate_write_response =
        immediate_write_awaiter.await_resume();
    auto immediate_read = memory.read(12, AccessPath::Backdoor);
    auto immediate_read_awaiter = std::move(immediate_read).operator co_await();
    const bool read_ready = immediate_read_awaiter.await_ready();
    const auto immediate_read_response = immediate_read_awaiter.await_resume();
    passed &= expect(
        "backdoor memory operations complete without child coroutines",
        write_ready && read_ready && immediate_write_response.okay() &&
            immediate_read_response.okay() &&
            immediate_read_response.data == 0x1357'9bdf);
}

cpptb::coro::Task<void> exercise_wide_register_memory(
    cpptb::vc::WideRegisterMemoryHandle<128, FakeMaster>& memory,
    FakeMaster& master, FakeWideMemoryBackdoor& backdoor, bool& passed) {
    using cpptb::vc::AccessPath;
    using cpptb::vc::MemoryStatus;
    using Wide = cpptb::Bits<128>;

    const auto value =
        Wide::from_hex("0x112233445566778899aabbccddeeff00");
    const auto writes_before = master.writes.size();
    const auto write = co_await memory.write(1, value);
    passed &= expect(
        "wide memory frontdoor splits one element into bus transfers",
        write.okay() && write.transfers_completed == 4 &&
            master.writes.size() == writes_before + 4 &&
            master.writes[writes_before + 0].address == 0x1410 &&
            master.writes[writes_before + 0].data == 0xddeeff00 &&
            master.writes[writes_before + 3].address == 0x141c &&
            master.writes[writes_before + 3].data == 0x11223344);

    master.read_responses = {
        {.data = 0x7654'3210, .status = MemoryStatus::Okay},
        {.data = 0xfedc'ba98, .status = MemoryStatus::Okay},
        {.data = 0x0123'4567, .status = MemoryStatus::Okay},
        {.data = 0x89ab'cdef, .status = MemoryStatus::Okay},
    };
    const auto read = co_await memory.read(1);
    passed &= expect(
        "wide memory frontdoor assembles every transfer",
        read.okay() && read.transfers_completed == 4 &&
            read.data == Wide::from_hex(
                             "0x89abcdef01234567fedcba9876543210"));

    const std::array values{
        Wide::from_hex("0x11112222333344445555666677778888"),
        Wide::from_hex("0x9999aaaabbbbccccddddeeeeffff0000")};
    const auto backdoor_write = co_await memory.write(
        1, std::span<const Wide>{values}, AccessPath::Backdoor);
    std::array<Wide, 2> readback{};
    const auto backdoor_read = co_await memory.read_into(
        1, std::span<Wide>{readback}, AccessPath::Backdoor);
    passed &= expect(
        "wide memory bulk backdoor preserves complete elements",
        backdoor_write.transfers_completed == 2 &&
            backdoor_read.transfers_completed == 2 && readback == values &&
            backdoor.last_path == "block.wide_buffer" &&
            backdoor.last_index == 2 && backdoor.last_address == 0x1420);

    auto window = memory.slice(1, 2);
    std::array<Wide, 2> window_readback{};
    window.peek_into(window_readback);
    passed &= expect(
        "wide memory slice and addressing mirror the narrow API",
        window_readback == values && window.address() == 0x1410 &&
            window.path() == "block.wide_buffer" &&
            memory.index_from_offset(0x20) == 2 &&
            memory.index_from_absolute(0x1420) == 2 &&
            memory.contains_absolute(0x1420) &&
            !memory.contains_absolute(0x1424) && memory.has_backdoor());
}

cpptb::coro::Task<void> exercise_memory_path_errors(
    cpptb::vc::RegisterMemoryHandle<FakeMaster>& no_backdoor,
    cpptb::vc::RegisterMemoryHandle<FakeMaster>& split,
    FakeMemoryBackdoor& backdoor, bool& passed) {
    bool missing_backdoor = false;
    try {
        static_cast<void>(co_await no_backdoor.read(
            0, cpptb::vc::AccessPath::Backdoor));
    } catch (const std::logic_error& error) {
        missing_backdoor =
            std::string_view{error.what()}.find("block.buffer") !=
            std::string_view::npos;
    }
    passed &= expect("semantic memory backdoor diagnostic includes path",
                     missing_backdoor);

    bool split_frontdoor = false;
    try {
        static_cast<void>(co_await split.read(0));
    } catch (const std::logic_error& error) {
        split_frontdoor =
            std::string_view{error.what()}.find("block.split_buffer") !=
            std::string_view::npos;
    }
    const auto split_backdoor = co_await split.write(
        0, 0x55aa'1234, cpptb::vc::AccessPath::Backdoor);
    passed &= expect("split-width memory rejects only frontdoor operations",
                     split_frontdoor && split_backdoor.okay() &&
                         backdoor.values[0] == 0x55aa'1234);
}

}  // namespace

int main() {
    bool passed = true;
    passed &= exercise_write_effect_algebra();
    cpptb::coro::Testbench scheduler;
    cpptb::TestResult result;
    cpptb::TestContext test{scheduler, result};
    FakeMaster master;
    FakeBackdoor backdoor;
    passed &= exercise_descriptor_validation(test, master);
    passed &= exercise_register_predictor(test, result, master);
    passed &= exercise_wide_register_predictor(test, result, master);
    FakeMaster map_master;
    scheduler.spawn_detached(
        exercise_register_address_maps(test, map_master, passed));
    passed &= expect("register address-map sequence completes",
                     scheduler.done());
    FakeMaster user_effect_master;
    scheduler.spawn_detached(
        exercise_user_effect_policy(test, user_effect_master, passed));
    passed &= expect("register user-effect sequence completes",
                     scheduler.done());
    FakeMaster passive_prediction_master;
    scheduler.spawn_detached(exercise_passive_prediction_mode(
        test, passive_prediction_master, passed));
    passed &= expect("passive prediction ownership sequence completes",
                     scheduler.done());
    cpptb::vc::RegisterHandle command{test, master, kCommand, 0x1000,
                                      &backdoor};

    scheduler.spawn_detached(exercise_register(command, master, passed));
    passed &= expect("register sequence completes", scheduler.done());

    backdoor.value = 0x1234'5678;
    passed &= expect("peek uses the replaceable backdoor adapter",
                     command.peek() == 0x1234'5678 &&
                         command.mirrored() == 0x1234'5678 &&
                         backdoor.last_path == "block.command" &&
                         backdoor.last_address == 0x1020);
    command.poke(0xa5a5'5a5a);
    passed &= expect("poke updates DUT and model state",
                     backdoor.value == 0xa5a5'5a5a &&
                         command.mirrored() == 0xa5a5'5a5a &&
                         backdoor.last_address == 0x1020);

    cpptb::vc::RegisterHandle no_backdoor{test, master, kCommand};
    bool missing_backdoor_reported = false;
    try {
        static_cast<void>(no_backdoor.peek());
    } catch (const std::logic_error& error) {
        missing_backdoor_reported =
            std::string_view{error.what()}.find("block.command") !=
            std::string_view::npos;
    }
    passed &= expect("missing backdoor has contextual diagnostic",
                     missing_backdoor_reported);

    cpptb::vc::RegisterHandle fieldless{test, master, kFieldless};
    scheduler.spawn_detached(exercise_fieldless(fieldless, master, passed));
    passed &= expect("fieldless register sequence completes", scheduler.done());

    cpptb::vc::RegisterHandle partial{test, master, kPartialReset};
    cpptb::vc::RegisterHandle unknown_fieldless{test, master,
                                                kUnknownFieldless};
    cpptb::vc::RegisterHandle unknown_pair{test, master, kUnknownPair};
    scheduler.spawn_detached(exercise_prediction_validity(
        partial, unknown_fieldless, unknown_pair, master, result, passed));
    passed &= expect("prediction-validity sequence completes", scheduler.done());

    cpptb::vc::RegisterHandle mixed{test, master, kMixed};
    scheduler.spawn_detached(
        exercise_mixed_access(mixed, master, result, passed));
    passed &= expect("mixed-access register sequence completes",
                     scheduler.done());

    cpptb::vc::RegisterHandle set_only{test, master, kSetOnly};
    scheduler.spawn_detached(
        exercise_unreachable_update(set_only, result, passed));
    passed &= expect("unreachable update sequence completes", scheduler.done());

    cpptb::vc::RegisterHandle split{test, master, kSplitRegister};
    cpptb::vc::RegisterHandle big_split{test, master,
                                       kBigEndianSplitRegister};
    cpptb::vc::RegisterHandle wide{test, master, kWideRegister};
    bool wide_diagnostic = false;
    scheduler.spawn_detached(
        exercise_split_registers(split, big_split, master, passed));
    passed &= expect("split-register sequence completes", scheduler.done());
    try {
        static_cast<void>(wide.desired());
    } catch (const std::logic_error& error) {
        wide_diagnostic = std::string_view{error.what()}.find("block.wide") !=
                          std::string_view::npos;
    }
    passed &= expect("unsupported wide registers do not poison construction",
                     wide_diagnostic);

    FakeWideBackdoor wide_backdoor;
    cpptb::vc::WideRegisterHandle<128, FakeMaster> wide128{
        test, master, kWide128Register, &wide_backdoor};
    scheduler.spawn_detached(exercise_wide_register(wide128, master, passed));
    passed &= expect("arbitrary-width register sequence completes",
                     scheduler.done());
    passed &= expect("wide backdoor records descriptor context",
                     wide_backdoor.last_path == "block.wide128" &&
                         wide_backdoor.last_address == 0x80);

    bool missing_wide_backdoor_reported = false;
    cpptb::vc::WideRegisterHandle<128, FakeMaster> no_wide_backdoor{
        test, master, kWide128Register};
    try {
        static_cast<void>(no_wide_backdoor.peek());
    } catch (const std::logic_error& error) {
        missing_wide_backdoor_reported =
            std::string_view{error.what()}.find("block.wide128") !=
            std::string_view::npos;
    }
    passed &= expect("missing wide backdoor diagnostic includes path",
                     missing_wide_backdoor_reported);

    cpptb::vc::WideRegisterHandle<128, FakeMaster> wide128_write_once{
        test, master, kWide128WriteOnceRegister};
    cpptb::vc::WideRegisterHandle<128, FakeMaster> wide128_set{
        test, master, kWide128SetRegister};
    scheduler.spawn_detached(exercise_wide_register_policies(
        wide128_write_once, wide128_set, result, passed));
    passed &= expect("arbitrary-width register policies complete",
                     scheduler.done());

    command.reset();
    scheduler.spawn_detached(exercise_mirror(command, master, result, passed));
    passed &= expect("mirror sequence completes", scheduler.done());

    cpptb::vc::RegisterHandle write_once{test, master, kWriteOnce};
    bool second_write_rejected = false;
    scheduler.spawn_detached(
        exercise_write_once(write_once, second_write_rejected));
    passed &= expect("write-once policy rejects a second write",
                     scheduler.done() && second_write_rejected);

    cpptb::vc::RegisterHandle split_write_once{
        test, master, kSplitWriteOnceRegister};
    scheduler.spawn_detached(
        exercise_split_write_once(split_write_once, passed));
    passed &= expect("split write-once sequence completes", scheduler.done());

    FakeMemoryBackdoor memory_backdoor;
    cpptb::vc::RegisterMemoryHandle buffer{master, kBuffer, 0x1000,
                                           &memory_backdoor};
    scheduler.spawn_detached(
        exercise_register_memory(buffer, master, memory_backdoor, passed));
    passed &= expect("register memory sequence completes", scheduler.done());

    FakeWideMemoryBackdoor wide_memory_backdoor;
    cpptb::vc::WideRegisterMemoryHandle<128, FakeMaster> wide_buffer{
        master, kWideBuffer, 0x1100, &wide_memory_backdoor};
    scheduler.spawn_detached(exercise_wide_register_memory(
        wide_buffer, master, wide_memory_backdoor, passed));
    passed &= expect("wide register memory sequence completes",
                     scheduler.done());
    bool missing_wide_memory_backdoor = false;
    cpptb::vc::WideRegisterMemoryHandle<128, FakeMaster> no_wide_memory_backdoor{
        master, kWideBuffer};
    try {
        static_cast<void>(no_wide_memory_backdoor.peek(0));
    } catch (const std::logic_error& error) {
        missing_wide_memory_backdoor =
            std::string_view{error.what()}.find("block.wide_buffer") !=
            std::string_view::npos;
    }
    passed &= expect("missing wide memory backdoor diagnostic includes path",
                     missing_wide_memory_backdoor);

    bool bad_index_reported = false;
    try {
        static_cast<void>(buffer.address(16));
    } catch (const std::out_of_range& error) {
        bad_index_reported =
            std::string_view{error.what()}.find("block.buffer") !=
            std::string_view::npos;
    }
    passed &= expect("register memory bounds error includes path",
                     bad_index_reported);

    cpptb::vc::RegisterMemoryHandle read_only_buffer{
        master, kReadOnlyBuffer, 0, &memory_backdoor};
    cpptb::vc::RegisterMemoryHandle write_only_buffer{
        master, kWriteOnlyBuffer, 0, &memory_backdoor};
    scheduler.spawn_detached(exercise_memory_access_errors(
        read_only_buffer, write_only_buffer, passed));
    passed &= expect("register memory access checks complete", scheduler.done());
    read_only_buffer.poke(0, 0x1234'5678);
    passed &= expect("raw poke bypasses semantic read-only policy",
                     read_only_buffer.peek(0) == 0x1234'5678);
    write_only_buffer.poke(1, 0x89ab'cdef);
    passed &= expect("raw peek bypasses semantic write-only policy",
                     write_only_buffer.peek(1) == 0x89ab'cdef);

    cpptb::vc::RegisterMemoryHandle split_buffer{
        master, kSplitBuffer, 0, &memory_backdoor};
    cpptb::vc::RegisterMemoryHandle no_memory_backdoor{master, kBuffer};
    scheduler.spawn_detached(exercise_memory_path_errors(
        no_memory_backdoor, split_buffer, memory_backdoor, passed));
    passed &= expect("memory path diagnostics complete", scheduler.done());

    bool bad_range_reported = false;
    try {
        std::array<uint32_t, 2> values{};
        buffer.peek_into(15, values);
    } catch (const std::out_of_range& error) {
        bad_range_reported =
            std::string_view{error.what()}.find("block.buffer") !=
            std::string_view::npos;
    }
    passed &= expect("register memory range error includes path",
                     bad_range_reported);

    bool bad_slice_size_reported = false;
    try {
        auto window = buffer.slice(4, 3);
        std::array<uint32_t, 2> values{};
        window.peek_into(values);
    } catch (const std::invalid_argument& error) {
        bad_slice_size_reported =
            std::string_view{error.what()}.find("block.buffer") !=
            std::string_view::npos;
    }
    passed &= expect("register memory slice errors include path",
                     bad_slice_size_reported);

    bool unaligned_offset_reported = false;
    try {
        static_cast<void>(buffer.read_offset(
            3, cpptb::vc::AccessPath::Backdoor));
    } catch (const std::invalid_argument& error) {
        const std::string_view diagnostic{error.what()};
        unaligned_offset_reported =
            diagnostic.find("block.buffer") != std::string_view::npos &&
            diagnostic.find("offset=3") != std::string_view::npos;
    }
    passed &= expect("unaligned relative address diagnostic includes context",
                     unaligned_offset_reported);

    bool unaligned_absolute_reported = false;
    try {
        static_cast<void>(buffer.read_absolute(
            0x1102, cpptb::vc::AccessPath::Backdoor));
    } catch (const std::invalid_argument& error) {
        const std::string_view diagnostic{error.what()};
        unaligned_absolute_reported =
            diagnostic.find("block.buffer") != std::string_view::npos &&
            diagnostic.find("address=4354") != std::string_view::npos;
    }
    passed &= expect("unaligned absolute address diagnostic includes context",
                     unaligned_absolute_reported);

    bool absolute_range_reported = false;
    try {
        static_cast<void>(buffer.read_absolute(
            0x10ff, cpptb::vc::AccessPath::Backdoor));
    } catch (const std::out_of_range& error) {
        const std::string_view diagnostic{error.what()};
        absolute_range_reported =
            diagnostic.find("block.buffer") != std::string_view::npos &&
            diagnostic.find("address=4351") != std::string_view::npos;
    }
    passed &= expect("absolute address diagnostic includes context",
                     absolute_range_reported);

    return passed ? 0 : 1;
}
