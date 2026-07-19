#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "cpptb_vc/register_sequences.hpp"

namespace {

using cpptb::coro::Task;
using cpptb::vc::AccessPath;
using cpptb::vc::MemoryReadRequest;
using cpptb::vc::MemoryReadResponse;
using cpptb::vc::MemoryWriteRequest;
using cpptb::vc::MemoryWriteResponse;
using cpptb::vc::RegisterAccess;
using cpptb::vc::RegisterBackdoor;
using cpptb::vc::RegisterDescriptor;
using cpptb::vc::RegisterFieldDescriptor;
using cpptb::vc::RegisterHandle;
using cpptb::vc::RegisterWriteEffect;

bool expect(std::string_view label, bool condition) {
    if (condition) return true;
    std::fprintf(stderr, "register sequence test failed: %.*s\n",
                 static_cast<int>(label.size()), label.data());
    return false;
}

struct FakeMaster {
    using address_type = uint32_t;
    using data_type = uint32_t;
    using byte_enable_type = uint8_t;
    using write_request_type =
        MemoryWriteRequest<address_type, data_type, byte_enable_type>;
    using read_request_type = MemoryReadRequest<address_type>;
    using write_response_type = MemoryWriteResponse;
    using read_response_type = MemoryReadResponse<data_type>;

    Task<write_response_type> write(write_request_type request) {
        storage.at(request.address / 4u) = request.data;
        ++writes;
        co_return write_response_type{};
    }

    Task<read_response_type> read(read_request_type request) {
        ++reads;
        co_return read_response_type{.data = storage.at(request.address / 4u)};
    }

    std::array<uint32_t, 4> storage{};
    uint64_t reads = 0;
    uint64_t writes = 0;
};

class FakeBackdoor final : public RegisterBackdoor<uint64_t> {
   public:
    explicit FakeBackdoor(FakeMaster& master) : master_(&master) {}

    uint64_t peek(const RegisterDescriptor&, uint64_t address) override {
        return master_->storage.at(address / 4u);
    }

    void poke(const RegisterDescriptor&, uint64_t address,
              uint64_t value) override {
        master_->storage.at(address / 4u) = static_cast<uint32_t>(value);
    }

   private:
    FakeMaster* master_;
};

constexpr std::array kNormalFields{
    RegisterFieldDescriptor{.name = "value",
                            .path = "demo.normal.value",
                            .width = 8,
                            .access = RegisterAccess::ReadWrite,
                            .reset_value = 0x5a,
                            .reset_mask = 0xff},
};

constexpr std::array kReadOnlyFields{
    RegisterFieldDescriptor{.name = "value",
                            .path = "demo.read_only.value",
                            .width = 8,
                            .access = RegisterAccess::ReadOnly,
                            .reset_value = 0xa5,
                            .reset_mask = 0xff},
};

constexpr std::array kSideEffectFields{
    RegisterFieldDescriptor{.name = "pending",
                            .path = "demo.pending.pending",
                            .width = 8,
                            .access = RegisterAccess::ReadWrite,
                            .write_effect =
                                RegisterWriteEffect::WriteOneClear,
                            .reset_value = 0xff,
                            .reset_mask = 0xff},
};

constexpr std::array kVolatileFields{
    RegisterFieldDescriptor{.name = "sampled",
                            .path = "demo.volatile_value.sampled",
                            .width = 8,
                            .access = RegisterAccess::ReadOnly,
                            .reset_value = 0x33,
                            .reset_mask = 0xff,
                            .volatile_value = true},
};

constexpr RegisterDescriptor kNormal{
    .name = "normal",
    .path = "demo.normal",
    .address = 0,
    .width = 32,
    .access_width = 32,
    .reset_value = 0x5a,
    .reset_mask = 0xff,
    .fields = kNormalFields,
};

constexpr RegisterDescriptor kReadOnly{
    .name = "read_only",
    .path = "demo.read_only",
    .address = 4,
    .width = 32,
    .access_width = 32,
    .reset_value = 0xa5,
    .reset_mask = 0xff,
    .fields = kReadOnlyFields,
};

constexpr RegisterDescriptor kSideEffect{
    .name = "pending",
    .path = "demo.pending",
    .address = 8,
    .width = 32,
    .access_width = 32,
    .reset_value = 0xff,
    .reset_mask = 0xff,
    .fields = kSideEffectFields,
};

constexpr RegisterDescriptor kVolatile{
    .name = "volatile_value",
    .path = "demo.volatile_value",
    .address = 12,
    .width = 32,
    .access_width = 32,
    .reset_value = 0x33,
    .reset_mask = 0xff,
    .fields = kVolatileFields,
};

class FakeModel {
   public:
    FakeModel(cpptb::TestContext test, FakeMaster& master,
              FakeBackdoor* backdoor)
        : normal(test, master, kNormal, backdoor),
          read_only(test, master, kReadOnly, backdoor),
          side_effect(test, master, kSideEffect, backdoor),
          volatile_value(test, master, kVolatile, backdoor) {}

    template <typename Function>
    Task<void> for_each_register_async(Function& function) {
        co_await function(normal);
        co_await function(read_only);
        co_await function(side_effect);
        co_await function(volatile_value);
    }

    RegisterHandle<FakeMaster> normal;
    RegisterHandle<FakeMaster> read_only;
    RegisterHandle<FakeMaster> side_effect;
    RegisterHandle<FakeMaster> volatile_value;
};

void load_reset(FakeMaster& master) {
    master.storage = {0x5a, 0xa5, 0xff, 0x33};
}

Task<void> exercise_sequences(cpptb::TestContext test, FakeModel& model,
                              FakeMaster& master, bool& passed) {
    load_reset(master);
    const auto frontdoor_reset =
        co_await cpptb::vc::register_reset_check(test, model);
    passed &= expect(
        "frontdoor reset summary",
        frontdoor_reset.registers_visited == 4 &&
            frontdoor_reset.registers_tested == 3 &&
            frontdoor_reset.registers_skipped == 1 &&
            frontdoor_reset.frontdoor_reads == 3);

    load_reset(master);
    const auto backdoor_reset = co_await cpptb::vc::register_reset_check(
        test, model, {.path = AccessPath::Backdoor});
    passed &= expect(
        "backdoor reset summary",
        backdoor_reset.registers_tested == 3 &&
            backdoor_reset.backdoor_reads == 3 &&
            backdoor_reset.frontdoor_reads == 0);

    load_reset(master);
    const auto access =
        co_await cpptb::vc::register_access_check(test, model);
    passed &= expect(
        "mixed access summary",
        access.registers_visited == 4 && access.registers_tested == 3 &&
            access.registers_skipped == 1 && access.frontdoor_reads == 3 &&
            access.frontdoor_writes == 1 && access.backdoor_reads == 4 &&
            access.backdoor_writes == 6);
    passed &= expect("access restores storage",
                     master.storage ==
                         std::array<uint32_t, 4>{0x5a, 0xa5, 0xff, 0x33});

    load_reset(master);
    const auto frontdoor_bash =
        co_await cpptb::vc::register_bit_bash(test, model);
    passed &= expect(
        "frontdoor bit-bash summary",
        frontdoor_bash.registers_tested == 1 &&
            frontdoor_bash.registers_skipped == 3 &&
            frontdoor_bash.bits_tested == 8 &&
            frontdoor_bash.frontdoor_reads == 9 &&
            frontdoor_bash.frontdoor_writes == 9);
    passed &= expect("frontdoor bit bash restores storage",
                     master.storage[0] == 0x5a);

    load_reset(master);
    const auto backdoor_bash = co_await cpptb::vc::register_bit_bash(
        test, model, {.path = AccessPath::Backdoor});
    passed &= expect(
        "backdoor bit-bash summary",
        backdoor_bash.registers_tested == 1 &&
            backdoor_bash.bits_tested == 8 &&
            backdoor_bash.backdoor_reads == 9 &&
            backdoor_bash.backdoor_writes == 9);
    passed &= expect("backdoor bit bash restores storage",
                     master.storage[0] == 0x5a);
}

Task<void> exercise_missing_backdoor(cpptb::TestContext test,
                                     FakeModel& model, bool& reported) {
    try {
        static_cast<void>(
            co_await cpptb::vc::register_access_check(test, model));
    } catch (const std::logic_error& error) {
        const std::string_view diagnostic{error.what()};
        reported = diagnostic.find("register access check") !=
                       std::string_view::npos &&
                   diagnostic.find("demo.normal") != std::string_view::npos;
    }
}

}  // namespace

int main() {
    cpptb::coro::Testbench scheduler;
    cpptb::TestResult result;
    cpptb::TestContext test{scheduler, result};
    FakeMaster master;
    FakeBackdoor backdoor{master};
    FakeModel model{test, master, &backdoor};
    bool passed = true;
    scheduler.spawn_detached(exercise_sequences(test, model, master, passed));
    passed &= expect("sequence task completes", scheduler.done());
    passed &= expect("sequence checks pass", result.failures == 0);

    cpptb::coro::Testbench missing_scheduler;
    cpptb::TestResult missing_result;
    cpptb::TestContext missing_test{missing_scheduler, missing_result};
    FakeMaster missing_master;
    FakeModel missing_model{missing_test, missing_master, nullptr};
    bool missing_reported = false;
    missing_scheduler.spawn_detached(exercise_missing_backdoor(
        missing_test, missing_model, missing_reported));
    passed &= expect("missing-backdoor task completes",
                     missing_scheduler.done());
    passed &= expect("missing backdoor includes sequence and path",
                     missing_reported);
    return passed ? 0 : 1;
}
