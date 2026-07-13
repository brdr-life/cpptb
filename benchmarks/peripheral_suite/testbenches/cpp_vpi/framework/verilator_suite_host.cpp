#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "Vvpi_peripheral_suite.h"
#include "benchmarks/peripheral_suite/testbenches/cpp_vpi/framework/peripheral_suite_bench.hpp"
#include "cpptb/coro_runtime.hpp"
#include "verilated.h"
#include "verilated_vpi.h"
#include "vpi_user.h"

namespace {

using cpptb::benchmarks::peripheral_suite::ApbBus;
using cpptb::benchmarks::peripheral_suite::PeripheralSuiteDut;
using cpptb::benchmarks::peripheral_suite::SignalId;
using cpptb::coro::EdgeKind;
using cpptb::coro::Signal;
using cpptb::coro::Testbench;
using cpptb::coro::make_vpi_signal;

uint64_t main_time = 0;
Testbench* tb = nullptr;
PeripheralSuiteDut dut;

struct SignalWatch {
    uint32_t signal_id = UINT32_MAX;
    vpiHandle callback = nullptr;
    s_vpi_time time{};
    s_vpi_value value{};
    s_cb_data callback_data{};
};

std::array<SignalWatch, cpptb::benchmarks::peripheral_suite::kSignalCount> watches;

uint32_t env_u32(const char* name, uint32_t default_value) {
    const char* value = std::getenv(name);
    if (!value || value[0] == '\0') return default_value;
    return static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
}

vpiHandle require_handle(const char* path) {
    auto* handle = vpi_handle_by_name(const_cast<PLI_BYTE8*>(path), nullptr);
    if (!handle) {
        std::fprintf(stderr, "peripheral-suite: missing VPI handle for %s\n", path);
        std::exit(1);
    }
    return handle;
}

Signal make_signal(const char* path, uint32_t id, const char* name) {
    return make_vpi_signal(require_handle(path), id, name);
}

Signal signal_by_id(uint32_t signal_id) {
    using namespace cpptb::benchmarks::peripheral_suite;

    switch (signal_id) {
        case kSignalHclk:
            return dut.HCLK;
        case kSignalHresetn:
            return dut.HRESETn;
        default:
            std::fprintf(stderr, "peripheral-suite: no edge lookup for signal id %u\n",
                         signal_id);
            std::exit(1);
    }
}

PLI_INT32 on_value_change(p_cb_data callback_data) {
    auto* watch = reinterpret_cast<SignalWatch*>(callback_data->user_data);
    if (!watch || !tb) return 0;

    const auto value = signal_by_id(watch->signal_id).get();
    tb->notify_edge(watch->signal_id,
                    value ? EdgeKind::Rising : EdgeKind::Falling);
    return 0;
}

void register_edge_callback(uint32_t signal_id) {
    if (signal_id >= watches.size()) {
        std::fprintf(stderr, "peripheral-suite: cannot watch signal id %u\n",
                     signal_id);
        std::exit(1);
    }

    auto& watch = watches[signal_id];
    if (watch.callback) return;

    watch.signal_id = signal_id;
    watch.time.type = vpiSimTime;
    watch.value.format = vpiIntVal;
    watch.callback_data.reason = cbValueChange;
    watch.callback_data.cb_rtn = on_value_change;
    watch.callback_data.obj = signal_by_id(signal_id).handle;
    watch.callback_data.time = &watch.time;
    watch.callback_data.value = &watch.value;
    watch.callback_data.user_data = reinterpret_cast<PLI_BYTE8*>(&watch);
    watch.callback = vpi_register_cb(&watch.callback_data);
    if (!watch.callback) {
        std::fprintf(stderr, "peripheral-suite: failed to register callback %u\n",
                     signal_id);
        std::exit(1);
    }
}

void settle(Vvpi_peripheral_suite* top) {
    top->eval();
    VerilatedVpi::callValueCbs();

    for (int i = 0; i < 8 && VerilatedVpi::evalNeeded(); ++i) {
        top->eval();
        VerilatedVpi::clearEvalNeeded();
        VerilatedVpi::callValueCbs();
    }
}

void update_time(uint64_t time) {
    main_time = time;
    if (tb) tb->set_time(main_time);
}

void set_clock(Vvpi_peripheral_suite* top, uint32_t value) {
    dut.HCLK.set(value);
    settle(top);
}

void tick(Vvpi_peripheral_suite* top) {
    set_clock(top, 0);
    update_time(main_time + 1);
    set_clock(top, 1);
    update_time(main_time + 1);
    set_clock(top, 0);
}

ApbBus bind_apb(const char* prefix, uint32_t paddr_id, uint32_t pwdata_id,
                uint32_t pwrite_id, uint32_t psel_id, uint32_t penable_id,
                uint32_t prdata_id, uint32_t pready_id, uint32_t pslverr_id) {
    char path[160];
    auto bind = [&](const char* suffix, uint32_t id) {
        std::snprintf(path, sizeof(path), "TOP.vpi_peripheral_suite.%s_%s", prefix,
                      suffix);
        return make_signal(path, id, suffix);
    };

    return ApbBus{
        bind("PADDR", paddr_id),     bind("PWDATA", pwdata_id),
        bind("PWRITE", pwrite_id),   bind("PSEL", psel_id),
        bind("PENABLE", penable_id), bind("PRDATA", prdata_id),
        bind("PREADY", pready_id),   bind("PSLVERR", pslverr_id),
    };
}

PeripheralSuiteDut bind_dut() {
    using namespace cpptb::benchmarks::peripheral_suite;

    const auto timer_apb = bind_apb("timer", kSignalTimerPaddr, kSignalTimerPwdata,
                                    kSignalTimerPwrite, kSignalTimerPsel,
                                    kSignalTimerPenable, kSignalTimerPrdata,
                                    kSignalTimerPready, kSignalTimerPslverr);
    const auto spi_apb = bind_apb("spi", kSignalSpiPaddr, kSignalSpiPwdata,
                                  kSignalSpiPwrite, kSignalSpiPsel,
                                  kSignalSpiPenable, kSignalSpiPrdata,
                                  kSignalSpiPready, kSignalSpiPslverr);
    const auto i2c_apb = bind_apb("i2c", kSignalI2cPaddr, kSignalI2cPwdata,
                                  kSignalI2cPwrite, kSignalI2cPsel,
                                  kSignalI2cPenable, kSignalI2cPrdata,
                                  kSignalI2cPready, kSignalI2cPslverr);

    return PeripheralSuiteDut{
        make_signal("TOP.vpi_peripheral_suite.HCLK", kSignalHclk, "HCLK"),
        make_signal("TOP.vpi_peripheral_suite.HRESETn", kSignalHresetn, "HRESETn"),
        {timer_apb,
         make_signal("TOP.vpi_peripheral_suite.timer_irq", kSignalTimerIrq,
                     "timer_irq")},
        {spi_apb,
         make_signal("TOP.vpi_peripheral_suite.spi_clk_div", kSignalSpiClkDiv,
                     "spi_clk_div"),
         make_signal("TOP.vpi_peripheral_suite.spi_clk_div_valid",
                     kSignalSpiClkDivValid, "spi_clk_div_valid"),
         make_signal("TOP.vpi_peripheral_suite.spi_status", kSignalSpiStatus,
                     "spi_status"),
         make_signal("TOP.vpi_peripheral_suite.spi_addr", kSignalSpiAddr,
                     "spi_addr"),
         make_signal("TOP.vpi_peripheral_suite.spi_addr_len", kSignalSpiAddrLen,
                     "spi_addr_len"),
         make_signal("TOP.vpi_peripheral_suite.spi_cmd", kSignalSpiCmd,
                     "spi_cmd"),
         make_signal("TOP.vpi_peripheral_suite.spi_cmd_len", kSignalSpiCmdLen,
                     "spi_cmd_len"),
         make_signal("TOP.vpi_peripheral_suite.spi_csreg", kSignalSpiCsreg,
                     "spi_csreg"),
         make_signal("TOP.vpi_peripheral_suite.spi_data_len", kSignalSpiDataLen,
                     "spi_data_len"),
         make_signal("TOP.vpi_peripheral_suite.spi_dummy_rd", kSignalSpiDummyRd,
                     "spi_dummy_rd"),
         make_signal("TOP.vpi_peripheral_suite.spi_dummy_wr", kSignalSpiDummyWr,
                     "spi_dummy_wr"),
         make_signal("TOP.vpi_peripheral_suite.spi_int_th_tx", kSignalSpiIntThTx,
                     "spi_int_th_tx"),
         make_signal("TOP.vpi_peripheral_suite.spi_int_th_rx", kSignalSpiIntThRx,
                     "spi_int_th_rx"),
         make_signal("TOP.vpi_peripheral_suite.spi_int_cnt_tx", kSignalSpiIntCntTx,
                     "spi_int_cnt_tx"),
         make_signal("TOP.vpi_peripheral_suite.spi_int_cnt_rx", kSignalSpiIntCntRx,
                     "spi_int_cnt_rx"),
         make_signal("TOP.vpi_peripheral_suite.spi_int_en", kSignalSpiIntEn,
                     "spi_int_en"),
         make_signal("TOP.vpi_peripheral_suite.spi_int_cnt_en",
                     kSignalSpiIntCntEn, "spi_int_cnt_en"),
         make_signal("TOP.vpi_peripheral_suite.spi_int_rd_sta",
                     kSignalSpiIntRdSta, "spi_int_rd_sta"),
         make_signal("TOP.vpi_peripheral_suite.spi_swrst", kSignalSpiSwrst,
                     "spi_swrst"),
         make_signal("TOP.vpi_peripheral_suite.spi_rd", kSignalSpiRd, "spi_rd"),
         make_signal("TOP.vpi_peripheral_suite.spi_wr", kSignalSpiWr, "spi_wr"),
         make_signal("TOP.vpi_peripheral_suite.spi_qrd", kSignalSpiQrd,
                     "spi_qrd"),
         make_signal("TOP.vpi_peripheral_suite.spi_qwr", kSignalSpiQwr,
                     "spi_qwr"),
         make_signal("TOP.vpi_peripheral_suite.spi_data_tx", kSignalSpiDataTx,
                     "spi_data_tx"),
         make_signal("TOP.vpi_peripheral_suite.spi_data_tx_valid",
                     kSignalSpiDataTxValid, "spi_data_tx_valid"),
         make_signal("TOP.vpi_peripheral_suite.spi_data_tx_ready",
                     kSignalSpiDataTxReady, "spi_data_tx_ready"),
         make_signal("TOP.vpi_peripheral_suite.spi_data_rx", kSignalSpiDataRx,
                     "spi_data_rx"),
         make_signal("TOP.vpi_peripheral_suite.spi_data_rx_valid",
                     kSignalSpiDataRxValid, "spi_data_rx_valid"),
         make_signal("TOP.vpi_peripheral_suite.spi_data_rx_ready",
                     kSignalSpiDataRxReady, "spi_data_rx_ready")},
        {i2c_apb,
         make_signal("TOP.vpi_peripheral_suite.i2c_interrupt", kSignalI2cInterrupt,
                     "i2c_interrupt"),
         make_signal("TOP.vpi_peripheral_suite.i2c_scl_pad_i", kSignalI2cSclPadI,
                     "i2c_scl_pad_i"),
         make_signal("TOP.vpi_peripheral_suite.i2c_scl_pad_o", kSignalI2cSclPadO,
                     "i2c_scl_pad_o"),
         make_signal("TOP.vpi_peripheral_suite.i2c_scl_padoen_o",
                     kSignalI2cSclPadoenO, "i2c_scl_padoen_o"),
         make_signal("TOP.vpi_peripheral_suite.i2c_sda_pad_i", kSignalI2cSdaPadI,
                     "i2c_sda_pad_i"),
         make_signal("TOP.vpi_peripheral_suite.i2c_sda_pad_o", kSignalI2cSdaPadO,
                     "i2c_sda_pad_o"),
         make_signal("TOP.vpi_peripheral_suite.i2c_sda_padoen_o",
                     kSignalI2cSdaPadoenO, "i2c_sda_padoen_o")},
    };
}

}  // namespace

double sc_time_stamp() { return static_cast<double>(main_time); }

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    const std::unique_ptr<VerilatedContext> context{new VerilatedContext};
    const std::unique_ptr<Vvpi_peripheral_suite> top{
        new Vvpi_peripheral_suite{context.get()}};

    Testbench testbench;
    tb = &testbench;

    settle(top.get());
    dut = bind_dut();
    register_edge_callback(cpptb::benchmarks::peripheral_suite::kSignalHclk);

    cpptb::benchmarks::peripheral_suite::BenchConfig config{
        env_u32("PERIPHERAL_SUITE_ITERS", 1000)};
    cpptb::benchmarks::peripheral_suite::BenchResult result;

    update_time(0);
    set_clock(top.get(), 0);

    const auto start = std::chrono::steady_clock::now();
    cpptb::benchmarks::peripheral_suite::register_benchmark(testbench, dut, config,
                                                            result);
    settle(top.get());

    const uint64_t max_cycles = static_cast<uint64_t>(config.iterations) * 400u + 2000u;
    uint64_t cycles = 0;
    for (; !testbench.done() && cycles < max_cycles; ++cycles) {
        tick(top.get());
    }

    const auto finish = std::chrono::steady_clock::now();
    const auto wall_us =
        std::chrono::duration_cast<std::chrono::microseconds>(finish - start).count();

    if (!testbench.done()) {
        std::fprintf(stderr, "peripheral-suite: timed out after %llu cycles\n",
                     static_cast<unsigned long long>(cycles));
        return 1;
    }

    top->final();
    const auto status = cpptb::benchmarks::peripheral_suite::done(result);
    std::printf(
        "CPP_VPI_PERIPHERAL_RESULT iterations=%u checks=%llu sim_cycles=%llu wall_ms=%.3f failures=%u\n",
        config.iterations, static_cast<unsigned long long>(result.checks),
        static_cast<unsigned long long>(cycles), static_cast<double>(wall_us) / 1000.0,
        result.failures);
    return status;
}
