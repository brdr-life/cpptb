#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "cpptb/cpptb.hpp"
#include "cpptb_vc/cpptb_vc.hpp"
#include "ipxact_regs.hpp"

namespace {

struct FakeMaster {
    using address_type = uint32_t;
    using data_type = uint32_t;
    using byte_enable_type = uint8_t;
    using write_request_type = cpptb::vc::MemoryWriteRequest<
        address_type, data_type, byte_enable_type>;
    using read_request_type = cpptb::vc::MemoryReadRequest<address_type>;
    using write_response_type = cpptb::vc::MemoryWriteResponse;
    using read_response_type = cpptb::vc::MemoryReadResponse<data_type>;

    cpptb::coro::Task<write_response_type> write(
        write_request_type request) {
        const auto index = request.address / sizeof(data_type);
        if (index >= storage.size()) {
            co_return write_response_type{
                .status = cpptb::vc::MemoryStatus::DecodeError};
        }
        storage[index] = request.data;
        ++writes;
        co_return write_response_type{};
    }

    cpptb::coro::Task<read_response_type> read(read_request_type request) {
        const auto index = request.address / sizeof(data_type);
        if (index >= storage.size()) {
            co_return read_response_type{
                .status = cpptb::vc::MemoryStatus::DecodeError};
        }
        ++reads;
        co_return read_response_type{.data = storage[index]};
    }

    std::array<uint32_t, 80> storage{};
    uint32_t writes = 0;
    uint32_t reads = 0;
};

static_assert(cpptb::vc::MemoryMappedMaster<FakeMaster>);

cpptb::coro::Task<void> exercise(
    ipxact_regs::RegModel<FakeMaster>& regs, FakeMaster& master,
    bool& passed) {
    const auto control = co_await regs.registers.control.write(0x5u);
    regs.registers.control.mode.stage(
        ipxact_regs::mode_enum_t::STREAM);
    const auto update = co_await regs.registers.control.update();
    const auto threshold =
        co_await regs.registers.threshold.at<2>().write(0x1234u);

    constexpr std::array<uint32_t, 3> words{
        0x1122'3344u, 0xa5a5'5a5au, 0xcafe'babeu};
    const auto memory_write = co_await regs.scratchpad.write(
        4, std::span<const uint32_t>{words});
    std::array<uint32_t, words.size()> readback{};
    const auto memory_read = co_await regs.scratchpad.read(
        4, std::span<uint32_t>{readback});

    passed = control.okay() && update.okay() && threshold.okay() &&
             memory_write.okay() && memory_read.okay() &&
             master.storage[0] == 0x3u && master.storage[0x28 / 4] == 0x1234u &&
             readback == words && master.writes == 6u && master.reads == 3u &&
             regs.registers.control.path() ==
                 std::string_view{"peripheral.registers.control"} &&
             regs.scratchpad.path() ==
                 std::string_view{"peripheral.scratchpad"};
}

}  // namespace

int main() {
    cpptb::coro::Testbench scheduler;
    cpptb::TestResult result;
    cpptb::TestContext test{scheduler, result};
    FakeMaster master;
    ipxact_regs::RegModel regs{test, master};
    bool passed = false;
    scheduler.spawn_detached(exercise(regs, master, passed));
    return scheduler.done() && passed ? 0 : 1;
}
