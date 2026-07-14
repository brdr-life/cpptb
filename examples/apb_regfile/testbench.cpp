#include <cstdint>

#include "cpptb/cpptb.hpp"
#include "examples/apb_regfile/generated/apb_regfile_dut.hpp"

namespace cpptb::examples::apb_regfile {
namespace {

using cpptb::generated::apb_regfile::Dut;
using coro::Delay;
using coro::FallingEdge;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

constexpr uint32_t kRegisterTransactions = 12;
constexpr uint32_t kIdAddress = 0x10u;
constexpr uint32_t kIdValue = 0x4350'5054u;

uint32_t next_word(uint32_t& state) {
    state = state * 1'664'525u + 1'013'904'223u;
    return state;
}

struct ApbReadResult {
    uint32_t data;
    uint32_t error;
};

class ApbMaster {
   public:
    ApbMaster(Dut dut, TestContext& test) : dut_(dut), test_(&test) {}

    Task<void> write(uint32_t address, uint32_t data) const {
        co_await FallingEdge{dut_.clk};
        dut_.apb_address.set(address);
        dut_.apb_write_data.set(data);
        dut_.apb_write.set(1);
        dut_.apb_select.set(1);
        dut_.apb_enable.set(0);

        co_await RisingEdge{dut_.clk};
        co_await FallingEdge{dut_.clk};
        dut_.apb_enable.set(1);

        co_await RisingEdge{dut_.clk};
        co_await Delay{1_ps};
        test_->expect_eq("APB write ready", dut_.apb_ready.get(), 1u);

        co_await FallingEdge{dut_.clk};
        idle();
    }

    Task<ApbReadResult> read_with_status(uint32_t address) const {
        co_await FallingEdge{dut_.clk};
        dut_.apb_address.set(address);
        dut_.apb_write.set(0);
        dut_.apb_select.set(1);
        dut_.apb_enable.set(0);

        co_await RisingEdge{dut_.clk};
        co_await FallingEdge{dut_.clk};
        dut_.apb_enable.set(1);

        co_await RisingEdge{dut_.clk};
        co_await Delay{1_ps};
        test_->expect_eq("APB read ready", dut_.apb_ready.get(), 1u);
        const ApbReadResult result{dut_.apb_read_data.get(),
                                   dut_.apb_error.get()};

        co_await FallingEdge{dut_.clk};
        idle();
        co_return result;
    }

    Task<uint32_t> read(uint32_t address) const {
        const auto result = co_await read_with_status(address);
        co_return result.data;
    }

    Task<void> read_expect(const char* label, uint32_t address,
                           uint32_t expected) const {
        const uint32_t actual = co_await read(address);
        test_->expect_eq(label, actual, expected);
    }

    Task<void> read_error_expect(const char* label, uint32_t address,
                                 uint32_t expected_data) const {
        const auto result = co_await read_with_status(address);
        test_->expect_eq(label, result.data, expected_data);
        test_->expect_eq("APB error asserted", result.error, 1u);
    }

    void idle() const {
        dut_.apb_select.set(0);
        dut_.apb_enable.set(0);
        dut_.apb_write.set(0);
    }

   private:
    Dut dut_;
    TestContext* test_;
};

Task<void> reset_dut(Dut dut) {
    dut.rst_n.set(0);
    dut.apb_select.set(0);
    dut.apb_enable.set(0);
    dut.apb_write.set(0);
    dut.apb_address.set(0);
    dut.apb_write_data.set(0);

    co_await clock_cycles(dut.clk, 2);
    co_await FallingEdge{dut.clk};
    dut.rst_n.set(1);
}

Task<void> register_sequence(Dut dut, TestContext& test) {
    dut.clk.set(0);
    test.start_clock(dut.clk, 10_ns);

    co_await reset_dut(dut);
    const ApbMaster apb{dut, test};
    uint32_t state = 0x1020'3040u;

    for (uint32_t index = 0; index < kRegisterTransactions; ++index) {
        const uint32_t address = (index % 4u) * 4u;
        const uint32_t value = next_word(state);
        co_await apb.write(address, value);
        co_await apb.read_expect("APB register readback", address, value);
    }

    co_await apb.read_expect("APB read-only ID", kIdAddress, kIdValue);
    co_await apb.read_error_expect("APB unmapped read data", 0xfcu, 0u);
}

CPPTB_REGISTER_TEST(register_sequence);

}  // namespace
}  // namespace cpptb::examples::apb_regfile
