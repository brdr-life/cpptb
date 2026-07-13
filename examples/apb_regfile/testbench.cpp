#include "examples/apb_regfile/framework.hpp"

namespace cpptb::examples::apb_regfile {
namespace {

using coro::Delay;
using coro::FallingEdge;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

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
    explicit ApbMaster(ApbRegfileTb tb) : tb_(tb) {}

    Task<void> write(uint32_t address, uint32_t data) const {
        co_await FallingEdge{tb_.dut.clk};
        tb_.dut.apb.address.set(address);
        tb_.dut.apb.write_data.set(data);
        tb_.dut.apb.write.set(1);
        tb_.dut.apb.select.set(1);
        tb_.dut.apb.enable.set(0);

        co_await RisingEdge{tb_.dut.clk};
        co_await FallingEdge{tb_.dut.clk};
        tb_.dut.apb.enable.set(1);

        co_await RisingEdge{tb_.dut.clk};
        co_await Delay{1_ps};
        tb_.expect_eq("APB write ready", tb_.dut.apb.ready.get(), 1);

        co_await FallingEdge{tb_.dut.clk};
        idle();
    }

    Task<ApbReadResult> read_with_status(uint32_t address) const {
        co_await FallingEdge{tb_.dut.clk};
        tb_.dut.apb.address.set(address);
        tb_.dut.apb.write.set(0);
        tb_.dut.apb.select.set(1);
        tb_.dut.apb.enable.set(0);

        co_await RisingEdge{tb_.dut.clk};
        co_await FallingEdge{tb_.dut.clk};
        tb_.dut.apb.enable.set(1);

        co_await RisingEdge{tb_.dut.clk};
        co_await Delay{1_ps};
        tb_.expect_eq("APB read ready", tb_.dut.apb.ready.get(), 1);
        const ApbReadResult result{tb_.dut.apb.read_data.get(),
                                   tb_.dut.apb.error.get()};

        co_await FallingEdge{tb_.dut.clk};
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
        tb_.expect_eq(label, actual, expected);
    }

    Task<void> read_error_expect(const char* label, uint32_t address,
                                 uint32_t expected_data) const {
        const auto result = co_await read_with_status(address);
        tb_.expect_eq(label, result.data, expected_data);
        tb_.expect_eq("APB error asserted", result.error, 1);
    }

    void idle() const {
        tb_.dut.apb.select.set(0);
        tb_.dut.apb.enable.set(0);
        tb_.dut.apb.write.set(0);
    }

   private:
    ApbRegfileTb tb_;
};

Task<void> reset_dut(ApbRegfileTb tb) {
    tb.dut.rst_n.set(0);
    tb.dut.apb.select.set(0);
    tb.dut.apb.enable.set(0);
    tb.dut.apb.write.set(0);
    tb.dut.apb.address.set(0);
    tb.dut.apb.write_data.set(0);

    co_await clock_cycles(tb.dut.clk, 2);
    co_await FallingEdge{tb.dut.clk};
    tb.dut.rst_n.set(1);
}

Task<void> register_sequence(ApbRegfileTb tb) {
    co_await reset_dut(tb);
    const ApbMaster apb{tb};
    uint32_t state = 0x1020'3040u;

    for (uint32_t index = 0; index < tb.iterations(); ++index) {
        const uint32_t address = (index % 4u) * 4u;
        const uint32_t value = next_word(state);
        co_await apb.write(address, value);
        co_await apb.read_expect("APB register readback", address, value);
    }

    co_await apb.read_expect("APB read-only ID", kIdAddress, kIdValue);
    co_await apb.read_error_expect("APB unmapped read data", 0xfcu, 0);
}

}  // namespace

void register_user_testbench(ApbRegfileTb& tb) {
    tb.run(register_sequence(tb));
}

}  // namespace cpptb::examples::apb_regfile
