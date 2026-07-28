// A cpptb port of Ibex's icache UVM testbench, dv/uvm/icache.
//
// ports/ibex_icache_uvm runs that environment unmodified on Verilator and all
// ten of its tests pass. This drives the same design with the same stimulus
// shape and the same scoreboard, so a disagreement between the two is a defect
// in one of the harnesses rather than a difference of opinion about the cache.
//
// What is here, and what it corresponds to upstream:
//
//   core_stimulus    ibex_icache_core_base_seq       branch/fetch/enable/
//                                                    invalidate/new-seed items
//   drive_branch,    ibex_icache_core_driver         the core-side pins
//   drive_req,       + ibex_icache_core_if
//   read_insn
//   core_monitor     ibex_icache_core_monitor        bus items to the scoreboard
//   mem_monitor      ibex_icache_mem_monitor         grants and responses
//   mem_responder,   ibex_icache_mem_driver          gnt, rvalid, rdata, err
//   mem_grant_driver + ibex_icache_mem_resp_seq
//   key_device       push_pull_agent, Pull/Device    the scrambling key
//   MemoryModel      ibex_icache_mem_model           seed to data and errors
//   Scoreboard       ibex_icache_scoreboard          check_compatible, the
//                                                    address sequence, busy,
//                                                    and the caching ratio
//
// Three tests are registered, named as upstream names them:
// ibex_icache_smoke, ibex_icache_passthru and ibex_icache_caching. They are the
// base virtual sequence and the two that turn the cache off and on. The other
// seven are not ported; see README.md for what each of them would need.
//
// The stimulus arithmetic is not transcribed from upstream's constraints but
// from ports/ibex_icache_uvm's build_tb.py, which replaced every `dist` in the
// environment with a direct draw of the same buckets at the same weights
// because Verilator gets `dist` wrong in three separate ways. That is the
// stimulus the baseline actually ran, so it is the stimulus to match.
//
// Timing convention. A "drive point" is the instant just after a falling edge
// of clk_i. The design samples on the rising edge, so a value written at a
// drive point is captured by the next rising edge and by no earlier one, which
// is what upstream's `default output negedge` clocking blocks produce. Every
// task below that drives a pin is entered at a drive point and returns at a
// drive point, and none of them opens with a wait: a task that re-anchored
// itself would place its first write one cycle later than the UVM driver does.
// Monitors read on the rising edge, where co_await RisingEdge resumes before
// the design has evaluated it, which is the same value `@(posedge clk)` sees in
// the Active region.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "cpptb/cpptb.hpp"
#include "dut.hpp"

namespace cpptb::ports::icache {
namespace {

using cpptb::Dut;
using cpptb::TestContext;
using namespace cpptb::coro;

// ibex_icache_env_cfg constrains clk_freq_mhz to 50.
constexpr auto kClockPeriod = 20_ns;

// dv_base_vseq's num_trans_c: `num_trans inside {[800:1000]}`.
constexpr uint32_t kMinTrans = 800;
constexpr uint32_t kMaxTrans = 1000;

// ---------------------------------------------------------------------------
// ibex_icache_mem_model, with BusWidth = 32
// ---------------------------------------------------------------------------

constexpr uint32_t kBusBytes = 4;

// The first integer hash from https://burtleburtle.net/bob/hash/integer.html,
// which is what upstream uses.
uint32_t hash32(uint32_t in) {
    in = (in ^ 61u) ^ (in >> 16);
    in = in + (in << 3);
    in = in ^ (in >> 4);
    in = in * 0x27d4eb2du;
    return in ^ (in >> 15);
}

uint32_t read_word(uint32_t seed, uint32_t word_address) {
    return hash32((word_address & 0x3fff'ffffu) ^ seed);
}

// ibex_icache_mem_model::read_data. Its loop runs twice for a misaligned
// address, but the accumulator is BusWidth bits wide and the second word is
// shifted left by 32 before being ORed in, so at BusWidth = 32 the second
// iteration contributes nothing and the result is the aligned word. The
// scoreboard handles misalignment itself, with two calls at two addresses.
uint32_t read_data(uint32_t seed, uint32_t address) {
    return read_word(seed, address >> 2);
}

// True if reading kBusBytes from address intersects the error range implied by
// the seed. All arithmetic wraps at 32 bits, as it does upstream.
bool is_error(uint32_t seed, uint32_t address, uint32_t error_shift) {
    const uint32_t rng_lo = seed ^ 0xdead'beefu;
    const uint32_t rng_w0 =
        error_shift >= 32 ? 0u : static_cast<uint32_t>(1u << (32 - error_shift));
    const uint32_t rng_w1 = 0xffff'ffffu - rng_lo;
    const uint32_t rng_hi = rng_lo + std::min(rng_w0, rng_w1);
    return (address < rng_hi) &&
           (rng_lo < static_cast<uint32_t>(address + kBusBytes));
}

bool is_mem_error(bool no_mem_errs, uint32_t seed, uint32_t address,
                  uint32_t error_shift) {
    return !no_mem_errs && is_error(seed, address ^ 0xf00d'beefu, error_shift);
}

// ---------------------------------------------------------------------------
// ibex_icache_scoreboard
// ---------------------------------------------------------------------------

struct MemState {
    uint32_t seed = 0;
    uint32_t err_shift = 0;
};

struct Counters {
    // Stimulus, so that a comparison against the UVM baseline can be
    // normalised per item rather than resting on two different draws of
    // num_trans.
    uint64_t items = 0;
    uint64_t branch_items = 0;
    uint64_t insns_requested = 0;

    uint64_t fetches = 0;
    uint64_t fetch_errors = 0;
    uint64_t branches = 0;
    uint64_t invalidations = 0;
    uint64_t enable_edges = 0;
    uint64_t busy_edges = 0;
    uint64_t new_seeds = 0;
    uint64_t mem_grants = 0;
    uint64_t mem_responses = 0;
    uint64_t mem_response_errors = 0;
    uint64_t windows_completed = 0;
    uint64_t windows_checked = 0;
    uint64_t possible_old = 0;
    uint64_t actual_old = 0;
    uint64_t cycles = 0;
};

class Scoreboard {
   public:
    Scoreboard(TestContext& test, bool disable_mem_errs, uint32_t mem_err_shift,
               bool disable_caching_ratio_test)
        : test_(test),
          no_mem_errs_(disable_mem_errs),
          mem_err_shift_(mem_err_shift),
          disable_caching_ratio_test_(disable_caching_ratio_test) {
        mem_states_.push_back({0, mem_err_shift_});
        window_reset();
    }

    Counters counters;

    // ibex_icache_scoreboard::process_branch
    void on_branch(uint32_t address) {
        ++counters.branches;
        next_addr_ = address;

        if (invalidate_seed_ > 0) {
            // An invalidation was seen recently. Clear out expired seeds.
            if (invalidate_seed_ < mem_states_.size()) {
                mem_states_.erase(
                    mem_states_.begin(),
                    mem_states_.begin() +
                        static_cast<std::ptrdiff_t>(invalidate_seed_));
            }
            invalidate_seed_ = 0;
        }
        last_branch_seed_ = mem_states_.size() - 1;
        if (!enabled_) no_cache_ = true;
    }

    // ibex_icache_scoreboard::process_fetch
    void on_fetch(uint32_t address, uint32_t insn_data, bool err,
                  bool err_plus2) {
        ++counters.fetches;
        if (err) ++counters.fetch_errors;

        if (next_addr_.has_value()) {
            test_.expect_eq("fetch address matches the expected address",
                            address, *next_addr_);
        }
        next_addr_ = address + (((insn_data & 3u) == 3u) ? 4u : 2u);

        check_compatible(address, insn_data, err, err_plus2);
        window_take_insn(address, err);
    }

    // ibex_icache_scoreboard::process_invalidate
    void on_invalidate() {
        ++counters.invalidations;
        invalidate_seed_ = mem_states_.size() - 1;
        not_invalidating_ = false;
        window_reset();
    }

    // ibex_icache_scoreboard::process_enable
    void on_enable(bool enable) {
        ++counters.enable_edges;
        enabled_ = enable;
        if (enabled_) no_cache_ = false;
    }

    // ibex_icache_scoreboard::process_busy
    void on_busy(bool busy) {
        ++counters.busy_edges;
        busy_ = busy;
        if (!busy_) {
            busy_check();
            not_invalidating_ = true;
        }
    }

    // ibex_icache_scoreboard::process_mem_fifo, grant half
    void on_mem_grant() {
        ++counters.mem_grants;
        ++mem_trans_count_;
        if (window_enabled()) ++reads_in_window_;
    }

    // ibex_icache_scoreboard::process_mem_fifo, response half
    void on_mem_response(bool err) {
        ++counters.mem_responses;
        if (err) ++counters.mem_response_errors;
        busy_check();
        test_.expect("a memory response arrived with a request outstanding",
                     mem_trans_count_ > 0);
        if (mem_trans_count_ > 0) --mem_trans_count_;
    }

    // ibex_icache_scoreboard::process_seed_fifo
    void on_new_seed(uint32_t seed) {
        ++counters.new_seeds;
        mem_states_.push_back({seed, mem_err_shift_});
    }

   private:
    // is_fetch_compatible_1. The chatty second pass upstream makes on a
    // failure is a diagnostic, not a check; the failure message here carries
    // the address and the number of seeds tried instead.
    bool is_fetch_compatible_1(uint32_t seen_insn_data, bool seen_err,
                               bool exp_err, uint32_t exp_insn_data) const {
        if (exp_err) return seen_err;
        if (seen_err) return false;
        const bool is_compressed = (exp_insn_data & 3u) != 3u;
        if (is_compressed) {
            return (seen_insn_data & 0xffffu) == (exp_insn_data & 0xffffu);
        }
        return seen_insn_data == exp_insn_data;
    }

    // is_fetch_compatible_2
    bool is_fetch_compatible_2(uint32_t seen_insn_data, bool seen_err_plus2,
                               bool exp_err_lo, bool exp_err_hi,
                               uint32_t exp_insn_data) const {
        if (exp_err_lo) return false;
        if (exp_err_hi != seen_err_plus2) return false;
        if (exp_err_hi) return true;
        return exp_insn_data == seen_insn_data;
    }

    // is_state_compatible_1
    bool is_state_compatible_1(uint32_t address, uint32_t seen_insn_data,
                               bool seen_err, const MemState& state) const {
        const uint32_t addr_lo = address & ~3u;
        const uint32_t lo_bits_to_drop = 8u * (address - addr_lo);
        const uint32_t rdata =
            lo_bits_to_drop >= 32u
                ? 0u
                : static_cast<uint32_t>(read_data(state.seed, addr_lo) >>
                                        lo_bits_to_drop);
        return is_fetch_compatible_1(
            seen_insn_data, seen_err,
            is_mem_error(no_mem_errs_, state.seed, addr_lo, state.err_shift),
            rdata);
    }

    // is_state_compatible_2
    bool is_state_compatible_2(uint32_t address, uint32_t seen_insn_data,
                               bool seen_err_plus2, const MemState& lo,
                               const MemState& hi) const {
        const uint32_t addr_lo = address & ~3u;
        const uint32_t addr_hi = addr_lo + kBusBytes;
        const uint32_t lo_bits_to_take = 8u * (address - addr_lo);
        const uint32_t lo_bits_to_drop = 32u - lo_bits_to_take;

        const bool exp_err_lo =
            is_mem_error(no_mem_errs_, lo.seed, addr_lo, lo.err_shift);
        uint32_t exp_data =
            lo_bits_to_drop >= 32u
                ? 0u
                : static_cast<uint32_t>(read_data(lo.seed, addr_lo) >>
                                        lo_bits_to_drop);
        const bool exp_err_hi =
            is_mem_error(no_mem_errs_, hi.seed, addr_hi, hi.err_shift);
        exp_data |= lo_bits_to_take >= 32u
                        ? 0u
                        : static_cast<uint32_t>(read_data(hi.seed, addr_hi)
                                                << lo_bits_to_take);
        return is_fetch_compatible_2(seen_insn_data, seen_err_plus2, exp_err_lo,
                                     exp_err_hi, exp_data);
    }

    bool check_compatible_1(uint32_t address, uint32_t seen_insn_data,
                            bool seen_err, size_t min_idx) {
        for (size_t index = min_idx; index < mem_states_.size(); ++index) {
            if (is_state_compatible_1(address, seen_insn_data, seen_err,
                                      mem_states_[index])) {
                last_fetch_age_ = mem_states_.size() - 1 - index;
                return true;
            }
        }
        return false;
    }

    bool check_compatible_2(uint32_t address, uint32_t seen_insn_data,
                            bool seen_err_plus2, size_t min_idx) {
        // The diagonal first: a hit there is much the likelier case.
        for (size_t index = min_idx; index < mem_states_.size(); ++index) {
            if (is_state_compatible_2(address, seen_insn_data, seen_err_plus2,
                                      mem_states_[index], mem_states_[index])) {
                last_fetch_age_ = mem_states_.size() - 1 - index;
                return true;
            }
        }
        for (size_t i = min_idx; i < mem_states_.size(); ++i) {
            for (size_t j = min_idx; j < mem_states_.size(); ++j) {
                if (i == j) continue;
                if (is_state_compatible_2(address, seen_insn_data,
                                          seen_err_plus2, mem_states_[i],
                                          mem_states_[j])) {
                    last_fetch_age_ = mem_states_.size() - 1 - std::min(i, j);
                    return true;
                }
            }
        }
        return false;
    }

    // ibex_icache_scoreboard::check_compatible
    void check_compatible(uint32_t address, uint32_t insn_data, bool err,
                          bool err_plus2) {
        const bool misaligned = (address & 3u) != 0u;
        const bool good_bottom_word = !err || err_plus2;
        const bool uncompressed = (insn_data & 3u) == 3u;

        const size_t min_idx = no_cache_ ? last_branch_seed_ : 0;
        if (min_idx >= mem_states_.size()) {
            test_.expect(
                "the scoreboard has a seed to check this fetch against", false);
            return;
        }

        // If the configured mem_err_shift no longer matches the back of the
        // list, append an entry carrying the same seed and the new shift.
        // Nothing in the three ported tests changes it, but the combo
        // sequences do.
        if (mem_states_.back().err_shift != mem_err_shift_) {
            mem_states_.push_back({mem_states_.back().seed, mem_err_shift_});
        }

        const bool ok =
            (misaligned && good_bottom_word && uncompressed)
                ? check_compatible_2(address, insn_data, err && err_plus2,
                                     min_idx)
                : check_compatible_1(address, insn_data, err, min_idx);

        if (ok) {
            test_.expect("fetch data is compatible with an available seed",
                         true);
        } else {
            const std::string message =
                "fetch at 0x" + hex32(address) + " returned 0x" +
                hex32(insn_data) + " (err " + (err ? "1" : "0") + "/" +
                (err_plus2 ? "1" : "0") +
                "), which is compatible with none of the " +
                std::to_string(mem_states_.size() - min_idx) +
                " available seeds";
            test_.expect(message, false);
        }

        if (mem_states_.size() > min_idx + 1) {
            ++counters.possible_old;
            if (last_fetch_age_ > 0) ++counters.actual_old;
        }
    }

    // busy_check
    void busy_check() {
        if (mem_trans_count_ > 0) {
            test_.expect(
                "busy is high while memory transactions are outstanding",
                busy_);
        }
    }

    void window_reset() {
        insns_in_window_ = 0;
        reads_in_window_ = 0;
        window_range_lo_ = 0xffff'ffffu;
        window_range_hi_ = 0;
    }

    bool window_enabled() const { return enabled_ && not_invalidating_; }

    // window_take_insn
    void window_take_insn(uint32_t address, bool err) {
        if (err || disable_caching_ratio_test_) {
            window_reset();
            return;
        }
        if (window_enabled()) {
            ++insns_in_window_;
            window_range_lo_ = std::min(window_range_lo_, address);
            window_range_hi_ = std::max(window_range_hi_, address);
        }
        if (insns_in_window_ < kWindowLen) return;

        ++counters.windows_completed;
        const uint64_t window_width =
            (static_cast<uint64_t>(window_range_hi_) - window_range_lo_ + 3) / 4;
        if (window_width <= kMaxWindowWidth) {
            ++counters.windows_checked;
            const uint64_t fetch_ratio_pc =
                (static_cast<uint64_t>(reads_in_window_) * 100 * 53 / 32) /
                insns_in_window_;
            test_.expect("caching ratio within the window is at most 67%",
                         fetch_ratio_pc <= 67);
        }
        window_reset();
    }

    static std::string hex32(uint32_t value) {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%08x", value);
        return std::string(buffer);
    }

    // The reasoning behind 250 and 850 is a long comment in
    // ibex_icache_scoreboard.sv; the numbers are copied from it.
    static constexpr uint32_t kMaxWindowWidth = 250;
    static constexpr uint32_t kWindowLen = 850;

    TestContext& test_;
    bool no_mem_errs_;
    uint32_t mem_err_shift_;
    bool disable_caching_ratio_test_;

    std::vector<MemState> mem_states_;
    std::optional<uint32_t> next_addr_;
    size_t invalidate_seed_ = 0;
    size_t last_branch_seed_ = 0;
    size_t last_fetch_age_ = 0;
    uint32_t mem_trans_count_ = 0;
    bool enabled_ = false;
    bool no_cache_ = true;
    bool busy_ = false;

    bool not_invalidating_ = false;
    uint32_t insns_in_window_ = 0;
    uint32_t reads_in_window_ = 0;
    uint32_t window_range_lo_ = 0xffff'ffffu;
    uint32_t window_range_hi_ = 0;
};

// ---------------------------------------------------------------------------
// Shared testbench state
// ---------------------------------------------------------------------------

struct Response {
    bool err = false;
    uint32_t rdata = 0;
    uint32_t delay = 0;
};

struct Env {
    Scoreboard& scoreboard;
    bool disable_mem_errs = false;
    uint32_t mem_err_shift = 3;

    // ibex_icache_mem_driver::rdata_queue
    std::deque<Response> responses;
    // The seed_fifo the core driver's analysis port feeds, drained at a grant
    // exactly as ibex_icache_mem_resp_seq::take_gnt drains it.
    std::vector<uint32_t> pending_seeds;
    uint32_t cur_seed = 0;

    // The value the core monitor last sampled on valid_o. This is what
    // `driver_cb.valid` holds, and the core driver's wait_valid needs the
    // sampled value rather than the live one.
    bool sampled_valid = false;

    void push_seed(uint32_t seed) {
        scoreboard.on_new_seed(seed);
        pending_seeds.push_back(seed);
    }
};

// ---------------------------------------------------------------------------
// Draws
//
// Every one of these reproduces a `dist` that ports/ibex_icache_uvm draws with
// $urandom_range for the same reason: the same buckets and the same weights.
// ---------------------------------------------------------------------------

// ibex_icache_core_base_seq::draw_base_addr
uint32_t draw_base_addr(Random& random) {
    uint32_t value = 0;
    const uint32_t pick = random.randint<uint32_t>(0, 3);
    if (pick == 0) {
        value = random.randint<uint32_t>(0, 15);
    } else if (pick == 3) {
        value = random.randint<uint32_t>(0xffff'fff0u, 0xffff'ffffu);
    } else {
        value = random.randint<uint32_t>(16, 0xffff'fff0u);
    }
    return value & ~1u;
}

// The unconstrained branch target: three buckets, and half-word aligned.
uint32_t draw_branch_addr(Random& random) {
    uint32_t value = 0;
    const uint32_t pick = random.randint<uint32_t>(0, 21);
    if (pick == 0) {
        value = random.randint<uint32_t>(0, 63);
    } else if (pick == 21) {
        value = random.randint<uint32_t>(0xffff'ffc0u, 0xffff'ffffu);
    } else {
        value = random.randint<uint32_t>(64, 0xffff'ffbfu);
    }
    return value & ~1u;
}

// ibex_icache_core_driver::drive_req_trans, req_low_cycles
uint32_t draw_req_low_cycles(Random& random, bool allow_no_low_cycles) {
    const uint32_t w_zero = allow_no_low_cycles ? 20u : 0u;
    const uint32_t total = w_zero + 5 + 2 + 1;
    const uint32_t pick = random.randint<uint32_t>(0, total - 1);
    if (pick < w_zero) return 0;
    if (pick < w_zero + 5) return random.randint<uint32_t>(1, 33);
    if (pick < w_zero + 7) return random.randint<uint32_t>(100, 200);
    return random.randint<uint32_t>(1000, 1200);
}

// ibex_icache_mem_driver::drive_grant, with min 0 and max 10
uint32_t draw_gnt_delay(Random& random) {
    constexpr uint32_t kMin = 0;
    constexpr uint32_t kMax = 10;
    const uint32_t pick = random.randint<uint32_t>(0, 4);
    if (pick < 3) return kMin;
    if (pick < 4) return random.randint<uint32_t>(kMin + 1, kMax - 1);
    return kMax;
}

// ibex_icache_mem_resp_item::draw_delay, with min 0, mid 5 and max 50
uint32_t draw_response_delay(Random& random) {
    const uint32_t pick = random.randint<uint32_t>(0, 10);
    if (pick < 5) return 0;
    if (pick < 10) return random.randint<uint32_t>(1, 5);
    return random.randint<uint32_t>(6, 50);
}

// ---------------------------------------------------------------------------
// The core-side driver
// ---------------------------------------------------------------------------

// ibex_icache_core_if::wait_clks, without the stop-on-reset fork: none of the
// three ported tests resets the DUT part way through.
Task<void> wait_clks(Dut dut, uint32_t count) {
    if (count == 0) co_return;
    co_await clock_cycles(dut.clk_i, count);
    co_await FallingEdge{dut.clk_i};
}

// ibex_icache_core_if::wait_valid. Upstream writes `wait (driver_cb.valid)`,
// which reads the value sampled at the most recent clocking event and so takes
// no time at all if valid was already high there. env.sampled_valid is that
// value, and this only ever reads it from a drive point, by which time the
// monitor has already run for the preceding edge.
Task<void> wait_valid(Dut dut, Env& env) {
    while (!env.sampled_valid) {
        co_await RisingEdge{dut.clk_i};
        co_await FallingEdge{dut.clk_i};
    }
}

// ibex_icache_core_driver::read_insn. Returns the error flag sampled on the
// edge where the fetch was taken, which is what read_insns stops on.
Task<bool> read_insn(Dut dut, TestContext& test, Env& env) {
    auto& random = test.random();

    // One time in ten, wait for valid before even considering ready.
    if (random.randint<uint32_t>(0, 9) == 0) co_await wait_valid(dut, env);
    co_await wait_clks(dut, random.randint<uint32_t>(0, 3));

    dut.ready_i.set(1);
    bool err = false;
    while (true) {
        co_await RisingEdge{dut.clk_i};
        if (dut.valid_o.get() != 0) {
            err = dut.err_o.get() != 0;
            break;
        }
    }
    co_await FallingEdge{dut.clk_i};
    dut.ready_i.set(0);
    co_return err;
}

// ibex_icache_core_driver::read_insns
Task<void> read_insns(Dut dut, TestContext& test, Env& env, uint32_t count,
                      bool& saw_error) {
    for (uint32_t index = 0; index < count; ++index) {
        if (co_await read_insn(dut, test, env)) {
            saw_error = true;
            break;
        }
    }
}

// ibex_icache_core_driver::invalidate, through invalidate_pulse
Task<void> maybe_invalidate(Dut dut, TestContext& test, bool wanted) {
    if (!wanted) co_return;
    auto& random = test.random();
    // dist { 1 :/ 10, [2:20] :/ 1 }
    const uint32_t cycles = random.randint<uint32_t>(0, 10) < 10
                                ? 1u
                                : random.randint<uint32_t>(2, 20);
    dut.invalidate_i.set(1);
    co_await wait_clks(dut, cycles);
    dut.invalidate_i.set(0);
}

// ibex_icache_core_driver::lower_req
Task<void> lower_req(Dut dut, uint32_t cycles) {
    if (cycles == 0) co_return;
    dut.req_i.set(0);
    co_await wait_clks(dut, cycles);
    dut.req_i.set(1);
}

struct Item {
    bool branch = false;
    uint32_t branch_addr = 0;
    bool enable = false;
    bool invalidate = false;
    uint32_t new_seed = 0;
    uint32_t num_insns = 0;
};

// ibex_icache_core_if::branch_to, then the fetches that follow it
Task<void> branch_then_read(Dut dut, TestContext& test, Env& env,
                            const Item item, bool& saw_error) {
    dut.branch_i.set(1);
    dut.branch_addr_i.set(item.branch_addr);
    co_await RisingEdge{dut.clk_i};
    co_await FallingEdge{dut.clk_i};
    dut.branch_i.set(0);
    dut.ready_i.set(0);
    co_await read_insns(dut, test, env, item.num_insns, saw_error);
}

// ibex_icache_core_driver::drive_branch_trans
Task<void> drive_branch(Dut dut, TestContext& test, Env& env, const Item item,
                        bool& saw_error) {
    dut.req_i.set(1);
    dut.enable_i.set(item.enable ? 1 : 0);
    // Ready is allowed to be high across a branch and should have no effect,
    // so let it happen occasionally.
    dut.ready_i.set(test.random().randint<uint32_t>(0, 16) == 0 ? 1 : 0);
    if (item.new_seed != 0) env.push_seed(item.new_seed);

    co_await Join{branch_then_read(dut, test, env, item, saw_error),
                  maybe_invalidate(dut, test, item.invalidate)};
}

// ibex_icache_core_driver::drive_req_trans
Task<void> drive_req(Dut dut, TestContext& test, Env& env, const Item item,
                     bool& saw_error) {
    dut.enable_i.set(item.enable ? 1 : 0);
    if (item.new_seed != 0) env.push_seed(item.new_seed);

    const uint32_t low_cycles =
        draw_req_low_cycles(test.random(), item.num_insns > 0);
    co_await Join{lower_req(dut, low_cycles),
                  maybe_invalidate(dut, test, item.invalidate)};
    co_await read_insns(dut, test, env, item.num_insns, saw_error);
}

// ---------------------------------------------------------------------------
// The core-side stimulus, ibex_icache_core_base_seq
// ---------------------------------------------------------------------------

struct SeqConfig {
    bool constrain_branches = false;
    bool initial_enable = false;
    bool const_enable = false;
    bool avoid_invalidation = false;
    uint32_t gap_between_invalidations = 49;
    uint32_t gap_between_toggle_enable = 49;
};

Task<void> core_stimulus(Dut dut, TestContext& test, Env& env,
                         const SeqConfig cfg, uint32_t num_trans) {
    auto& random = test.random();

    const uint32_t base_addr = draw_base_addr(random);
    const uint32_t top_restricted_addr =
        base_addr <= 0xffff'ffffu - 64 ? base_addr + 64 : 0xffff'ffffu;

    bool force_branch = true;
    bool cache_enabled = cfg.initial_enable;
    bool stale_seed = false;
    // Only the combo sequences ever set this, and neither of them is ported.
    const bool must_invalidate = false;
    uint32_t insns_since_branch = 0;

    co_await FallingEdge{dut.clk_i};

    // run_reqs runs num_trans - 1 items.
    for (uint32_t index = 0; index + 1 < num_trans; ++index) {
        if (cfg.constrain_branches && insns_since_branch >= 100) {
            force_branch = true;
        }

        Item item;
        // trans_type carries no dist and no constraint but force_branch, so an
        // unconstrained solve is a uniform pick over the two enum values.
        item.branch = force_branch || random.randint<uint32_t>(0, 1) == 0;

        if (item.branch) {
            if (cfg.constrain_branches) {
                // `branch_addr inside {[base_addr:top_restricted_addr]}` with
                // the item's own `!branch_addr[0]`, so a uniform pick over the
                // even addresses in the range.
                const uint32_t steps = (top_restricted_addr - base_addr) / 2;
                item.branch_addr =
                    base_addr + 2 * random.randint<uint32_t>(0, steps);
            } else {
                item.branch_addr = draw_branch_addr(random);
            }
        }

        item.enable =
            cfg.const_enable
                ? cache_enabled
                : (random.randint<uint32_t>(
                       0, cfg.gap_between_toggle_enable) == 0
                       ? !cache_enabled
                       : cache_enabled);

        if (must_invalidate) {
            item.invalidate = true;
        } else if (stale_seed && item.enable) {
            item.invalidate = true;
        } else if (cfg.avoid_invalidation) {
            item.invalidate = false;
        } else {
            item.invalidate =
                random.randint<uint32_t>(0, cfg.gap_between_invalidations) == 0;
        }

        // new_seed dist { 0 :/ 1, nonzero :/ W }. The zero weight when the
        // cache is enabled is what keeps a new seed away from an enabled
        // cache, which the scoreboard would see as a multi-way hit.
        const uint32_t seed_weight =
            item.invalidate ? 1000u : (item.enable ? 0u : 1u);
        if (seed_weight == 0 || random.randint<uint32_t>(0, seed_weight) == 0) {
            item.new_seed = 0;
        } else {
            item.new_seed = random.randint<uint32_t>(1, 0xffff'ffffu);
        }

        // num_insns: the dist's bucket applied as a preference, the support it
        // implied as a hard bound, and constrain_branches' cap on top. A
        // bucket the cap rules out is dropped rather than failing the draw,
        // which is what `soft` does upstream.
        uint32_t bucket_lo = 0;
        uint32_t bucket_hi = 0;
        if (item.branch) {
            const uint32_t pick = random.randint<uint32_t>(0, 25);
            if (pick < 5) {
                bucket_lo = 0;
                bucket_hi = 0;
            } else if (pick < 25) {
                bucket_lo = 1;
                bucket_hi = 20;
            } else {
                bucket_lo = 21;
                bucket_hi = 100;
            }
        } else {
            const uint32_t pick = random.randint<uint32_t>(0, 20);
            if (pick < 1) {
                bucket_lo = 0;
                bucket_hi = 0;
            } else {
                bucket_lo = 1;
                bucket_hi = 20;
            }
        }
        uint32_t cap = item.branch ? 100u : 20u;
        if (cfg.constrain_branches && !item.branch) {
            cap = std::min(cap, 100u - insns_since_branch);
        }
        const uint32_t narrowed_hi = std::min(bucket_hi, cap);
        item.num_insns = bucket_lo <= narrowed_hi
                             ? random.randint<uint32_t>(bucket_lo, narrowed_hi)
                             : random.randint<uint32_t>(0, cap);

        ++env.scoreboard.counters.items;
        if (item.branch) ++env.scoreboard.counters.branch_items;
        env.scoreboard.counters.insns_requested += item.num_insns;

        bool saw_error = false;
        if (item.branch) {
            co_await drive_branch(dut, test, env, item, saw_error);
        } else {
            co_await drive_req(dut, test, env, item, saw_error);
        }

        cache_enabled = item.enable;
        if (item.invalidate) {
            stale_seed = false;
        } else if (item.new_seed != 0) {
            stale_seed = true;
        }
        force_branch = saw_error;
        insns_since_branch =
            (item.branch ? 0u : insns_since_branch) + item.num_insns;
    }
}

// ---------------------------------------------------------------------------
// Monitors
// ---------------------------------------------------------------------------

// ibex_icache_core_monitor::collect_trans. Outputs are collected before
// inputs, so that a branch asserted on the same cycle as a fetch is seen by the
// scoreboard after that fetch and it needs no reordering.
Task<void> core_monitor(Dut dut, Env& env) {
    bool last_inval = false;
    bool last_enable = false;
    bool last_busy = false;

    while (true) {
        co_await RisingEdge{dut.clk_i};
        ++env.scoreboard.counters.cycles;

        const bool valid = dut.valid_o.get() != 0;
        env.sampled_valid = valid;
        const bool ready = dut.ready_i.get() != 0;
        const bool branch = dut.branch_i.get() != 0;

        // Anything coming back while branch is asserted is ignored: the cache
        // is being redirected at the end of the cycle.
        if (ready && valid && !branch) {
            env.scoreboard.on_fetch(dut.addr_o.get(), dut.rdata_o.get(),
                                    dut.err_o.get() != 0,
                                    dut.err_plus2_o.get() != 0);
        }

        const bool enable = dut.enable_i.get() != 0;
        if (enable != last_enable) env.scoreboard.on_enable(enable);
        last_enable = enable;

        const bool busy = dut.busy_o.get() != 0;
        if (busy != last_busy) env.scoreboard.on_busy(busy);
        last_busy = busy;

        if (branch) env.scoreboard.on_branch(dut.branch_addr_i.get());

        const bool inval = dut.invalidate_i.get() != 0;
        if (inval && !last_inval) env.scoreboard.on_invalidate();
        last_inval = inval;
    }
}

// ibex_icache_mem_monitor, with ibex_icache_mem_resp_seq::take_gnt folded in:
// the response for a grant is decided at the grant, from the seed in force at
// that moment, which is what keeps one fetch tied to exactly one seed.
Task<void> mem_monitor(Dut dut, TestContext& test, Env& env) {
    while (true) {
        co_await RisingEdge{dut.clk_i};

        if (dut.instr_req_o.get() != 0 && dut.instr_gnt_i.get() != 0) {
            for (const uint32_t seed : env.pending_seeds) env.cur_seed = seed;
            env.pending_seeds.clear();

            const uint32_t address = dut.instr_addr_o.get();
            Response response;
            response.err = is_mem_error(env.disable_mem_errs, env.cur_seed,
                                        address, env.mem_err_shift);
            // Upstream drives 'X with the error. Verilator has no X, so the
            // baseline drives zero here too; a wrong fetch that returned this
            // without the error flag would still fail the scoreboard, because
            // the seed hash is what the data is checked against.
            response.rdata =
                response.err ? 0u : read_data(env.cur_seed, address);
            response.delay = draw_response_delay(test.random());
            env.responses.push_back(response);
            env.scoreboard.on_mem_grant();
        }

        if (dut.instr_rvalid_i.get() != 0) {
            env.scoreboard.on_mem_response(dut.instr_err_i.get() != 0);
        }
    }
}

// ---------------------------------------------------------------------------
// The memory-side driver
// ---------------------------------------------------------------------------

// ibex_icache_mem_driver::drive_grant. The grant line toggles independently of
// every other signal on the bus, and stays high for two consecutive cycles
// when the drawn delay is zero, exactly as it does upstream.
Task<void> mem_grant_driver(Dut dut, TestContext& test) {
    co_await FallingEdge{dut.clk_i};
    while (true) {
        const uint32_t delay = draw_gnt_delay(test.random());
        co_await wait_clks(dut, delay);
        dut.instr_gnt_i.set(1);
        co_await RisingEdge{dut.clk_i};
        co_await FallingEdge{dut.clk_i};
        dut.instr_gnt_i.set(0);
    }
}

// ibex_icache_mem_driver::drive_responses, as a per-cycle state machine rather
// than a blocking get from a mailbox. The timing is the same: a response
// granted at edge N drives rvalid for exactly one cycle, at edge N + delay + 1,
// and responses are serialised in the order they were granted.
Task<void> mem_responder(Dut dut, Env& env) {
    bool driving = false;
    bool busy = false;
    Response current;
    uint32_t countdown = 0;

    while (true) {
        co_await FallingEdge{dut.clk_i};

        if (driving) {
            dut.instr_rvalid_i.set(0);
            dut.instr_err_i.set(0);
            dut.instr_rdata_i.set(0);
            driving = false;
            busy = false;
        }
        if (!busy && !env.responses.empty()) {
            current = env.responses.front();
            env.responses.pop_front();
            busy = true;
            countdown = current.delay;
        }
        if (busy) {
            if (countdown == 0) {
                dut.instr_rvalid_i.set(1);
                dut.instr_err_i.set(current.err ? 1 : 0);
                dut.instr_rdata_i.set(current.rdata);
                driving = true;
            } else {
                --countdown;
            }
        }
    }
}

// The scrambling key provider. Upstream is a push_pull_agent in Pull/Device
// mode whose d_data is randomised per transaction, so the key, the nonce and
// the valid bit are all random and a request is answered only when the valid
// bit happens to come up set. This answers every request with a valid key
// after a randomised delay instead, which is the same handshake with a bounded
// response time. The key and nonce are random either way, and neither the
// design's behaviour on the core interface nor the scoreboard depends on them.
Task<void> key_device(Dut dut, TestContext& test) {
    while (true) {
        co_await FallingEdge{dut.clk_i};
        if (dut.scr_key_req_o.get() == 0) continue;

        co_await wait_clks(dut, test.random().randint<uint32_t>(0, 10));
        dut.scr_key_i.set(test.random().randbits<128>());
        dut.scr_nonce_i.set(test.random().next_u64());
        dut.scr_key_valid_i.set(1);
        co_await RisingEdge{dut.clk_i};
        co_await FallingEdge{dut.clk_i};
        dut.scr_key_valid_i.set(0);
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void report(TestContext& test, const char* name, const Counters& counters) {
    // One machine-readable line, so run_tests.py can put these beside the
    // numbers it counts out of the UVM baseline's log.
    std::printf(
        "cpptb-icache %s items=%llu branch_items=%llu insns_requested=%llu "
        "fetches=%llu fetch_errors=%llu branches=%llu "
        "invalidations=%llu enable_edges=%llu busy_edges=%llu new_seeds=%llu "
        "mem_grants=%llu mem_responses=%llu mem_response_errors=%llu "
        "windows_completed=%llu windows_checked=%llu possible_old=%llu "
        "actual_old=%llu cycles=%llu\n",
        name, static_cast<unsigned long long>(counters.items),
        static_cast<unsigned long long>(counters.branch_items),
        static_cast<unsigned long long>(counters.insns_requested),
        static_cast<unsigned long long>(counters.fetches),
        static_cast<unsigned long long>(counters.fetch_errors),
        static_cast<unsigned long long>(counters.branches),
        static_cast<unsigned long long>(counters.invalidations),
        static_cast<unsigned long long>(counters.enable_edges),
        static_cast<unsigned long long>(counters.busy_edges),
        static_cast<unsigned long long>(counters.new_seeds),
        static_cast<unsigned long long>(counters.mem_grants),
        static_cast<unsigned long long>(counters.mem_responses),
        static_cast<unsigned long long>(counters.mem_response_errors),
        static_cast<unsigned long long>(counters.windows_completed),
        static_cast<unsigned long long>(counters.windows_checked),
        static_cast<unsigned long long>(counters.possible_old),
        static_cast<unsigned long long>(counters.actual_old),
        static_cast<unsigned long long>(counters.cycles));
    test.expect("the sequence issued at least one fetch", counters.fetches > 0);
}

Task<void> run_icache_test(Dut dut, TestContext& test, const char* name,
                           SeqConfig cfg) {
    // Every pin is touched here, unconditionally and before anything else.
    // `cpptb build` compiles this file a second time with
    // -DCPPTB_HIERARCHY_DISCOVERY and runs it to learn which signals are
    // clocks and which paths need a transport, so a signal only reached down a
    // conditional branch can be missed.
    dut.clk_i.set(0);
    dut.rst_ni.set(1);
    dut.req_i.set(0);
    dut.branch_i.set(0);
    dut.branch_addr_i.set(0);
    dut.ready_i.set(0);
    dut.enable_i.set(0);
    dut.invalidate_i.set(0);
    dut.instr_gnt_i.set(0);
    dut.instr_rvalid_i.set(0);
    dut.instr_rdata_i.set(0);
    dut.instr_err_i.set(0);
    dut.scr_key_valid_i.set(0);
    dut.scr_key_i.set(Bits<128>{});
    dut.scr_nonce_i.set(0);
    test.start_clock(dut.clk_i, kClockPeriod);

    Scoreboard scoreboard(test, /*disable_mem_errs=*/false,
                          /*mem_err_shift=*/3,
                          /*disable_caching_ratio_test=*/false);
    Env env{scoreboard};

    // dv_base_vseq::dut_init, through clk_rst_if::apply_reset. Reset starts
    // released so that asserting it produces the negedge the design's
    // asynchronous resets need.
    co_await clock_cycles(dut.clk_i, 2);
    dut.rst_ni.set(0);
    co_await clock_cycles(dut.clk_i, 5);
    co_await FallingEdge{dut.clk_i};
    dut.rst_ni.set(1);

    // The core monitor is started first so that, when a core item and a memory
    // item land on the same edge, the scoreboard sees them in the order its
    // two forked processes see them upstream.
    auto monitor_core = test.spawn(core_monitor(dut, env));
    auto monitor_mem = test.spawn(mem_monitor(dut, test, env));
    auto grants = test.spawn(mem_grant_driver(dut, test));
    auto responses = test.spawn(mem_responder(dut, env));
    auto keys = test.spawn(key_device(dut, test));

    const uint32_t num_trans =
        test.random().randint<uint32_t>(kMinTrans, kMaxTrans);
    co_await core_stimulus(dut, test, env, cfg, num_trans);

    // ibex_icache_base_vseq::body ends by killing every sequence still running
    // once the core sequence has run all its items.
    keys.cancel();
    responses.cancel();
    grants.cancel();
    monitor_mem.cancel();
    monitor_core.cancel();
    co_await keys;
    co_await responses;
    co_await grants;
    co_await monitor_mem;
    co_await monitor_core;

    report(test, name, scoreboard.counters);
}

// ibex_icache_base_vseq: everything at its default, which means the enable
// line toggles, seeds change and the cache is invalidated from time to time.
// It proves the environment runs and that every fetch returned correct data.
// It completes no caching-ratio window, because toggling the enable line
// resets the window, so it says nothing at all about caching.
Task<void> ibex_icache_smoke(Dut dut, TestContext& test) {
    co_await run_icache_test(dut, test, "ibex_icache_smoke", SeqConfig{});
}

// ibex_icache_passthru_vseq: branch targets constrained to a 64-byte window
// and the cache held disabled, so every fetch goes to memory.
Task<void> ibex_icache_passthru(Dut dut, TestContext& test) {
    SeqConfig cfg;
    cfg.constrain_branches = true;
    cfg.initial_enable = false;
    cfg.const_enable = true;
    co_await run_icache_test(dut, test, "ibex_icache_passthru", cfg);
}

// ibex_icache_caching_vseq: the same window with the cache held enabled and
// invalidation avoided, which is what lets the caching-ratio windows complete.
Task<void> ibex_icache_caching(Dut dut, TestContext& test) {
    SeqConfig cfg;
    cfg.constrain_branches = true;
    cfg.initial_enable = true;
    cfg.const_enable = true;
    cfg.avoid_invalidation = true;
    co_await run_icache_test(dut, test, "ibex_icache_caching", cfg);
}

// 100 ms of simulated time is about 5,000,000 cycles at 20 ns, and the
// longest of the three runs about 80,000. A sequence that stalls stops here
// with a wait graph rather than running until the harness gives up.
CPPTB_REGISTER_TEST_WITH_OPTIONS(ibex_icache_smoke,
                                 (TestOptions{.simulation_timeout = 100_ms}));
CPPTB_REGISTER_TEST_WITH_OPTIONS(ibex_icache_passthru,
                                 (TestOptions{.simulation_timeout = 100_ms}));
CPPTB_REGISTER_TEST_WITH_OPTIONS(ibex_icache_caching,
                                 (TestOptions{.simulation_timeout = 100_ms}));

}  // namespace
}  // namespace cpptb::ports::icache
