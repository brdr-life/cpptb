// A cpptb harness for the RISC-V architectural tests on Ibex Simple System.
//
// The CoreMark port next door loads its firmware during elaboration, through
// ibex_simple_system's SRAMInitFile parameter and $readmemh. That works when
// there is one program. Here there are 96, and baking the path into the build
// would mean 96 elaborations of a design that takes minutes to elaborate.
//
// So this loads each program at run time instead, writing it straight into the
// RAM array through cpptb's memory backdoor:
//
//     dut.u_ram.u_ram.mem[index].deposit(word)
//
// One build runs the whole suite. Upstream reaches the same memory through the
// simutil_memload DPI export, driven by MemArea and an ELF reader built on
// libelf; the backdoor is the framework feature that replaces all of it.
//
// The program to run comes from the environment rather than the command line,
// matching the existing convention in tests/conformance/runtime and keeping
// this file free of any Verilator header.
//
//     ACT_FIRMWARE   path to the VMEM to load        (required)
//     ACT_NAME       test name, for the report       (optional)
//     ACT_SIG_BEGIN  signature region, inclusive     (optional, hex or decimal)
//     ACT_SIG_END    signature region, exclusive     (optional)
//
// When the signature bounds are given, the run ends by reading the region back
// through the backdoor and digesting it with the same function the program
// itself uses on the way out. The two are printed side by side. They are
// computed by completely different means -- one by the core executing loads,
// one by the host reading the array -- so agreement is a real check that the
// backdoor sees the same memory the core does, and a mismatch localises the
// problem to the loader rather than the design.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "cpptb/cpptb.hpp"
#include "dut.hpp"

#ifdef CPPTB_COSIM
// Declared here rather than in a header because this is the whole of the
// interface, and cosim_glue.cc is linked only into the co-simulation build. See
// that file for why Spike cannot be seeded when it is constructed.
void cosim_seed_memory(std::uint32_t base_addr, const std::uint8_t* data,
                       std::size_t size);
std::uint64_t cosim_instructions_matched();
#endif

namespace cpptb::ports::act {
namespace {

using cpptb::Dut;
using coro::RisingEdge;
using coro::Task;
using namespace coro;

// Upstream's SimCtrl drives this design at 10ns.
constexpr auto kClockPeriod = 10_ns;

// The longest applicable test runs a few million cycles; most run a few
// hundred thousand. This is far above that, so it catches a program that never
// halts while still failing fast, and sits below the watchdog in cpptb.toml so
// this reports first and can say which test hung.
constexpr uint64_t kDefaultCycleLimit = 10'000'000;

// examples/simple_system/rtl/ibex_simple_system.sv: boot_addr_i is 0x00100000
// and the RAM holds 1024*1024/4 words.
constexpr uint32_t kRamBase = 0x0010'0000;
constexpr uint32_t kRamWords = 1024 * 1024 / 4;

// ibex_pkg::PRIV_LVL_M
constexpr uint32_t kPrivLevelMachine = 3;

// Must match RVMODEL_SIG_DIGEST in target/rvmodel_macros.h exactly:
//   h = 0x811c9dc5; for each word w: h = rotl(h ^ w, 7) + w
class Digest {
   public:
    void update(uint32_t word) {
        uint32_t mixed = hash_ ^ word;
        hash_ = ((mixed << 7) | (mixed >> 25)) + word;
        ++words_;
    }
    [[nodiscard]] uint32_t hash() const { return hash_; }
    [[nodiscard]] uint32_t words() const { return words_; }

   private:
    uint32_t hash_ = 0x811c'9dc5;
    uint32_t words_ = 0;
};

struct Word {
    uint32_t index;
    uint32_t value;
};

// A VMEM is what $readmemh reads: '@' sets the current word address, and every
// other token is one hex word. bin2vmem.py writes a single '@' record followed
// by one word per line, but the format allows more of both and parsing it
// properly costs nothing.
std::vector<Word> read_vmem(const std::string& path, std::string& error) {
    std::vector<Word> words;
    std::ifstream file(path);
    if (!file) {
        error = "cannot open firmware " + path;
        return words;
    }

    uint32_t index = 0;
    std::string token;
    while (file >> token) {
        if (token.empty()) continue;
        if (token[0] == '@') {
            index = static_cast<uint32_t>(
                std::stoul(token.substr(1), nullptr, 16));
            continue;
        }
        if (index >= kRamWords) {
            error = "firmware runs past the end of RAM at word " +
                    std::to_string(index);
            return words;
        }
        words.push_back({index, static_cast<uint32_t>(
                                    std::stoul(token, nullptr, 16))});
        ++index;
    }
    if (words.empty()) error = "firmware " + path + " contains no data";
    return words;
}

std::string env(const char* name, const std::string& fallback = {}) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : fallback;
}

// Word index into the RAM array for a core-visible byte address.
bool ram_index(uint32_t address, uint32_t& index) {
    if (address < kRamBase) return false;
    uint32_t word = (address - kRamBase) / 4;
    if (word >= kRamWords) return false;
    index = word;
    return true;
}

// Everything this testbench touches has to be reached on every path, because
// `cpptb build` compiles it a second time with -DCPPTB_HIERARCHY_DISCOVERY and
// *runs* it to work out which signals are clocks and which hierarchy paths need
// a transport. That run happens at build time, with none of the ACT_* variables
// set, so a testbench that returns early when the environment is empty is
// discovered as one that never starts a clock and never touches memory.
//
// The generated SystemVerilog then comes out with CALENDAR_CLOCK_COUNT = 0 and
// IO_CLK as an output rather than a driven clock, nothing toggles it, and every
// run dies at time zero with "scheduler starvation" -- a message about the
// scheduler for what is really a build-time discovery problem.
//
// So there are no early returns here, a missing firmware is reported and the
// run continues, and the two data-dependent loops are preceded by one
// unconditional access apiece.
// Asynchronous interrupt stimulus, the part of Ibex's UVM flow a self-contained
// program cannot provide for itself. riscv-dv's riscv_irq_* tests exist to be
// interrupted at a point the program does not choose.
//
// ibex_simple_system.sv ties the interrupt pins to constants --
// `.irq_external_i (1'b0)` and the rest -- so they are forced rather than
// driven. Forcing an input of the elaborated core is what force is for, and it
// needs no change to the design.
//
// The co-simulation stays honest through it: Ibex exports the pin state on
// rvfi_ext_pre_mip and rvfi_ext_post_mip, and the checker already forwards
// those to Spike with riscv_cosim_set_mip, so the reference model sees the same
// interrupt the core did. Nothing here has to tell Spike anything.
//
// Off unless ACT_IRQ_PERIOD is set, so every other port keeps its behaviour.
// Bounded, because an unbounded one never lets the program finish: it keeps
// servicing interrupts instead of reaching its exit sequence. Firing a known
// number and then stopping is also what makes a run reproducible.
Task<void> interrupt_stimulus(Dut dut, uint64_t period, uint64_t hold,
                              uint64_t count, uint64_t& fired) {
    for (uint64_t i = 0; i < count; ++i) {
        co_await clock_cycles(dut.IO_CLK, period);
        dut.u_top.u_ibex_top.irq_external_i.force(1);
        co_await clock_cycles(dut.IO_CLK, hold);
        dut.u_top.u_ibex_top.irq_external_i.release();
        ++fired;
    }
}

Task<void> arch_test(Dut dut, TestContext& test) {
    const std::string name = env("ACT_NAME", "riscv-arch-test");
    // Overridable so this testbench can also run upstream's own co-simulation
    // workload, CoreMark, which needs 40.7 million cycles rather than the
    // 190,000 an architectural test does.
    const std::string limit_text = env("ACT_CYCLE_LIMIT");
    const uint64_t cycle_limit = limit_text.empty()
        ? kDefaultCycleLimit
        : std::stoull(limit_text, nullptr, 0);
    const std::string firmware = env("ACT_FIRMWARE");

    std::string error;
    std::vector<Word> image;
    if (firmware.empty()) {
        error = "ACT_FIRMWARE is not set";
    } else {
        image = read_vmem(firmware, error);
    }

    // Start with reset released and then assert it, as upstream's SimCtrl
    // does. The core's flops reset on `negedge rst_ni`, so holding reset low
    // from time zero never produces that edge, and the core boots in user mode
    // where every machine-mode CSR access traps -- which surfaces thousands of
    // cycles later as an illegal instruction and looks nothing like its cause.
    dut.IO_CLK.set_now(0);
    dut.IO_RST_N.set(1);
    test.start_clock(dut.IO_CLK, kClockPeriod);

    co_await clock_cycles(dut.IO_CLK, 2);
    dut.IO_RST_N.set(0);
    co_await clock_cycles(dut.IO_CLK, 4);

    // Load with the core held in reset, so it fetches the program rather than
    // whatever the array powered up holding, and so the writes land while
    // nothing else is driving the memory.
    //
    // The first deposit is outside the loop so that discovery, which sees an
    // empty image, still records the backdoor. It is not wasted work: every
    // image starts at the base of RAM -- build_tests.py refuses to emit one
    // that does not -- so the loop's first iteration overwrites it.
    dut.u_ram.u_ram.mem[0].deposit(0);
    for (const Word& word : image) {
        dut.u_ram.u_ram.mem[static_cast<std::int32_t>(word.index)]
            .deposit(word.value);
    }

#ifdef CPPTB_COSIM
    // Spike has to start from the same bytes the core will fetch. The image is
    // handed over rather than read back through the backdoor: a backdoor read
    // would be seeded from the same writes, so it would agree even if those
    // writes were wrong, and the co-simulation would then compare two
    // identically wrong memories and report nothing. Reading it from the file
    // keeps the check independent.
    if (!image.empty()) {
        const uint32_t first = image.front().index;
        const uint32_t last = image.back().index;
        std::vector<std::uint8_t> bytes((last - first + 1) * 4, 0);
        for (const Word& word : image) {
            const size_t at = static_cast<size_t>(word.index - first) * 4;
            bytes[at + 0] = static_cast<std::uint8_t>(word.value);
            bytes[at + 1] = static_cast<std::uint8_t>(word.value >> 8);
            bytes[at + 2] = static_cast<std::uint8_t>(word.value >> 16);
            bytes[at + 3] = static_cast<std::uint8_t>(word.value >> 24);
        }
        cosim_seed_memory(kRamBase + first * 4, bytes.data(), bytes.size());
    }
#endif

    dut.IO_RST_N.set(1);

    test.expect(error.empty() ? "firmware loaded" : error.c_str(),
                error.empty());

    uint64_t interrupts_fired = 0;
    // Spawned rather than joined: it never returns, and the run ends when the
    // program halts.
    const std::string irq_period = env("ACT_IRQ_PERIOD");
    if (!irq_period.empty()) {
        const uint64_t period = std::stoull(irq_period, nullptr, 0);
        const std::string hold_text = env("ACT_IRQ_HOLD", "5");
        const uint64_t count =
            std::stoull(env("ACT_IRQ_COUNT", "20"), nullptr, 0);
        test.spawn(interrupt_stimulus(dut, period,
                                      std::stoull(hold_text, nullptr, 0),
                                      count, interrupts_fired));
        std::printf("cpptb: driving irq_external_i %llu times, every %llu "
                    "cycles\n", static_cast<unsigned long long>(count),
                    static_cast<unsigned long long>(period));
    }

    test.expect_eq("core boots in machine mode",
                   dut.u_top.u_ibex_top.u_ibex_core.cs_registers_i
                       .priv_lvl_q.get().signal_value(),
                   kPrivLevelMachine);

    // The program ends by writing bit 0 of the simulator_ctrl register, which
    // raises sim_finish and then issues $finish from RTL. Watching that
    // register lets this finish on its own terms and report a result, rather
    // than being cut off mid-await when the simulator stops.
    uint64_t cycles = 0;
    bool finished = false;
    while (cycles < cycle_limit) {
        co_await RisingEdge{dut.IO_CLK};
        ++cycles;
        if (dut.u_simulator_ctrl.sim_finish.get() != 0) {
            finished = true;
            break;
        }
    }

    test.expect("program signalled completion", finished);
    test.expect("completed within the cycle limit", cycles < cycle_limit);

    // Read the signature back out and digest it the same way the program did.
    const std::string begin_text = env("ACT_SIG_BEGIN");
    const std::string end_text = env("ACT_SIG_END");
    // Unconditional, for the same discovery reason as the deposit above.
    (void)dut.u_ram.u_ram.mem[0].get();
    if (finished && !begin_text.empty() && !end_text.empty()) {
        const auto begin = static_cast<uint32_t>(
            std::stoul(begin_text, nullptr, 0));
        const auto end = static_cast<uint32_t>(std::stoul(end_text, nullptr, 0));

        uint32_t first = 0;
        uint32_t last = 0;
        const bool mapped = ram_index(begin, first) && ram_index(end, last);
        test.expect("signature region lies within RAM", mapped);
        if (mapped) {
            Digest digest;
            for (uint32_t index = first; index < last; ++index) {
                digest.update(dut.u_ram.u_ram.mem[
                    static_cast<std::int32_t>(index)].get());
            }
            // Printed for run_suite.py to compare against the ACT-SIG line the
            // program wrote to the log. Same value, arrived at two ways.
            std::printf("cpptb: backdoor-sig %08x %08x\n", digest.hash(),
                        digest.words());
        }
    }

#ifdef CPPTB_COSIM
    const std::uint64_t matched = cosim_instructions_matched();
    test.expect("the co-simulation actually stepped the reference model",
                matched > 0);
    std::printf("cpptb: co-simulation matched %llu instructions\n",
                static_cast<unsigned long long>(matched));
#endif

    if (interrupts_fired > 0) {
        std::printf("cpptb: fired %llu interrupts\n",
                    static_cast<unsigned long long>(interrupts_fired));
    }
    std::printf("cpptb: %s ran %llu cycles\n", name.c_str(),
                static_cast<unsigned long long>(cycles));
}

CPPTB_REGISTER_TEST(arch_test);

}  // namespace
}  // namespace cpptb::ports::act
