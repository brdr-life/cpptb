// A cpptb port of Ibex's core_ibex UVM environment, dv/uvm/core_ibex.
//
// ports/core_ibex_uvm runs that environment unmodified on Verilator 5 and
// passes 912 of the 944 entries in directed_tests/directed_testlist.yaml. This
// drives the same design with the same memory agents and the same Spike
// co-simulation scoreboard, so a disagreement between the two harnesses on the
// same program is a defect in one of them rather than a difference of opinion
// about the core.
//
// Scope. 943 of the 944 directed entries run under `core_ibex_base_test`, and
// so does every riscv-dv entry this port is compared on; the 944th
// (mcounteren_lock_test) runs under core_ibex_mcounteren_lock_test, which is
// thirty lines on top of the base test and is here too. Nothing else from
// core_ibex_test_lib.sv is ported: the interrupt agent, the debug agent and the
// integrity/glitch machinery serve the other eight classes, and
// ports/core_ibex_uvm's README records that each of those contributes exactly
// one pass, on a program whose defining generator options pyflow cannot
// produce. Porting them would reproduce tests that do not test what their names
// claim. See README.md.
//
// What is here, and what it corresponds to upstream:
//
//   MemoryModel        mem_model_pkg::mem_model
//   MemAgent           ibex_mem_intf_response_agent: the monitor, the driver
//                      and ibex_mem_intf_response_seq collapsed into one
//                      object, because the sequence's only job is to answer
//                      the monitor and everything between them is zero-time
//   grant_driver       ibex_mem_intf_response_driver::send_grant
//   response_driver    ibex_mem_intf_response_driver::send_read_data
//   bus_monitor        ibex_mem_intf_monitor, plus the response sequence's
//                      body(), plus the base test's test_done_port subscriber
//   key_device         push_pull_agent in Pull/Device mode
//   fetch_enable_stim  fetch_enable_seq
//   CosimScoreboard    ibex_cosim_scoreboard, whole: run_cosim_rvfi,
//                      run_cosim_dmem, run_cosim_ifetch, run_cosim_ifetch_pmp,
//                      run_cosim_imem_errors and run_cosim_prune_imem_errors
//   RvfiMonitor        ibex_rvfi_monitor
//   DoubleFaultDetector core_ibex_scoreboard::double_fault_detector
//   wait_for_test_done core_ibex_base_test::wait_for_test_done
//
// Timing convention, the same one ports/ibex_icache_cpptb states and for the
// same reason. A "drive point" is the instant just after a falling edge of
// clk_i. `co_await RisingEdge` resumes before the design has evaluated that
// edge, so it yields the value `@(posedge clk)` reads in the Active region and
// is where every monitor here samples. Driving there would be wrong, because
// set() is immediate and the value would be captured by the edge being awaited.
//
// Upstream's drivers write through `@(posedge clk)` clocking blocks with the
// default output skew of zero, which means the pin changes in the NBA region of
// a posedge and is therefore sampled by the DUT at the *following* posedge. A
// value written at the drive point between those two posedges reaches the DUT
// at exactly the same edge. So:
//
//   * a statement upstream writes at `@(cb)` -- posedge p -- is written here at
//     the drive point after p, and
//   * a statement upstream writes after `wait_neg_clks()` -- a negedge between
//     posedge p and p+1 -- is written here at the drive point after p+1,
//     because the clocking block defers it to the next clocking event.
//
// The second of those is why grant_driver waits one falling edge more than
// send_grant appears to. Getting it wrong costs a cycle of grant latency on
// every bus access, which is exactly the sort of thing replay.py exists to
// catch.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpptb/cpptb.hpp"
#include "dut.hpp"

// ---------------------------------------------------------------------------
// Spike
//
// `cpptb build` compiles this file twice. The first compile is the hierarchy
// discovery pass: plain g++ with CPPTB_HIERARCHY_DISCOVERY defined, which then
// runs the result to find out which signals are clocks and which paths need a
// transport. That compile does not get Verilator's -CFLAGS or -LDFLAGS, so
// Spike's headers are not on the include path and its libraries are not on the
// link line. Everything that touches Spike is therefore behind this guard, and
// CosimBridge below is the whole of the interface.
//
// The other consequence of that pass is that this file must reach every signal
// it will ever reach on a run with no environment set. There are no early
// returns here: a missing program is reported and the run continues, and the
// clock starts before anything can go wrong. A testbench that returns before
// starting a clock is discovered as one that has no clock, and every later run
// dies at time zero with "scheduler starvation".
// ---------------------------------------------------------------------------
#ifndef CPPTB_HIERARCHY_DISCOVERY
#include "cosim.h"
#include "spike_cosim.h"

#ifndef CPPTB_COSIM_ISA
#error "CPPTB_COSIM_ISA must name the ISA the design was elaborated for"
#endif
#define CPPTB_STRINGIFY_IMPL(value) #value
#define CPPTB_STRINGIFY(value) CPPTB_STRINGIFY_IMPL(value)
#endif

namespace cpptb::ports::core_ibex {
namespace {

using cpptb::Dut;
using cpptb::TestContext;
using namespace cpptb::coro;

// ---------------------------------------------------------------------------
// Constants that have to agree with the build
// ---------------------------------------------------------------------------

// clk_rst_if's default clk_freq_mhz is 50, so clk_period_ps is 20,000.
constexpr auto kClockPeriod = 20_ns;

// core_ibex_cpptb_tb_top's BootAddr, and ADDRESS_DEFINES in
// ports/core_ibex_uvm/build_tb.py. The reset vector is BootAddr + 0x80.
constexpr uint32_t kBootAddr = 0x8000'0000u;
constexpr uint32_t kStartPc = (kBootAddr & ~0xFFu) | 0x80u;
constexpr uint32_t kStartMtvec = (kBootAddr & ~0xFFu) | 0x01u;
constexpr uint32_t kDmStartAddr = 0x1A11'0000u;
constexpr uint32_t kDmEndAddr = kDmStartAddr + 0x0000'0FFFu + 1u;

// The elaborated parameters, which core_ibex_base_test reads out of the UVM
// config DB and hands to spike_cosim_init. They are named here so that a
// cpptb.toml regenerated for a different configuration stops this compiling
// rather than co-simulating against the wrong core. fusesoc_setup.py writes
// them; --check is what keeps the two in step.
constexpr uint32_t kPmpNumRegions = 16;
constexpr uint32_t kPmpGranularity = 0;
constexpr uint32_t kMhpmCounterNum = 10;
constexpr bool kSecureIbex = true;
constexpr bool kICache = true;

// ibex_pkg
constexpr uint32_t kIbexMuBiOn = 0x5u;
constexpr uint32_t kIbexMuBiOff = 0xAu;
constexpr uint32_t kCsrMhpmCounter3 = 0xB03;
constexpr uint32_t kCsrMhpmCounter3H = 0xB83;
constexpr uint32_t kCsrMcycle = 0xB00;
constexpr uint32_t kCsrMcycleH = 0xB80;

// riscv_signature_pkg
constexpr uint32_t kSigCoreStatus = 0;
constexpr uint32_t kSigTestResult = 1;
constexpr uint32_t kTestPass = 0;
constexpr uint32_t kTestFail = 1;

// ibex_top_tracing exports ten performance counters on RVFI, and
// ibex_rvfi_monitor.sv loops over exactly ten.
constexpr std::size_t kNumPerfCounters = 10;

// core_ibex_env_cfg: the double-fault detector's two thresholds, and that
// reaching either is fatal.
constexpr uint32_t kDoubleFaultConsecutive = 100;
constexpr uint32_t kDoubleFaultTotal = 1000;

std::string env_string(const char* name,
                       const std::string& fallback = {}) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : fallback;
}

uint64_t env_number(const char* name, uint64_t fallback) {
    const std::string text = env_string(name);
    if (text.empty()) return fallback;
    return std::strtoull(text.c_str(), nullptr, 0);
}

std::string hex32(uint32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%08x", value);
    return std::string(buffer);
}

// ---------------------------------------------------------------------------
// CosimBridge: ibex_cosim_scoreboard's chandle, and the calls cosim_dpi.svh
// makes on it. The UVM environment reaches SpikeCosim through DPI because it
// has no other way to; this is the same object called directly.
// ---------------------------------------------------------------------------

class CosimBridge {
   public:
    // ibex_cosim_scoreboard::init_cosim, which spike_cosim_dpi.cc follows with
    // add_memory over the whole address space.
    void create(const std::string& log_file);
    void release();
    [[nodiscard]] bool active() const { return handle_ != nullptr; }

    // ibex_cosim_agent::load_binary_to_mem and write_mem_byte
    void write_mem(uint32_t addr, const uint8_t* data, std::size_t length);
    void write_mem_byte(uint32_t addr, uint8_t value);

    void set_mip(uint32_t pre, uint32_t post);
    void set_nmi(bool value);
    void set_nmi_int(bool value);
    void set_debug_req(bool value);
    void set_mcycle(uint64_t value);
    void set_csr(uint32_t number, uint32_t value);
    void set_ic_scr_key_valid(bool value);
    void set_iside_error(uint32_t addr);
    void notify_dside_access(bool store, uint32_t addr, uint32_t data,
                             uint32_t be, bool error, bool misaligned_first,
                             bool misaligned_second,
                             bool misaligned_first_saw_error,
                             bool m_mode_access);

    // riscv_cosim_step, and get_cosim_error_str on a false return.
    bool step(uint32_t write_reg, uint32_t write_reg_data, uint32_t pc,
              bool sync_trap, bool suppress_reg_write);
    [[nodiscard]] std::string error_string();
    [[nodiscard]] uint64_t insn_count() const;

   private:
    void* handle_ = nullptr;
};

#ifdef CPPTB_HIERARCHY_DISCOVERY
void CosimBridge::create(const std::string&) {}
void CosimBridge::release() {}
void CosimBridge::write_mem(uint32_t, const uint8_t*, std::size_t) {}
void CosimBridge::write_mem_byte(uint32_t, uint8_t) {}
void CosimBridge::set_mip(uint32_t, uint32_t) {}
void CosimBridge::set_nmi(bool) {}
void CosimBridge::set_nmi_int(bool) {}
void CosimBridge::set_debug_req(bool) {}
void CosimBridge::set_mcycle(uint64_t) {}
void CosimBridge::set_csr(uint32_t, uint32_t) {}
void CosimBridge::set_ic_scr_key_valid(bool) {}
void CosimBridge::set_iside_error(uint32_t) {}
void CosimBridge::notify_dside_access(bool, uint32_t, uint32_t, uint32_t, bool,
                                      bool, bool, bool, bool) {}
bool CosimBridge::step(uint32_t, uint32_t, uint32_t, bool, bool) { return true; }
std::string CosimBridge::error_string() { return {}; }
uint64_t CosimBridge::insn_count() const { return 0; }
#else
namespace {
Cosim* as_cosim(void* handle) { return static_cast<Cosim*>(handle); }
}  // namespace

void CosimBridge::create(const std::string& log_file) {
    release();
    // The static_cast is load-bearing: SpikeCosim inherits from simif_t and
    // from Cosim, so the Cosim subobject does not start at the same address as
    // the object. spike_cosim_dpi.cc writes the same cast for the same reason.
    auto* spike = new SpikeCosim(
        CPPTB_STRINGIFY(CPPTB_COSIM_ISA), kStartPc, kStartMtvec, log_file,
        kSecureIbex, kICache, kPmpNumRegions, kPmpGranularity, kMhpmCounterNum,
        kDmStartAddr, kDmEndAddr);
    handle_ = static_cast<Cosim*>(spike);
    // "Add a memory device that covers the entire address space. This will
    // only be sparsely populated." -- spike_cosim_dpi.cc
    as_cosim(handle_)->add_memory(0x0000'0000u, 0xFFFF'0000u);
}

void CosimBridge::release() {
    if (handle_ == nullptr) return;
    delete as_cosim(handle_);
    handle_ = nullptr;
}

void CosimBridge::write_mem(uint32_t addr, const uint8_t* data,
                            std::size_t length) {
    if (handle_ == nullptr) return;
    as_cosim(handle_)->backdoor_write_mem(addr, length, data);
}

void CosimBridge::write_mem_byte(uint32_t addr, uint8_t value) {
    write_mem(addr, &value, 1);
}

void CosimBridge::set_mip(uint32_t pre, uint32_t post) {
    if (handle_ != nullptr) as_cosim(handle_)->set_mip(pre, post);
}
void CosimBridge::set_nmi(bool value) {
    if (handle_ != nullptr) as_cosim(handle_)->set_nmi(value);
}
void CosimBridge::set_nmi_int(bool value) {
    if (handle_ != nullptr) as_cosim(handle_)->set_nmi_int(value);
}
void CosimBridge::set_debug_req(bool value) {
    if (handle_ != nullptr) as_cosim(handle_)->set_debug_req(value);
}
void CosimBridge::set_mcycle(uint64_t value) {
    if (handle_ != nullptr) as_cosim(handle_)->set_mcycle(value);
}
void CosimBridge::set_csr(uint32_t number, uint32_t value) {
    if (handle_ != nullptr)
        as_cosim(handle_)->set_csr(static_cast<int>(number), value);
}
void CosimBridge::set_ic_scr_key_valid(bool value) {
    if (handle_ != nullptr) as_cosim(handle_)->set_ic_scr_key_valid(value);
}
void CosimBridge::set_iside_error(uint32_t addr) {
    if (handle_ != nullptr) as_cosim(handle_)->set_iside_error(addr);
}

void CosimBridge::notify_dside_access(bool store, uint32_t addr, uint32_t data,
                                      uint32_t be, bool error,
                                      bool misaligned_first,
                                      bool misaligned_second,
                                      bool misaligned_first_saw_error,
                                      bool m_mode_access) {
    if (handle_ == nullptr) return;
    DSideAccessInfo info{};
    info.store = store;
    info.data = data;
    info.addr = addr;
    info.be = be;
    info.error = error;
    info.misaligned_first = misaligned_first;
    info.misaligned_second = misaligned_second;
    info.misaligned_first_saw_error = misaligned_first_saw_error;
    info.m_mode_access = m_mode_access;
    as_cosim(handle_)->notify_dside_access(info);
}

bool CosimBridge::step(uint32_t write_reg, uint32_t write_reg_data, uint32_t pc,
                       bool sync_trap, bool suppress_reg_write) {
    if (handle_ == nullptr) return true;
    return as_cosim(handle_)->step(write_reg, write_reg_data, pc, sync_trap,
                                   suppress_reg_write);
}

std::string CosimBridge::error_string() {
    if (handle_ == nullptr) return {};
    std::string message = "Cosim mismatch ";
    for (const std::string& error : as_cosim(handle_)->get_errors()) {
        message += error;
        message += "\n";
    }
    as_cosim(handle_)->clear_errors();
    return message;
}

uint64_t CosimBridge::insn_count() const {
    if (handle_ == nullptr) return 0;
    return as_cosim(handle_)->get_insn_cnt();
}
#endif

// ---------------------------------------------------------------------------
// mem_model_pkg::mem_model
//
// A byte-addressed sparse memory that knows which bytes have been written.
// Upstream is an associative array of bytes; this is the same thing in pages,
// because a run of the longer directed tests makes a few million accesses and
// a page lookup is a shift where a hash is not.
// ---------------------------------------------------------------------------

class MemoryModel {
   public:
    static constexpr uint32_t kPageBits = 12;
    static constexpr uint32_t kPageSize = 1u << kPageBits;

    [[nodiscard]] bool addr_exists(uint32_t addr) const {
        const Page* page = find(addr >> kPageBits);
        return page != nullptr && page->valid[addr & (kPageSize - 1)];
    }

    [[nodiscard]] uint8_t read_byte(uint32_t addr) const {
        const Page* page = find(addr >> kPageBits);
        return page == nullptr ? 0u : page->data[addr & (kPageSize - 1)];
    }

    void write_byte(uint32_t addr, uint8_t value) {
        Page& page = obtain(addr >> kPageBits);
        page.data[addr & (kPageSize - 1)] = value;
        page.valid[addr & (kPageSize - 1)] = true;
    }

    [[nodiscard]] uint64_t bytes_written() const { return written_; }

   private:
    struct Page {
        std::array<uint8_t, kPageSize> data{};
        std::array<bool, kPageSize> valid{};
    };

    [[nodiscard]] const Page* find(uint32_t index) const {
        const auto found = pages_.find(index);
        return found == pages_.end() ? nullptr : found->second.get();
    }

    Page& obtain(uint32_t index) {
        auto& slot = pages_[index];
        if (!slot) {
            slot = std::make_unique<Page>();
        }
        ++written_;
        return *slot;
    }

    std::unordered_map<uint32_t, std::unique_ptr<Page>> pages_;
    uint64_t written_ = 0;
};

// ---------------------------------------------------------------------------
// ibex_mem_intf_seq_item, restricted to the fields this environment sets
// ---------------------------------------------------------------------------

struct MemItem {
    uint32_t addr = 0;
    bool write = false;
    uint32_t data = 0;
    uint32_t be = 0;
    bool error = false;
    bool bad_intg = false;
    bool spurious = false;
    uint32_t rvalid_delay = 0;
    // The four fields core_ibex_tb_top.sv assigns into ibex_mem_intf from
    // inside the load/store unit, which the cosim scoreboard forwards.
    bool misaligned_first = false;
    bool misaligned_second = false;
    bool misaligned_first_saw_error = false;
    bool m_mode_access = false;
};

// ---------------------------------------------------------------------------
// Counters, so that a cpptb run and a UVM run can be compared on more than
// their verdict. run_directed.py reads the line report() prints.
// ---------------------------------------------------------------------------

struct Counters {
    uint64_t cycles = 0;
    uint64_t imem_grants = 0;
    uint64_t imem_responses = 0;
    uint64_t imem_uninit = 0;
    uint64_t dmem_grants = 0;
    uint64_t dmem_responses = 0;
    uint64_t dmem_writes = 0;
    uint64_t dmem_uninit = 0;
    uint64_t dmem_spurious = 0;
    uint64_t retired = 0;
    uint64_t irq_only_items = 0;
    uint64_t cosim_steps = 0;
    uint64_t traps = 0;
    uint64_t iside_errors = 0;
    uint64_t ifetches = 0;
    uint64_t pmp_ifetch_errors = 0;
    uint64_t double_faults = 0;
    uint64_t fetch_enable_pulses = 0;
    uint64_t key_answers = 0;
    uint64_t signature_writes = 0;
};

// ---------------------------------------------------------------------------
// The environment, shared between the coroutines the way the UVM environment's
// config object and analysis ports are shared between its components.
// ---------------------------------------------------------------------------

enum class Ending {
    kRunning,
    kHandshakePass,
    kHandshakeFail,
    kCosimMismatch,
    kDoubleFault,
    kCycleTimeout,
    kMalformedHandshake,
    kReplayDivergence,
};

struct BusConfig {
    // ibex_mem_intf_response_agent_cfg, at its defaults.
    uint32_t gnt_delay_min = 0;
    uint32_t gnt_delay_max = 10;
    uint32_t gnt_pick_medium_speed_weight = 1;
    uint32_t gnt_pick_slow_speed_weight = 1;
    uint32_t valid_delay_min = 0;
    uint32_t valid_delay_max = 20;
    uint32_t valid_pick_medium_speed_weight = 1;
    uint32_t valid_pick_slow_speed_weight = 1;
    uint32_t zero_delay_pct = 50;
    bool zero_delays = false;
    bool enable_bad_intg_on_uninit_access = false;
    // ibex_mem_intf_response_agent::build_phase sets this to SecureIbex. With
    // it clear the driver puts 'x on rdata and rintg for a write response, and
    // ibex_top's IbexDataRPayloadX assertion says that is only allowed when the
    // core is not checking integrity on writes. It is set here for the same
    // reason it is set there.
    bool fixed_data_write_response = kSecureIbex;
    uint32_t spurious_response_delay_min = 0;
    uint32_t spurious_response_delay_max = 100;
};

struct Bus {
    bool dside = false;
    BusConfig cfg;
    // ibex_mem_intf_response_seq: is_dmem_seq, and the two error injectors,
    // which nothing in core_ibex_base_test ever calls.
    bool enable_error = false;
    bool enable_intg_error = false;
    bool enable_spurious_response = false;
    uint32_t spurious_response_delay_cycles = 0;

    // ibex_mem_intf_response_driver::rdata_queue
    std::deque<MemItem> responses;
    // ibex_mem_intf_monitor::collect_response_queue
    std::deque<MemItem> outstanding;
    int outstanding_accesses = 0;
    // The response driver's `spurious_response` output, which the monitor reads
    // as a vif field upstream. There is no such pin on the wrapper because
    // nothing in the design looks at it; the driver writes it at the same drive
    // point it writes rvalid, and the monitor reads it at the same posedge, so
    // it behaves exactly as the wire does.
    bool driving_spurious = false;
};

struct Env {
    Counters counters;
    MemoryModel mem;
    CosimBridge cosim;
    Bus imem;
    Bus dmem;

    uint32_t signature_addr = 0x8fff'fffc;
    uint64_t timeout_cycles = 5'000'000;
    bool relax_cosim_check = false;
    bool test_done = false;
    Ending ending = Ending::kRunning;
    std::string ending_detail;

    // core_ibex_mcounteren_lock_test's stimulus, off in the base test.
    bool mcounteren_lock = false;

    // Set when the run is replaying a recording from ports/core_ibex_uvm
    // rather than generating its own stimulus. The monitors still run -- the
    // point is to put this port's co-simulation scoreboard on the baseline's
    // stimulus -- but nothing below them generates a response.
    bool replay = false;
    uint64_t replay_cycles = 0;

    // Fault injection, so that a run can show these checks are live rather than
    // merely running. See README.md.
    uint64_t corrupt_imem_response = 0;
    uint64_t corrupt_dmem_response = 0;

    // The first reason to stop wins, with one exception: a cosim mismatch is a
    // uvm_fatal upstream whenever it happens, including in the three thousand
    // cycles the base test runs after the handshake, so it overrides a verdict
    // already reached.
    void finish(Ending reason, std::string detail = {}) {
        if (ending != Ending::kRunning && reason != Ending::kCosimMismatch) {
            return;
        }
        if (ending == Ending::kCosimMismatch) return;
        ending = reason;
        ending_detail = std::move(detail);
        test_done = true;
    }
};

// ---------------------------------------------------------------------------
// Bus pin access. The two ibex_mem_intf instances differ only in their port
// names, so every driver and monitor below takes the bus by reference and asks
// these.
// ---------------------------------------------------------------------------

bool bus_req(Dut dut, const Bus& bus) {
    return (bus.dside ? dut.data_req_o.get() : dut.instr_req_o.get()) != 0;
}
uint32_t bus_addr(Dut dut, const Bus& bus) {
    return bus.dside ? dut.data_addr_o.get() : dut.instr_addr_o.get();
}
bool bus_we(Dut dut, const Bus& bus) {
    return bus.dside && dut.data_we_o.get() != 0;
}
uint32_t bus_be(Dut dut, const Bus& bus) {
    // core_ibex_tb_top.sv ties instr_mem_vif.be to zero.
    return bus.dside ? dut.data_be_o.get() : 0u;
}
uint32_t bus_wdata(Dut dut, const Bus& bus) {
    return bus.dside ? dut.data_wdata_o.get() : 0u;
}
bool bus_gnt(Dut dut, const Bus& bus) {
    return (bus.dside ? dut.data_gnt_i.get() : dut.instr_gnt_i.get()) != 0;
}
bool bus_rvalid(Dut dut, const Bus& bus) {
    return (bus.dside ? dut.data_rvalid_i.get() : dut.instr_rvalid_i.get()) != 0;
}
void set_bus_gnt(Dut dut, const Bus& bus, bool value) {
    if (bus.dside) {
        dut.data_gnt_i.set(value ? 1 : 0);
    } else {
        dut.instr_gnt_i.set(value ? 1 : 0);
    }
}
void set_bus_response(Dut dut, const Bus& bus, bool rvalid, uint32_t rdata,
                      bool error, bool bad_intg) {
    if (bus.dside) {
        dut.data_rvalid_i.set(rvalid ? 1 : 0);
        dut.data_rdata_i.set(rdata);
        dut.data_err_i.set(error ? 1 : 0);
        dut.data_bad_intg_i.set(bad_intg ? 1 : 0);
    } else {
        dut.instr_rvalid_i.set(rvalid ? 1 : 0);
        dut.instr_rdata_i.set(rdata);
        dut.instr_err_i.set(error ? 1 : 0);
        dut.instr_bad_intg_i.set(bad_intg ? 1 : 0);
    }
}

// ---------------------------------------------------------------------------
// The two delay distributions.
//
// Upstream draws both through a constrained randomize(), which on Verilator is
// a round trip to a z3 subprocess for every bus access and cost the baseline a
// factor of forty; ports/core_ibex_uvm replaces the grant one with a direct
// draw of the same buckets at the same weights, and its README records that the
// distributions are unchanged. These are those buckets. `:/` spreads a weight
// uniformly across its range, which is what the ranged branches do.
// ---------------------------------------------------------------------------

uint32_t draw_gnt_delay(Random& random, const BusConfig& cfg) {
    if (cfg.zero_delays) return 0;
    const uint32_t w_lo = 10;
    const uint32_t w_mid = (cfg.gnt_delay_max >= cfg.gnt_delay_min + 2)
                               ? cfg.gnt_pick_medium_speed_weight
                               : 0;
    const uint32_t w_hi = cfg.gnt_pick_slow_speed_weight;
    const uint32_t pick = random.randint<uint32_t>(0, w_lo + w_mid + w_hi - 1);
    if (pick < w_lo) return cfg.gnt_delay_min;
    if (pick < w_lo + w_mid) {
        return random.randint<uint32_t>(cfg.gnt_delay_min + 1,
                                        cfg.gnt_delay_max - 1);
    }
    return cfg.gnt_delay_max;
}

uint32_t draw_valid_delay(Random& random, const BusConfig& cfg) {
    if (cfg.zero_delays) return 0;
    const uint32_t half = cfg.valid_delay_max / 2;
    const bool has_low_range = half >= cfg.valid_delay_min + 2;
    const bool has_high_range = cfg.valid_delay_max >= half + 1;
    const uint32_t w_min = 5;
    const uint32_t w_low = has_low_range ? 3 : 0;
    const uint32_t w_mid =
        has_high_range ? cfg.valid_pick_medium_speed_weight : 0;
    const uint32_t w_max = cfg.valid_pick_slow_speed_weight;
    const uint32_t pick =
        random.randint<uint32_t>(0, w_min + w_low + w_mid + w_max - 1);
    if (pick < w_min) return cfg.valid_delay_min;
    if (pick < w_min + w_low) {
        return random.randint<uint32_t>(cfg.valid_delay_min + 1, half - 1);
    }
    if (pick < w_min + w_low + w_mid) {
        return random.randint<uint32_t>(half, cfg.valid_delay_max - 1);
    }
    return cfg.valid_delay_max;
}

// prim_secded_inv_39_32_enc's data half is the data itself, so the integrity
// bits are computed in the wrapper and the testbench drives only `bad_intg`.
// See core_ibex_cpptb_tb_top.sv.

// ---------------------------------------------------------------------------
// ibex_mem_intf_response_seq::read and ::write, and the item it builds
// ---------------------------------------------------------------------------

// Reads a word, handling uninitialised memory the way the response sequence
// does: DMEM returns random data and writes it back into both memory models,
// IMEM returns {2{16'h0000}}, two C.unimp instructions.
uint32_t read_word(Env& env, Bus& bus, Random& random, uint32_t addr,
                   bool& uninit) {
    uint32_t data = 0;
    for (int index = 3; index >= 0; --index) {
        data <<= 8;
        const uint32_t byte_addr = addr + static_cast<uint32_t>(index);
        if (!env.mem.addr_exists(byte_addr)) {
            uninit = true;
            if (!bus.dside) {
                ++env.counters.imem_uninit;
                return 0x0000'0000u;
            }
            ++env.counters.dmem_uninit;
            const auto value = random.randint<uint32_t>(0, 0xFFu);
            env.mem.write_byte(byte_addr, static_cast<uint8_t>(value));
            env.cosim.write_mem_byte(byte_addr, static_cast<uint8_t>(value));
            data = (data & ~0xFFu) | value;
            continue;
        }
        data = (data & ~0xFFu) | env.mem.read_byte(byte_addr);
    }
    return data;
}

void write_word(Env& env, uint32_t addr, uint32_t data, uint32_t be) {
    for (int index = 0; index < 4; ++index) {
        if ((be >> index) & 1u) {
            env.mem.write_byte(addr + static_cast<uint32_t>(index),
                               static_cast<uint8_t>(data));
        }
        data >>= 8;
    }
}

// ibex_mem_intf_response_seq's body, for one observed request. Upstream this is
// a sequence blocked on p_sequencer.addr_ph_port; here it is called by the
// monitor at the posedge the address phase completes, which is the same instant
// and the same order.
MemItem make_response(Env& env, Bus& bus, Random& random,
                      const MemItem& observed) {
    MemItem response = observed;
    response.rvalid_delay = draw_valid_delay(random, bus.cfg);
    response.error = bus.enable_error;
    bus.enable_error = false;  // "Disable after single inserted error."

    const uint32_t aligned = observed.addr & ~3u;
    // "Do not inject any error to the handshake test_control_addr" -- the two
    // words the base test watches for.
    if (aligned == (env.signature_addr - 4u) || aligned == env.signature_addr) {
        response.error = false;
        bus.enable_intg_error = false;
    }

    bool uninit = false;
    if (response.error) {
        response.data = random.randint<uint32_t>(0, 0xFFFF'FFFFu);
    } else if (!observed.write) {
        response.data = read_word(env, bus, random, aligned, uninit);
    } else {
        write_word(env, aligned, observed.data, observed.be);
        if (bus.cfg.fixed_data_write_response) {
            // "drive data in store response to fixed 32'hffffffff value.
            // Integrity is calculated below." -- the response sequence.
            response.data = 0xFFFF'FFFFu;
        } else {
            response.data = 0;
        }
    }

    response.bad_intg =
        (bus.cfg.enable_bad_intg_on_uninit_access && uninit) ||
        bus.enable_intg_error;
    bus.enable_intg_error = false;
    return response;
}

// ---------------------------------------------------------------------------
// ibex_mem_intf_response_driver::send_grant
//
// Entered at a drive point. See the header for why the write is one falling
// edge later than upstream's text puts it: `wait_neg_clks` moves to a negedge,
// and a clocking-block output written there is deferred to the next posedge.
// ---------------------------------------------------------------------------

Task<void> grant_driver(Dut dut, TestContext& test, Env& env, Bus& bus) {
    auto& random = test.random();
    while (true) {
        co_await FallingEdge{dut.clk_i};
        if (!bus_req(dut, bus)) continue;

        const uint32_t delay = draw_gnt_delay(random, bus.cfg);
        for (uint32_t index = 0; index <= delay; ++index) {
            co_await FallingEdge{dut.clk_i};
        }
        set_bus_gnt(dut, bus, true);
        co_await FallingEdge{dut.clk_i};
        set_bus_gnt(dut, bus, false);
    }
}

// ---------------------------------------------------------------------------
// ibex_mem_intf_response_driver::send_read_data
//
// Upstream drives rdata, rintg and error to 'x between responses. Verilator has
// no X, so the baseline drives zero there too; this is a statement about both
// harnesses rather than a difference between them.
// ---------------------------------------------------------------------------

Task<void> response_driver(Dut dut, Env& env, Bus& bus) {
    uint64_t responses_driven = 0;
    while (true) {
        co_await FallingEdge{dut.clk_i};
        set_bus_response(dut, bus, false, 0, false, false);
        bus.driving_spurious = false;

        while (bus.responses.empty()) {
            co_await FallingEdge{dut.clk_i};
        }
        MemItem item = bus.responses.front();
        bus.responses.pop_front();

        for (uint32_t index = 0; index < item.rvalid_delay; ++index) {
            co_await FallingEdge{dut.clk_i};
        }

        // Read responses only. A write response carries no data the core
        // reads: `fixed_data_write_response` makes it a constant, the wrapper
        // computes matching integrity for whatever is on the pins, and the LSU
        // discards the payload. Corrupting one is invisible by construction, so
        // counting them would make the injection index mean nothing.
        if (!item.write && !item.spurious) ++responses_driven;
        const uint64_t corrupt =
            bus.dside ? env.corrupt_dmem_response : env.corrupt_imem_response;
        if (corrupt != 0 && !item.write && responses_driven == corrupt) {
            // One bit of one response, which is how the co-simulation is shown
            // to be live rather than quiet. See README.md.
            item.data ^= 1u;
            std::printf("cpptb-core-ibex: corrupting %s read response %llu at "
                        "0x%s\n", bus.dside ? "dmem" : "imem",
                        static_cast<unsigned long long>(responses_driven),
                        hex32(item.addr).c_str());
        }

        bus.driving_spurious = item.spurious;
        set_bus_response(dut, bus, true, item.data, item.error, item.bad_intg);
        if (bus.dside) {
            ++env.counters.dmem_responses;
        } else {
            ++env.counters.imem_responses;
        }
    }
}

// ---------------------------------------------------------------------------
// ibex_mem_intf_monitor, the response sequence's body, and the base test's
// test_done_port subscriber, at one posedge each.
//
// Upstream these are four processes over two analysis ports and a mailbox,
// with nothing but zero-time between them. The order below is the order the
// data flows: a response is matched against the transaction whose address phase
// completed on an earlier posedge, so the response phase runs before the
// address phase, exactly as upstream's `do @(cb); while (...)` guarantees.
// ---------------------------------------------------------------------------

void on_signature_write(Env& env, const MemItem& item);

Task<void> bus_monitor(Dut dut, TestContext& test, Env& env, Bus& bus) {
    auto& random = test.random();
    while (true) {
        co_await RisingEdge{dut.clk_i};

        const bool rvalid = bus_rvalid(dut, bus) && !bus.driving_spurious;

        // collect_response_phase
        if (rvalid && !bus.outstanding.empty()) {
            MemItem item = bus.outstanding.front();
            bus.outstanding.pop_front();
            if (!item.write) {
                item.data = bus.dside ? dut.data_rdata_i.get()
                                      : dut.instr_rdata_i.get();
            }
            item.error = (bus.dside ? dut.data_err_i.get()
                                    : dut.instr_err_i.get()) != 0;
            if (bus.dside && env.replay && !item.write) {
                // The baseline answered a read of uninitialised memory with
                // random data and wrote it into both memory models. What
                // reaches the wire here is that data, so the same two writes
                // have to happen or Spike executes against a memory the DUT
                // never had.
                const uint32_t aligned = item.addr & ~3u;
                uint32_t bytes = item.data;
                for (uint32_t index = 0; index < 4; ++index) {
                    if (!env.mem.addr_exists(aligned + index)) {
                        const auto value = static_cast<uint8_t>(bytes);
                        env.mem.write_byte(aligned + index, value);
                        env.cosim.write_mem_byte(aligned + index, value);
                    }
                    bytes >>= 8;
                }
            }
            if (bus.dside) {
                // core_ibex_env connects the dside monitor to the cosim agent,
                // and the base test connects it to test_done_port as well.
                env.cosim.notify_dside_access(
                    item.write, item.addr, item.data, item.be, item.error,
                    item.misaligned_first, item.misaligned_second,
                    item.misaligned_first_saw_error, item.m_mode_access);
                on_signature_write(env, item);
            }
        }

        // collect_address_phase
        if (bus_req(dut, bus) && bus_gnt(dut, bus)) {
            MemItem observed;
            observed.addr = bus_addr(dut, bus);
            observed.be = bus_be(dut, bus);
            observed.write = bus_we(dut, bus);
            if (observed.write) observed.data = bus_wdata(dut, bus);
            if (bus.dside) {
                observed.misaligned_first =
                    dut.data_misaligned_first_o.get() != 0;
                observed.misaligned_second =
                    dut.data_misaligned_second_o.get() != 0;
                observed.misaligned_first_saw_error =
                    dut.data_misaligned_first_saw_error_o.get() != 0;
                observed.m_mode_access = dut.data_m_mode_access_o.get() != 0;
                ++env.counters.dmem_grants;
                if (observed.write) ++env.counters.dmem_writes;
            } else {
                ++env.counters.imem_grants;
            }
            bus.outstanding.push_back(observed);
            ++bus.outstanding_accesses;
            if (env.replay) {
                // The response is already on the wire. The memory model is
                // still kept up to date, because the co-simulation needs to
                // know which addresses had been written when a read of an
                // uninitialised one comes back.
                if (observed.write) {
                    write_word(env, observed.addr & ~3u, observed.data,
                               observed.be);
                }
            } else {
                bus.responses.push_back(
                    make_response(env, bus, random, observed));
            }
        }
        if (rvalid) --bus.outstanding_accesses;

        // The spurious-response half of ibex_mem_intf_response_seq::body. It
        // wakes on every monitor tick, and only sends when the interface is
        // idle: no new request this cycle and nothing outstanding.
        if (bus.enable_spurious_response && !bus_req(dut, bus)) {
            if (bus.spurious_response_delay_cycles == 0 &&
                bus.outstanding_accesses == 0) {
                MemItem spurious;
                spurious.spurious = true;
                spurious.rvalid_delay = 0;
                spurious.data = random.randint<uint32_t>(0, 0xFFFF'FFFFu);
                bus.responses.push_back(spurious);
                ++env.counters.dmem_spurious;
                bus.spurious_response_delay_cycles = random.randint<uint32_t>(
                    bus.cfg.spurious_response_delay_min,
                    bus.cfg.spurious_response_delay_max);
            } else if (bus.spurious_response_delay_cycles > 0) {
                --bus.spurious_response_delay_cycles;
            }
        }
    }
}

// core_ibex_base_test::wait_for_test_done's first fork arm, through
// wait_for_mem_txn(signature_addr - 4, TEST_RESULT, test_done_port).
void on_signature_write(Env& env, const MemItem& item) {
    if (!item.write) return;
    if (item.addr == env.signature_addr ||
        item.addr == env.signature_addr - 4u) {
        ++env.counters.signature_writes;
    }
    if (item.addr != env.signature_addr - 4u) return;
    if ((item.data & 0xFFu) != kSigTestResult) return;

    const uint32_t result = item.data >> 8;
    if (result == kTestPass) {
        env.finish(Ending::kHandshakePass);
    } else if (result == kTestFail) {
        env.finish(Ending::kHandshakeFail);
    } else {
        env.finish(Ending::kMalformedHandshake,
                   "the data 0x" + hex32(item.data) +
                       " written to the signature address is formatted "
                       "incorrectly");
    }
}

// ---------------------------------------------------------------------------
// push_pull_agent in Pull/Device mode: the scrambling key provider.
//
// The agent randomises the whole device data word per transaction and answers
// after a delay drawn from its config. Nothing in the design reads the key for
// anything but scrambling the icache RAMs, so what the bits are does not
// matter; that the request is eventually answered does, because the cache
// blocks its first fill until it has a valid key.
// ---------------------------------------------------------------------------

Task<void> key_device(Dut dut, TestContext& test, Env& env) {
    auto& random = test.random();
    while (true) {
        co_await FallingEdge{dut.clk_i};
        if (dut.scramble_req_o.get() == 0) {
            dut.scramble_key_valid_i.set(0);
            continue;
        }
        const auto delay = random.randint<uint32_t>(0, 10);
        for (uint32_t index = 0; index < delay; ++index) {
            co_await FallingEdge{dut.clk_i};
        }
        dut.scramble_key_i.set(random.randbits<128>());
        dut.scramble_nonce_i.set(random.next_u64());
        dut.scramble_key_valid_i.set(1);
        ++env.counters.key_answers;
        co_await RisingEdge{dut.clk_i};
        co_await FallingEdge{dut.clk_i};
        dut.scramble_key_valid_i.set(0);
    }
}

// ---------------------------------------------------------------------------
// fetch_enable_seq
//
// core_ibex_vseq::pre_body starts this unless +disable_fetch_enable_seq is set,
// with InfiniteRuns and a 1000 to 2000 cycle gap between items. Each item takes
// fetch_enable to one of the off encodings for 1 to 500 cycles and puts it back.
// zero_delays, drawn per item at 50%, removes the gap.
//
// ports/core_ibex_uvm runs this: the README records that no plusarg workaround
// is needed and that the fetch-enable stimulus runs, so it is part of what the
// 912 passes were produced with.
// ---------------------------------------------------------------------------

Task<void> fetch_enable_stimulus(Dut dut, TestContext& test, Env& env) {
    auto& random = test.random();
    dut.fetch_enable_i.set(kIbexMuBiOn);
    while (true) {
        const bool zero_delays = random.randint<uint32_t>(0, 99) < 50;
        if (!zero_delays) {
            const auto gap = random.randint<uint32_t>(1000, 2000);
            for (uint32_t index = 0; index < gap; ++index) {
                co_await RisingEdge{dut.clk_i};
            }
        }
        // SecureIbex makes fetch_enable a MuBi, and the sequence exercises
        // every off encoding rather than just IbexMuBiOff.
        uint32_t off = kIbexMuBiOff;
        if (kSecureIbex) {
            do {
                off = random.randint<uint32_t>(0, 15);
            } while (off == kIbexMuBiOn);
        }
        co_await FallingEdge{dut.clk_i};
        dut.fetch_enable_i.set(off);
        ++env.counters.fetch_enable_pulses;
        const auto hold = random.randint<uint32_t>(1, 500);
        for (uint32_t index = 0; index < hold; ++index) {
            co_await RisingEdge{dut.clk_i};
        }
        co_await FallingEdge{dut.clk_i};
        dut.fetch_enable_i.set(kIbexMuBiOn);
    }
}

// ---------------------------------------------------------------------------
// ibex_cosim_scoreboard, and the two monitors that feed it
// ---------------------------------------------------------------------------

class CosimScoreboard {
   public:
    CosimScoreboard(TestContext& test, Env& env) : test_(test), env_(env) {}

    // run_cosim_ifetch: which fetch addresses have seen an error that was not a
    // PMP error. The icache records both with the same error bits, which is
    // what run_cosim_ifetch_pmp is for.
    void on_ifetch(uint32_t addr, uint32_t rdata, bool err, bool err_plus2) {
        ++env_.counters.ifetches;
        const uint32_t aligned = addr & ~3u;
        const uint32_t next = aligned + 4u;
        if (err) {
            failed_iside_[err_plus2 ? next : aligned] = true;
            return;
        }
        if ((addr & 3u) != 0 && (rdata & 3u) == 3u) {
            failed_iside_.erase(next);
        }
        failed_iside_.erase(aligned);
    }

    // run_cosim_ifetch_pmp
    void on_ifetch_pmp(uint32_t addr, bool pmp_err) {
        if (pmp_err) {
            iside_pmp_failure_[addr] = true;
            ++env_.counters.pmp_ifetch_errors;
        } else {
            iside_pmp_failure_.erase(addr);
        }
    }

    // run_cosim_imem_errors
    void on_id_stage(bool id_done, uint64_t order, uint32_t pc,
                     bool is_compressed, bool wb_exception) {
        // run_cosim_prune_imem_errors, which pops an error whose instruction
        // was flushed from ID by a writeback exception before it retired.
        if (pruning_ == Pruning::kArmed) {
            pruning_ = Pruning::kWaiting;
        } else if (pruning_ == Pruning::kWaiting &&
                   (id_done || wb_exception)) {
            if (!id_done && wb_exception && !iside_error_queue_.empty()) {
                iside_error_queue_.pop_back();
            }
            pruning_ = Pruning::kIdle;
        }

        if (!id_done || order == latest_order_) return;
        latest_order_ = order;
        if (wb_exception) return;

        const uint32_t aligned = pc & 0xFFFF'FFFCu;
        const uint32_t next = aligned + 4u;
        if (failed_iside_.count(aligned) && !iside_pmp_failure_.count(aligned)) {
            iside_error_queue_.push_back({order, aligned});
            pruning_ = Pruning::kArmed;
        } else if (!is_compressed && (pc & 3u) != 0 &&
                   failed_iside_.count(next) &&
                   !iside_pmp_failure_.count(next)) {
            iside_error_queue_.push_back({order, next});
            pruning_ = Pruning::kArmed;
        }
    }

    // run_cosim_rvfi
    void on_rvfi(Dut dut) {
        const bool valid = dut.rvfi_valid_o.get() != 0;
        const bool irq_valid = dut.rvfi_ext_irq_valid_o.get() != 0;
        const bool irq_only = !valid && irq_valid;
        ++double_fault_items_;
        if (double_fault_pulse_seen_) {
            double_fault_pulse_seen_ = false;
            ++double_fault_total_;
            ++double_fault_consecutive_;
            ++env_.counters.double_faults;
        } else {
            double_fault_consecutive_ = 0;
        }
        if (double_fault_consecutive_ == kDoubleFaultConsecutive ||
            double_fault_total_ == kDoubleFaultTotal) {
            env_.finish(Ending::kDoubleFault,
                        "double_fault detector reached its threshold");
            return;
        }

        const uint32_t pre_mip = dut.rvfi_ext_pre_mip_o.get();
        const uint32_t post_mip = dut.rvfi_ext_post_mip_o.get();
        const bool nmi = dut.rvfi_ext_nmi_o.get() != 0;
        const bool nmi_int = dut.rvfi_ext_nmi_int_o.get() != 0;

        if (irq_only) {
            ++env_.counters.irq_only_items;
            env_.cosim.set_nmi(nmi);
            env_.cosim.set_nmi_int(nmi_int);
            env_.cosim.set_mip(pre_mip, pre_mip);
            return;
        }

        const uint64_t order = dut.rvfi_order_o.get();
        while (!iside_error_queue_.empty() &&
               iside_error_queue_.front().order < order) {
            iside_error_queue_.pop_front();
        }
        if (!iside_error_queue_.empty() &&
            iside_error_queue_.front().order == order) {
            env_.cosim.set_iside_error(iside_error_queue_.front().addr);
            iside_error_queue_.pop_front();
            ++env_.counters.iside_errors;
        }

        // The order of these five matters and is upstream's: debug, then nmi,
        // then the interrupt, so that the co-simulator resolves the priority
        // the same way the core does when they arrive together.
        env_.cosim.set_debug_req(dut.rvfi_ext_debug_req_o.get() != 0);
        env_.cosim.set_nmi(nmi);
        env_.cosim.set_nmi_int(nmi_int);
        env_.cosim.set_mip(pre_mip, post_mip);
        env_.cosim.set_mcycle(dut.rvfi_ext_mcycle_o.get());

        const auto counters = dut.rvfi_ext_mhpmcounters_o.get();
        const auto countersh = dut.rvfi_ext_mhpmcountersh_o.get();
        for (std::size_t index = 0; index < kNumPerfCounters; ++index) {
            env_.cosim.set_csr(kCsrMhpmCounter3 + static_cast<uint32_t>(index),
                               counters.word(index));
            env_.cosim.set_csr(kCsrMhpmCounter3H + static_cast<uint32_t>(index),
                               countersh.word(index));
        }
        env_.cosim.set_ic_scr_key_valid(
            dut.rvfi_ext_ic_scr_key_valid_o.get() != 0);

        const bool trap = dut.rvfi_trap_o.get() != 0;
        if (trap) ++env_.counters.traps;
        ++env_.counters.retired;
        ++env_.counters.cosim_steps;

        if (!env_.cosim.step(dut.rvfi_rd_addr_o.get(),
                             dut.rvfi_rd_wdata_o.get(),
                             dut.rvfi_pc_rdata_o.get(), trap,
                             dut.rvfi_ext_rf_wr_suppress_o.get() != 0)) {
            const std::string message = env_.cosim.error_string();
            if (env_.relax_cosim_check) {
                std::printf("cpptb-core-ibex: %s", message.c_str());
                return;
            }
            test_.expect(message, false);
            env_.finish(Ending::kCosimMismatch, message);
        }
    }

    void on_double_fault_pulse() { double_fault_pulse_seen_ = true; }

   private:
    struct IsideError {
        uint64_t order;
        uint32_t addr;
    };
    enum class Pruning { kIdle, kArmed, kWaiting };

    TestContext& test_;
    Env& env_;
    std::unordered_map<uint32_t, bool> failed_iside_;
    std::unordered_map<uint32_t, bool> iside_pmp_failure_;
    std::deque<IsideError> iside_error_queue_;
    uint64_t latest_order_ = ~uint64_t{0};
    Pruning pruning_ = Pruning::kIdle;

    bool double_fault_pulse_seen_ = false;
    uint32_t double_fault_total_ = 0;
    uint32_t double_fault_consecutive_ = 0;
    uint64_t double_fault_items_ = 0;
};

// The cosim agent's three monitors and the double-fault detector's latch, at
// one posedge each. Upstream these are six forked processes reading five
// clocking blocks; running them in one coroutine fixes an order that UVM leaves
// to the scheduler, and the order chosen is the order the data flows.
Task<void> cosim_monitor(Dut dut, Env& env, CosimScoreboard& scoreboard) {
    bool last_double_fault = false;
    bool double_fault_wait = false;
    while (true) {
        co_await RisingEdge{dut.clk_i};

        // core_ibex_scoreboard's `@(posedge double_fault_seen)` followed by
        // wait_clks(1).
        const bool double_fault = dut.double_fault_seen_o.get() != 0;
        if (double_fault_wait) {
            double_fault_wait = false;
        } else if (double_fault && !last_double_fault) {
            scoreboard.on_double_fault_pulse();
            double_fault_wait = true;
        }
        last_double_fault = double_fault;

        // ibex_ifetch_monitor
        if (dut.ifetch_valid_o.get() != 0 && dut.ifetch_ready_o.get() != 0) {
            scoreboard.on_ifetch(dut.ifetch_addr_o.get(),
                                 dut.ifetch_rdata_o.get(),
                                 dut.ifetch_err_o.get() != 0,
                                 dut.ifetch_err_plus2_o.get() != 0);
        }

        // ibex_ifetch_pmp_monitor
        if (dut.ifetch_pmp_valid_o.get() != 0) {
            scoreboard.on_ifetch_pmp(dut.ifetch_pmp_addr_o.get(),
                                     dut.ifetch_pmp_err_o.get() != 0);
        }

        scoreboard.on_id_stage(dut.instr_mon_rvfi_id_done_o.get() != 0,
                               dut.instr_mon_rvfi_order_id_o.get(),
                               dut.instr_mon_pc_id_o.get(),
                               dut.instr_mon_is_compressed_id_o.get() != 0,
                               dut.wb_exception_o.get() != 0);

        // ibex_rvfi_monitor
        if (dut.rvfi_valid_o.get() != 0 ||
            dut.rvfi_ext_irq_valid_o.get() != 0) {
            scoreboard.on_rvfi(dut);
        }
    }
}

// ---------------------------------------------------------------------------
// core_ibex_mcounteren_lock_test::send_stimulus
//
// The one directed entry that does not run under core_ibex_base_test. It
// snoops the CSR interface for a write to MCYCLE, which the program uses to say
// "lock mcounteren now", and for a write to MCYCLEH to unlock it again.
// ---------------------------------------------------------------------------

// core_ibex_mcounteren_lock_test::wait_for_live_csr_write. CSR_OP_READ is
// 2'b00, so "not a read" is what the class tests for.
Task<void> wait_for_csr_write(Dut dut, uint32_t number) {
    while (true) {
        co_await RisingEdge{dut.clk_i};
        if (dut.csr_access_o.get() != 0 && dut.csr_addr_o.get() == number &&
            dut.csr_op_o.get() != 0) {
            co_return;
        }
    }
}

Task<void> mcounteren_lock_stimulus(Dut dut) {
    co_await wait_for_csr_write(dut, kCsrMcycle);
    co_await FallingEdge{dut.clk_i};
    dut.mcounteren_writable_i.set(kIbexMuBiOff);
    std::printf("cpptb-core-ibex: write to MCYCLE, locking mcounteren\n");
    co_await wait_for_csr_write(dut, kCsrMcycleH);
    co_await FallingEdge{dut.clk_i};
    dut.mcounteren_writable_i.set(kIbexMuBiOn);
    std::printf("cpptb-core-ibex: write to MCYCLEH, unlocking mcounteren\n");
}

// ---------------------------------------------------------------------------
// Replay
//
// ports/core_ibex_uvm records what its environment drove into the DUT and what
// the DUT drove back, one line per posedge; this drives the same inputs at the
// same pins and checks that the DUT answers with the same outputs on the same
// cycles. Two harnesses running independent random streams can only be compared
// on whether they reach the same verdict, which is a statement about two
// programs; this is a statement about one run, cycle by cycle.
//
// It also puts this port's co-simulation scoreboard on the baseline's stimulus,
// which is the one thing neither direction had done: every instruction the
// baseline retired, checked against Spike by the code in this file.
//
// The recording is made by the baseline and replayed here, which is the
// direction that tests the port against the reference. Every value in it is
// read in the Active region of a posedge -- what the design samples at that
// edge -- so the inputs are driven at the drive point before that edge and the
// outputs are compared at the edge itself.
// ---------------------------------------------------------------------------

// Bit positions in the two packed fields of a `C` line, as the recording's own
// header names them. A change to either side has to be a change to both.
enum : uint32_t {
    kInRstN = 0,
    kInInstrGnt = 1,
    kInInstrRvalid = 2,
    kInInstrErr = 3,
    kInInstrBadIntg = 4,
    kInDataGnt = 5,
    kInDataRvalid = 6,
    kInDataErr = 7,
    kInDataBadIntg = 8,
    kInKeyValid = 9,
    kInInstrIntgKnown = 10,
    kInDataIntgKnown = 11,
};

enum : uint32_t {
    kOutInstrReq = 0,
    kOutDataReq = 1,
    kOutDataWe = 2,
    kOutRvfiValid = 3,
    kOutRvfiTrap = 4,
    kOutDoubleFault = 5,
    kOutAlertMinor = 6,
    kOutAlertMajorInternal = 7,
    kOutAlertMajorBus = 8,
    kOutCoreSleep = 9,
    kOutKeyReq = 10,
};

struct ReplayKey {
    Bits<128> key{};
    uint64_t nonce = 0;
};

struct ReplayCycle {
    uint32_t in = 0;
    uint32_t ctl = 0;
    uint32_t instr_rdata = 0;
    uint32_t data_rdata = 0;
    uint32_t out = 0;
    uint32_t instr_addr = 0;
    uint32_t data_addr = 0;
    uint32_t data_be = 0;
    uint32_t data_wdata = 0;
    uint64_t rvfi_order = 0;
    uint32_t rvfi_pc = 0;
    uint32_t rvfi_rd = 0;
    uint32_t rvfi_rd_wdata = 0;
    // Index into Trace::keys, or -1 for "the key did not change on this cycle".
    int key = -1;
};

struct Trace {
    std::vector<ReplayCycle> cycles;
    std::vector<ReplayKey> keys;
    uint64_t intg_unknown = 0;
};

Bits<128> parse_key(const char* text) {
    Bits<128> key{};
    // 32 hex digits, most significant first, in four 32-bit words.
    for (std::size_t word = 0; word < 4; ++word) {
        char chunk[9] = {};
        std::memcpy(chunk, text + word * 8, 8);
        key.set_word(3 - word,
                     static_cast<uint32_t>(std::strtoul(chunk, nullptr, 16)));
    }
    return key;
}

Trace read_trace(const std::string& path, std::string& error) {
    Trace trace;
    std::FILE* file = std::fopen(path.c_str(), "r");
    if (file == nullptr) {
        error = "cannot open the recording " + path;
        return trace;
    }
    char line[512];
    int pending_key = -1;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (line[0] == '#') continue;
        if (line[0] == 'K') {
            unsigned long long cycle = 0;
            char key_text[64] = {};
            unsigned long long nonce = 0;
            if (std::sscanf(line, "K %llu %32s %llx", &cycle, key_text,
                            &nonce) != 3) {
                error = "malformed K line in " + path;
                break;
            }
            ReplayKey key;
            key.key = parse_key(key_text);
            key.nonce = nonce;
            trace.keys.push_back(key);
            pending_key = static_cast<int>(trace.keys.size()) - 1;
            continue;
        }
        if (line[0] != 'C') continue;
        ReplayCycle cycle;
        unsigned long long number = 0;
        unsigned long long order = 0;
        if (std::sscanf(line,
                        "C %llu %x %x %x %x %x %x %x %x %x %llx %x %x %x",
                        &number, &cycle.in, &cycle.ctl, &cycle.instr_rdata,
                        &cycle.data_rdata, &cycle.out, &cycle.instr_addr,
                        &cycle.data_addr, &cycle.data_be, &cycle.data_wdata,
                        &order, &cycle.rvfi_pc, &cycle.rvfi_rd,
                        &cycle.rvfi_rd_wdata) != 14) {
            error = "malformed C line in " + path;
            break;
        }
        cycle.rvfi_order = order;
        cycle.key = pending_key;
        pending_key = -1;
        // The recording says whether the integrity bits it saw really were the
        // encoding of the data or its inverse. The wrapper can only drive those
        // two, so anything else is a response this port cannot reproduce and is
        // worth reporting rather than silently approximating.
        if ((((cycle.in >> kInInstrRvalid) & 1u) != 0 &&
             ((cycle.in >> kInInstrIntgKnown) & 1u) == 0) ||
            (((cycle.in >> kInDataRvalid) & 1u) != 0 &&
             ((cycle.in >> kInDataIntgKnown) & 1u) == 0)) {
            ++trace.intg_unknown;
        }
        trace.cycles.push_back(cycle);
    }
    std::fclose(file);
    if (trace.cycles.empty() && error.empty()) {
        error = "the recording " + path + " has no cycles in it";
    }
    return trace;
}

void apply_cycle(Dut dut, const Trace& trace, std::size_t index) {
    const ReplayCycle& cycle = trace.cycles[index];
    dut.rst_ni.set((cycle.in >> kInRstN) & 1u);
    dut.instr_gnt_i.set((cycle.in >> kInInstrGnt) & 1u);
    dut.instr_rvalid_i.set((cycle.in >> kInInstrRvalid) & 1u);
    dut.instr_err_i.set((cycle.in >> kInInstrErr) & 1u);
    dut.instr_bad_intg_i.set((cycle.in >> kInInstrBadIntg) & 1u);
    dut.instr_rdata_i.set(cycle.instr_rdata);
    dut.data_gnt_i.set((cycle.in >> kInDataGnt) & 1u);
    dut.data_rvalid_i.set((cycle.in >> kInDataRvalid) & 1u);
    dut.data_err_i.set((cycle.in >> kInDataErr) & 1u);
    dut.data_bad_intg_i.set((cycle.in >> kInDataBadIntg) & 1u);
    dut.data_rdata_i.set(cycle.data_rdata);
    dut.scramble_key_valid_i.set((cycle.in >> kInKeyValid) & 1u);
    dut.fetch_enable_i.set((cycle.ctl >> 5) & 0xFu);
    dut.mcounteren_writable_i.set((cycle.ctl >> 1) & 0xFu);
    dut.debug_req_i.set(cycle.ctl & 1u);
    if (cycle.key >= 0) {
        dut.scramble_key_i.set(trace.keys[static_cast<std::size_t>(cycle.key)].key);
        dut.scramble_nonce_i.set(
            trace.keys[static_cast<std::size_t>(cycle.key)].nonce);
    }
}

Task<void> replay_run(Dut dut, TestContext& test, Env& env,
                      const Trace& trace) {
    for (std::size_t index = 0; index < trace.cycles.size(); ++index) {
        co_await RisingEdge{dut.clk_i};
        const ReplayCycle& cycle = trace.cycles[index];

        uint32_t out = 0;
        out |= (dut.instr_req_o.get() != 0 ? 1u : 0u) << kOutInstrReq;
        out |= (dut.data_req_o.get() != 0 ? 1u : 0u) << kOutDataReq;
        out |= (dut.data_we_o.get() != 0 ? 1u : 0u) << kOutDataWe;
        out |= (dut.rvfi_valid_o.get() != 0 ? 1u : 0u) << kOutRvfiValid;
        out |= (dut.rvfi_trap_o.get() != 0 ? 1u : 0u) << kOutRvfiTrap;
        out |= (dut.double_fault_seen_o.get() != 0 ? 1u : 0u) << kOutDoubleFault;
        out |= (dut.alert_minor_o.get() != 0 ? 1u : 0u) << kOutAlertMinor;
        out |= (dut.alert_major_internal_o.get() != 0 ? 1u : 0u)
               << kOutAlertMajorInternal;
        out |= (dut.alert_major_bus_o.get() != 0 ? 1u : 0u) << kOutAlertMajorBus;
        out |= (dut.core_sleep_o.get() != 0 ? 1u : 0u) << kOutCoreSleep;
        out |= (dut.scramble_req_o.get() != 0 ? 1u : 0u) << kOutKeyReq;

        const uint32_t instr_addr = dut.instr_addr_o.get();
        const uint32_t data_addr = dut.data_addr_o.get();
        const uint32_t data_be = dut.data_be_o.get();
        const uint32_t data_wdata = dut.data_wdata_o.get();
        const uint64_t rvfi_order = dut.rvfi_order_o.get();
        const uint32_t rvfi_pc = dut.rvfi_pc_rdata_o.get();
        const uint32_t rvfi_rd = dut.rvfi_rd_addr_o.get();
        const uint32_t rvfi_rd_wdata = dut.rvfi_rd_wdata_o.get();

        const bool same =
            out == cycle.out && instr_addr == cycle.instr_addr &&
            data_addr == cycle.data_addr && data_be == cycle.data_be &&
            data_wdata == cycle.data_wdata &&
            rvfi_order == cycle.rvfi_order && rvfi_pc == cycle.rvfi_pc &&
            rvfi_rd == cycle.rvfi_rd && rvfi_rd_wdata == cycle.rvfi_rd_wdata;
        ++env.replay_cycles;
        if (!same) {
            std::printf(
                "cpptb-core-ibex replay divergence at cycle %llu:\n"
                "  recorded out=%03x iaddr=%s daddr=%s be=%x wdata=%s "
                "order=%llu pc=%s rd=%02x rdw=%s\n"
                "  replayed out=%03x iaddr=%s daddr=%s be=%x wdata=%s "
                "order=%llu pc=%s rd=%02x rdw=%s\n",
                static_cast<unsigned long long>(index), cycle.out,
                hex32(cycle.instr_addr).c_str(), hex32(cycle.data_addr).c_str(),
                cycle.data_be, hex32(cycle.data_wdata).c_str(),
                static_cast<unsigned long long>(cycle.rvfi_order),
                hex32(cycle.rvfi_pc).c_str(), cycle.rvfi_rd,
                hex32(cycle.rvfi_rd_wdata).c_str(), out,
                hex32(instr_addr).c_str(), hex32(data_addr).c_str(), data_be,
                hex32(data_wdata).c_str(),
                static_cast<unsigned long long>(rvfi_order),
                hex32(rvfi_pc).c_str(), rvfi_rd,
                hex32(rvfi_rd_wdata).c_str());
            test.expect("the DUT answered the recording's stimulus the way the "
                        "baseline recorded", false);
            env.finish(Ending::kReplayDivergence,
                       "cycle " + std::to_string(index));
            co_return;
        }

        co_await FallingEdge{dut.clk_i};
        if (index + 1 < trace.cycles.size()) {
            apply_cycle(dut, trace, index + 1);
        }
    }
}

// ---------------------------------------------------------------------------
// The program
// ---------------------------------------------------------------------------

std::vector<uint8_t> read_binary(const std::string& path, std::string& error) {
    std::vector<uint8_t> bytes;
    if (path.empty()) {
        error = "IBEX_BIN is not set";
        return bytes;
    }
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        error = "cannot open test binary " + path;
        return bytes;
    }
    uint8_t buffer[65536];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        bytes.insert(bytes.end(), buffer, buffer + read);
    }
    std::fclose(file);
    if (bytes.empty()) error = "test binary " + path + " is empty";
    return bytes;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

// One word per ending, because run_directed.py reads the token that follows
// `outcome=` on a line whose remaining fields are `name=value` pairs. The
// spelling with spaces is the runner's, so the two harnesses' results files
// use the same words for the same outcomes.
const char* ending_name(Ending ending) {
    switch (ending) {
        case Ending::kHandshakePass: return "passed";
        case Ending::kHandshakeFail: return "self-check-failed";
        case Ending::kCosimMismatch: return "cosim-mismatch";
        case Ending::kDoubleFault: return "double-faults";
        case Ending::kCycleTimeout: return "cycle-timeout";
        case Ending::kMalformedHandshake: return "malformed-handshake";
        case Ending::kReplayDivergence: return "replay-divergence";
        default: return "no-verdict";
    }
}

void report(const std::string& name, const Env& env) {
    const Counters& c = env.counters;
    std::printf(
        "cpptb-core-ibex %s outcome=%s cycles=%llu retired=%llu "
        "cosim_steps=%llu cosim_matched=%llu traps=%llu "
        "imem_grants=%llu imem_responses=%llu imem_uninit=%llu "
        "dmem_grants=%llu dmem_responses=%llu dmem_writes=%llu dmem_uninit=%llu "
        "dmem_spurious=%llu ifetches=%llu pmp_ifetch_errors=%llu "
        "iside_errors=%llu irq_only=%llu double_faults=%llu "
        "fetch_enable_pulses=%llu key_answers=%llu signature_writes=%llu\n",
        name.c_str(), ending_name(env.ending),
        static_cast<unsigned long long>(c.cycles),
        static_cast<unsigned long long>(c.retired),
        static_cast<unsigned long long>(c.cosim_steps),
        static_cast<unsigned long long>(env.cosim.insn_count()),
        static_cast<unsigned long long>(c.traps),
        static_cast<unsigned long long>(c.imem_grants),
        static_cast<unsigned long long>(c.imem_responses),
        static_cast<unsigned long long>(c.imem_uninit),
        static_cast<unsigned long long>(c.dmem_grants),
        static_cast<unsigned long long>(c.dmem_responses),
        static_cast<unsigned long long>(c.dmem_writes),
        static_cast<unsigned long long>(c.dmem_uninit),
        static_cast<unsigned long long>(c.dmem_spurious),
        static_cast<unsigned long long>(c.ifetches),
        static_cast<unsigned long long>(c.pmp_ifetch_errors),
        static_cast<unsigned long long>(c.iside_errors),
        static_cast<unsigned long long>(c.irq_only_items),
        static_cast<unsigned long long>(c.double_faults),
        static_cast<unsigned long long>(c.fetch_enable_pulses),
        static_cast<unsigned long long>(c.key_answers),
        static_cast<unsigned long long>(c.signature_writes));
    if (!env.ending_detail.empty()) {
        std::printf("cpptb-core-ibex detail: %s\n", env.ending_detail.c_str());
    }
}

// ---------------------------------------------------------------------------
// core_ibex_base_test
// ---------------------------------------------------------------------------

Task<void> run_core_ibex_test(Dut dut, TestContext& test, const char* name,
                              bool mcounteren_lock) {
    Env env;
    auto& random = test.random();

    env.mcounteren_lock = mcounteren_lock;
    env.signature_addr = static_cast<uint32_t>(std::strtoul(
        env_string("IBEX_SIGNATURE_ADDR", "8ffffffc").c_str(), nullptr, 16));
    // run_directed.py's default. Upstream never overrides
    // core_ibex_base_test's 100,000,000, which is hours on either harness;
    // ports/core_ibex_uvm settled on this and the entry's own timeout_s as the
    // real bound.
    env.timeout_cycles = env_number("IBEX_TIMEOUT_CYCLES", 5'000'000);
    // core_ibex_mcounteren_lock_test sets relax_cosim_check in its build_phase,
    // "so mismatches during lock don't abort".
    env.relax_cosim_check =
        mcounteren_lock || env_number("IBEX_DISABLE_COSIM", 0) != 0;
    env.corrupt_imem_response = env_number("IBEX_CORRUPT_IMEM", 0);
    env.corrupt_dmem_response = env_number("IBEX_CORRUPT_DMEM", 0);

    env.imem.dside = false;
    env.dmem.dside = true;
    // core_ibex_base_test::build_phase: "Never create bad integrity bits in
    // response to accessing uninit memory on the Iside, as the Ibex can fetch
    // speculatively." The dside default is the other way, and is a plusarg
    // upstream.
    env.imem.cfg.enable_bad_intg_on_uninit_access = false;
    env.dmem.cfg.enable_bad_intg_on_uninit_access =
        env_number("IBEX_BAD_INTG_ON_UNINIT", 1) != 0;
    // ibex_mem_intf_response_agent_cfg declares `rand bit zero_delays` with a
    // 50/50 dist, and nothing ever randomizes that object: core_ibex_base_test
    // creates the two configs with type_id::create and writes fields into them,
    // and no `randomize` names either. So `zero_delays` is 0 on every run of the
    // baseline and both delay distributions are always drawn. Drawing it here
    // would be a stimulus difference with nothing behind it.
    env.imem.cfg.zero_delays = env_number("IBEX_ZERO_DELAYS", 0) != 0;
    env.dmem.cfg.zero_delays = env.imem.cfg.zero_delays;
    // core_ibex_vseq::pre_body: spurious dside responses in
    // cfg.spurious_response_pct of runs, and core_ibex_base_test turns them off
    // altogether when the configuration is not SecureIbex.
    //
    // The percentage is a knob because upstream's 20 and the baseline's
    // behaviour are not the same number. That draw is a `dist` through a
    // constrained randomize(), and on Verilator it comes out 1 on 940 of the
    // baseline's 944 directed runs rather than on about 190 of them. So a run
    // that matches upstream's intent and a run that matches what the baseline
    // executes are two different runs, and both are worth being able to make.
    const uint32_t spurious_pct =
        static_cast<uint32_t>(env_number("IBEX_SPURIOUS_DSIDE_PCT", 20));
    env.dmem.enable_spurious_response =
        kSecureIbex && random.randint<uint32_t>(0, 99) < spurious_pct;
    if (env.dmem.enable_spurious_response) {
        env.dmem.spurious_response_delay_cycles = random.randint<uint32_t>(
            env.dmem.cfg.spurious_response_delay_min,
            env.dmem.cfg.spurious_response_delay_max);
        std::printf("cpptb-core-ibex: spurious dside responses enabled\n");
    }

    const std::string binary = env_string("IBEX_BIN");
    std::string load_error;
    const std::vector<uint8_t> image = read_binary(binary, load_error);

    // IBEX_REPLAY names a recording made by ports/core_ibex_uvm with
    // +core_ibex_record. With it set nothing below the monitors generates
    // stimulus: every input comes off the recording and every output is
    // compared against it.
    const std::string replay_prefix = env_string("IBEX_REPLAY");
    env.replay = !replay_prefix.empty();
    Trace trace;
    if (env.replay) {
        std::string trace_error;
        trace = read_trace(replay_prefix + ".pins", trace_error);
        test.expect(trace_error.empty() ? "the recording loaded"
                                        : trace_error.c_str(),
                    trace_error.empty());
        // IBEX_REPLAY_PERTURB moves one bit of the first instruction fetched
        // at or after cycle N, so a run can show the comparison is live rather
        // than merely running. See README.md.
        const uint64_t perturb = env_number("IBEX_REPLAY_PERTURB", 0);
        if (perturb != 0) {
            for (std::size_t index = perturb; index < trace.cycles.size();
                 ++index) {
                if (((trace.cycles[index].in >> kInInstrRvalid) & 1u) == 0) {
                    continue;
                }
                const uint32_t before = trace.cycles[index].instr_rdata;
                trace.cycles[index].instr_rdata ^= 1u;
                std::printf("cpptb-core-ibex replay: the instruction returned "
                            "at cycle %llu was changed from 0x%s to 0x%s\n",
                            static_cast<unsigned long long>(index),
                            hex32(before).c_str(),
                            hex32(trace.cycles[index].instr_rdata).c_str());
                break;
            }
        }
        if (trace.intg_unknown != 0) {
            std::printf("cpptb-core-ibex replay: %llu responses carried "
                        "integrity bits that are neither the encoding of the "
                        "data nor its inverse, which this wrapper cannot "
                        "drive\n",
                        static_cast<unsigned long long>(trace.intg_unknown));
        }
        test.expect("every recorded response carries integrity bits the "
                    "wrapper can reproduce", trace.intg_unknown == 0);
    }

    // ibex_cosim_scoreboard::init_cosim runs in build_phase, before the first
    // clock edge and before anything is loaded.
    if (env_number("IBEX_NO_COSIM", 0) == 0) {
        env.cosim.create(env_string("IBEX_COSIM_LOG"));
    }

    // Reset starts released so that asserting it produces the falling edge the
    // design's asynchronous resets need. ports/core_ibex_uvm found the same
    // thing the hard way: clk_rst_if declares o_rst_n with no initialiser, and
    // on Verilator the assignment to zero is 0 -> 0, no edge, so the core boots
    // in User mode and the first CSR access of every program traps.
    dut.clk_i.set(0);
    dut.rst_ni.set(1);
    dut.fetch_enable_i.set(kIbexMuBiOff);
    // core_ibex_dut_probe_if has an initial block that sets
    //     debug_req = 1'b0; mcounteren_writable = ibex_pkg::IbexMuBiOn;
    // so the counter-enable CSR is writable unless a test says otherwise.
    // Leaving it at zero costs mcounteren_test a cosim mismatch, because
    // ibex_cs_registers gates the write on `mcounteren_writable_i ==
    // IbexMuBiOn` and Spike models no such input.
    dut.mcounteren_writable_i.set(kIbexMuBiOn);
    dut.debug_req_i.set(0);
    dut.instr_gnt_i.set(0);
    dut.instr_rvalid_i.set(0);
    dut.instr_rdata_i.set(0);
    dut.instr_err_i.set(0);
    dut.instr_bad_intg_i.set(0);
    dut.data_gnt_i.set(0);
    dut.data_rvalid_i.set(0);
    dut.data_rdata_i.set(0);
    dut.data_err_i.set(0);
    dut.data_bad_intg_i.set(0);
    dut.scramble_key_valid_i.set(0);
    dut.scramble_key_i.set(Bits<128>{});
    dut.scramble_nonce_i.set(0);
    test.start_clock(dut.clk_i, kClockPeriod);

    if (env.replay && !trace.cycles.empty()) {
        // The recording begins at the first posedge of the baseline's run, by
        // which time its initial block has already driven rst_n. Two idle
        // cycles with rst_ni high first, so applying cycle 0 produces the real
        // falling edge the design's asynchronous resets need.
        co_await clock_cycles(dut.clk_i, 2);
        co_await FallingEdge{dut.clk_i};

        env.mem.write_byte(kBootAddr, 0);
        for (std::size_t offset = 0; offset < image.size(); ++offset) {
            env.mem.write_byte(kBootAddr + static_cast<uint32_t>(offset),
                               image[offset]);
        }
        if (!image.empty()) {
            env.cosim.write_mem(kBootAddr, image.data(), image.size());
        }
        test.expect(load_error.empty() ? "the test binary loaded"
                                       : load_error.c_str(),
                    load_error.empty());

        CosimScoreboard replay_scoreboard(test, env);
        apply_cycle(dut, trace, 0);

        auto replay_imem = test.spawn(bus_monitor(dut, test, env, env.imem));
        auto replay_dmem = test.spawn(bus_monitor(dut, test, env, env.dmem));
        auto replay_cosim =
            test.spawn(cosim_monitor(dut, env, replay_scoreboard));
        co_await replay_run(dut, test, env, trace);
        replay_cosim.cancel();
        replay_dmem.cancel();
        replay_imem.cancel();
        co_await replay_cosim;
        co_await replay_dmem;
        co_await replay_imem;

        env.counters.cycles = env.replay_cycles;
        if (env.ending == Ending::kRunning) {
            env.ending = Ending::kHandshakePass;
        }
        std::printf("cpptb-core-ibex replay: %llu of %llu recorded cycles "
                    "matched\n",
                    static_cast<unsigned long long>(env.replay_cycles),
                    static_cast<unsigned long long>(trace.cycles.size()));
        report(name, env);
        env.cosim.release();
        co_return;
    }

    // clk_rst_if::apply_reset(.reset_width_clks(100)), which core_ibex_tb_top
    // forks from its initial block.
    co_await clock_cycles(dut.clk_i, 2);
    co_await FallingEdge{dut.clk_i};
    dut.rst_ni.set(0);
    co_await clock_cycles(dut.clk_i, 100);
    co_await FallingEdge{dut.clk_i};
    dut.rst_ni.set(1);

    CosimScoreboard scoreboard(test, env);

    auto imem_monitor = test.spawn(bus_monitor(dut, test, env, env.imem));
    auto dmem_monitor = test.spawn(bus_monitor(dut, test, env, env.dmem));
    auto imem_grants = test.spawn(grant_driver(dut, test, env, env.imem));
    auto dmem_grants = test.spawn(grant_driver(dut, test, env, env.dmem));
    auto imem_responses = test.spawn(response_driver(dut, env, env.imem));
    auto dmem_responses = test.spawn(response_driver(dut, env, env.dmem));
    auto keys = test.spawn(key_device(dut, test, env));
    auto cosim = test.spawn(cosim_monitor(dut, env, scoreboard));

    // core_ibex_base_test::run_phase: fetch_enable off, a hundred clocks, the
    // backdoor load into both memory models, then fetch_enable on. The wait
    // runs concurrently with the reset upstream and after it here, which moves
    // the load a hundred cycles later in simulated time and cannot matter: it
    // is a zero-time backdoor write, and the core cannot fetch until
    // fetch_enable goes on after it.
    co_await clock_cycles(dut.clk_i, 100);

    test.expect(load_error.empty() ? "the test binary loaded"
                                   : load_error.c_str(),
                load_error.empty());
    // Unconditional, so hierarchy discovery still records the write path on a
    // run with no program.
    env.mem.write_byte(kBootAddr, 0);
    for (std::size_t offset = 0; offset < image.size(); ++offset) {
        env.mem.write_byte(kBootAddr + static_cast<uint32_t>(offset),
                           image[offset]);
    }
    // ibex_cosim_agent::load_binary_to_mem, which reads the same file. It is
    // given the bytes rather than a read-back of the RTL model on purpose: a
    // read-back would be seeded from the same writes and the two memories would
    // agree even if the load were wrong.
    if (!image.empty()) {
        env.cosim.write_mem(kBootAddr, image.data(), image.size());
    }

    auto fetch_stim = test.spawn(fetch_enable_stimulus(dut, test, env));
    std::optional<Process> mcounteren;
    if (mcounteren_lock) {
        mcounteren.emplace(test.spawn(mcounteren_lock_stimulus(dut)));
    }

    // wait_for_test_done's timeout arm, with the handshake, the double-fault
    // detector and the cosim scoreboard all reaching it through env.finish.
    uint64_t cycles = 0;
    while (env.ending == Ending::kRunning && cycles < env.timeout_cycles) {
        co_await RisingEdge{dut.clk_i};
        ++cycles;
    }
    if (env.ending == Ending::kRunning) {
        env.finish(Ending::kCycleTimeout,
                   "TEST TIMEOUT after " + std::to_string(cycles) + " cycles");
    }

    // The tail of wait_for_test_done: stop the stimulus sequences, wait ten
    // clocks, de-assert fetch enable, and give the instructions still in the
    // pipeline three thousand clocks to retire. The cosim scoreboard is still
    // running over those, and a mismatch in them still fails the run.
    env.test_done = true;
    fetch_stim.cancel();
    co_await fetch_stim;
    if (mcounteren.has_value()) {
        mcounteren->cancel();
        co_await *mcounteren;
    }
    for (uint32_t index = 0; index < 10; ++index) {
        co_await RisingEdge{dut.clk_i};
        ++cycles;
    }
    co_await FallingEdge{dut.clk_i};
    dut.fetch_enable_i.set(kIbexMuBiOff);
    for (uint32_t index = 0; index < 3000; ++index) {
        co_await RisingEdge{dut.clk_i};
        ++cycles;
        if (env.ending == Ending::kCosimMismatch) break;
    }
    env.counters.cycles = cycles;

    cosim.cancel();
    keys.cancel();
    dmem_responses.cancel();
    imem_responses.cancel();
    dmem_grants.cancel();
    imem_grants.cancel();
    dmem_monitor.cancel();
    imem_monitor.cancel();
    co_await cosim;
    co_await keys;
    co_await dmem_responses;
    co_await imem_responses;
    co_await dmem_grants;
    co_await imem_grants;
    co_await dmem_monitor;
    co_await imem_monitor;

    // ibex_cosim_scoreboard::final_phase reports the same number, and it is the
    // evidence the check ran at all: a co-simulation that created Spike and
    // never stepped it reports success exactly as loudly as one that checked
    // every instruction.
    test.expect("the co-simulation stepped the reference model",
                !env.cosim.active() || env.cosim.insn_count() > 0 ||
                    !load_error.empty());
    switch (env.ending) {
        case Ending::kHandshakePass:
            std::printf("cpptb-core-ibex: test done due to RISCV-DV handshake "
                        "(payload=TEST_PASS)\n");
            break;
        case Ending::kHandshakeFail:
            test.expect("the program's own verdict was TEST_PASS", false);
            break;
        case Ending::kCosimMismatch:
            // Already recorded where it happened, with Spike's own words.
            break;
        default:
            test.expect(ending_name(env.ending), false);
            break;
    }
    report(name, env);
    env.cosim.release();
}

// core_ibex_base_test. 943 of the 944 directed entries name this class, and so
// does every riscv-dv entry the two harnesses are compared on.
Task<void> core_ibex_base_test(Dut dut, TestContext& test) {
    co_await run_core_ibex_test(dut, test, "core_ibex_base_test", false);
}

// core_ibex_mcounteren_lock_test: the base test plus a snoop on the CSR
// interface that locks and unlocks mcounteren when the program asks.
Task<void> core_ibex_mcounteren_lock_test(Dut dut, TestContext& test) {
    co_await run_core_ibex_test(dut, test, "core_ibex_mcounteren_lock_test",
                                true);
}

// The longest directed entries run a few hundred thousand cycles and the
// budget above allows five million, which is 100 ms at 20 ns. This sits above
// that so a coroutine that stalls stops here with a wait graph rather than
// running until the harness gives up.
constexpr auto kTestOptions = TestOptions{.simulation_timeout = 200_ms};

CPPTB_REGISTER_TEST_WITH_OPTIONS(core_ibex_base_test, (kTestOptions));
CPPTB_REGISTER_TEST_WITH_OPTIONS(core_ibex_mcounteren_lock_test,
                                 (kTestOptions));

}  // namespace
}  // namespace cpptb::ports::core_ibex
