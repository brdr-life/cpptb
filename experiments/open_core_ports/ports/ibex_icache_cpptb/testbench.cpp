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
//                    + ibex_icache_core_back_line_seq invalidate/new-seed items
//   drive_branch,    ibex_icache_core_driver         the core-side pins
//   drive_req,       + ibex_icache_core_if
//   read_insn
//   core_monitor     ibex_icache_core_monitor        bus items to the scoreboard
//   mem_monitor      ibex_icache_mem_monitor         grants and responses
//   mem_responder,   ibex_icache_mem_driver          gnt, rvalid, rdata, err
//   mem_grant_driver + ibex_icache_mem_resp_seq
//   key_device       push_pull_agent, Pull/Device    the scrambling key
//   ecc_corrupter    ibex_icache_ram_if              RAM read-data corruption
//   MemoryModel      ibex_icache_mem_model           seed to data and errors
//   Scoreboard       ibex_icache_scoreboard          check_compatible, the
//                                                    address sequence, busy,
//                                                    the caching ratio and the
//                                                    reset hooks
//   combo_body       ibex_icache_combo_vseq          child sequences back to
//                                                    back, with reset between
//
// All ten tests are registered, named as upstream names them, each built from
// the same virtual sequence knobs its ibex_icache_*_vseq sets.
//
// The stimulus arithmetic is not transcribed from upstream's constraints but
// from ports/ibex_icache_uvm's build_tb.py, which replaced every `dist` in the
// environment with a direct draw of the same buckets at the same weights
// because Verilator gets `dist` wrong in three separate ways. That is the
// stimulus the baseline actually ran, so it is the stimulus to match. Where
// the two disagree the disagreement is called out in a comment; see RESULTS.md.
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
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
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

// ibex_pkg, with the icache's default 4 kB two-way configuration:
// IC_NUM_WAYS is 2, TagSizeECC is IC_TAG_SIZE + 6 = 28 and LineSizeECC is
// (BUS_SIZE + 7) * IC_LINE_BEATS = 78. These are the widths of the mask ports
// on ibex_icache_tb_top, so a change to any of them stops this compiling
// rather than quietly corrupting the wrong bits.
constexpr std::size_t kNumWays = 2;
constexpr std::size_t kTagSizeEcc = 28;
constexpr std::size_t kBusSizeEcc = 39;
constexpr std::size_t kLineBeats = 2;
constexpr std::size_t kLineSizeEcc = kBusSizeEcc * kLineBeats;

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

std::string hex32(uint32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%08x", value);
    return std::string(buffer);
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
    // The combo sequences: how many child sequences ran and how many of them
    // were preceded by a reset.
    uint64_t child_sequences = 0;
    uint64_t resets = 0;
    uint64_t killed_sequences = 0;
    // ibex_icache_ram_if: RAM reads whose data was corrupted on the way to the
    // cache, and cycles on which the cache reported an ECC error.
    uint64_t ecc_injections = 0;
    uint64_t ecc_errors = 0;
    // The scrambling key: requests answered, and answers that carried the
    // valid bit clear so the request had to be made again.
    uint64_t key_answers = 0;
    uint64_t key_refusals = 0;
    uint64_t cycles = 0;
};

class Scoreboard {
   public:
    Scoreboard(TestContext& test, bool disable_mem_errs, uint32_t mem_err_shift)
        : test_(test),
          no_mem_errs_(disable_mem_errs),
          mem_err_shift_(mem_err_shift) {
        mem_states_.push_back({0, mem_err_shift_});
        tracking_reset();
    }

    Counters counters;

    // cfg.mem_agent_cfg.mem_err_shift, which ibex_icache_base_vseq::pre_start
    // writes for each sequence it starts.
    void set_mem_err_shift(uint32_t value) { mem_err_shift_ = value; }

    // cfg.disable_caching_ratio_test, which ibex_icache_ecc_vseq::body sets
    // around its child and restores afterwards.
    void set_disable_caching_ratio_test(bool value) {
        disable_caching_ratio_test_ = value;
    }

    // ICACHE_KEEP_STATE_ON_RESET disables the two hooks below, which is how a
    // run can show they are doing work rather than merely running; see
    // README.md.
    void set_keep_state_on_reset(bool value) { keep_state_on_reset_ = value; }

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

    // ibex_icache_scoreboard::start_reset, which monitor_negedge_reset calls on
    // every falling edge of rst_n. Everything the cache was holding is gone, so
    // the expected address is unknown again, only the newest seed can still be
    // returned, and the memory bus has no outstanding transactions.
    void start_reset() {
        ++counters.resets;
        if (keep_state_on_reset_) return;
        next_addr_.reset();
        if (mem_states_.size() > 1) {
            mem_states_.erase(mem_states_.begin(), mem_states_.end() - 1);
        }
        invalidate_seed_ = 0;
        last_branch_seed_ = 0;
        mem_trans_count_ = 0;
    }

    // ibex_icache_scoreboard::reset, which dv_base_scoreboard::monitor_reset
    // calls on the rising edge of rst_n that ends the reset.
    void end_reset() {
        if (keep_state_on_reset_) return;
        tracking_reset();
    }

   private:
    // is_fetch_compatible_1. `why` is upstream's chatty pass: the failure path
    // asks each seed why it did not match rather than making the search twice.
    bool is_fetch_compatible_1(uint32_t seen_insn_data, bool seen_err,
                               bool exp_err, uint32_t exp_insn_data,
                               uint32_t seed, std::string* why) const {
        if (exp_err) {
            if (seen_err) return true;
            if (why != nullptr) {
                *why = "not seed 0x" + hex32(seed) +
                       ": expected seen_err but saw 0";
            }
            return false;
        }
        if (seen_err) {
            if (why != nullptr) {
                *why = "not seed 0x" + hex32(seed) +
                       ": got unexpected error flag";
            }
            return false;
        }
        const bool is_compressed = (exp_insn_data & 3u) != 3u;
        const bool ok =
            is_compressed
                ? (seen_insn_data & 0xffffu) == (exp_insn_data & 0xffffu)
                : seen_insn_data == exp_insn_data;
        if (!ok && why != nullptr) {
            *why = "not seed 0x" + hex32(seed) + " (expected " +
                   (is_compressed ? "cmp" : "uncomp") + " 0x" +
                   hex32(exp_insn_data) + "; saw 0x" + hex32(seen_insn_data) +
                   ")";
        }
        return ok;
    }

    // is_fetch_compatible_2
    bool is_fetch_compatible_2(uint32_t seen_insn_data, bool seen_err_plus2,
                               bool exp_err_lo, bool exp_err_hi,
                               uint32_t exp_insn_data, uint32_t seed_lo,
                               uint32_t seed_hi, std::string* why) const {
        const std::string pair =
            "seeds 0x" + hex32(seed_lo) + "/0x" + hex32(seed_hi);
        if (exp_err_lo) {
            if (why != nullptr) {
                *why = "not " + pair + " (expected error in low word)";
            }
            return false;
        }
        if (exp_err_hi != seen_err_plus2) {
            if (why != nullptr) {
                *why = "not " + pair + " (exp/seen top errors " +
                       (exp_err_hi ? "1" : "0") + "/" +
                       (seen_err_plus2 ? "1" : "0") + ")";
            }
            return false;
        }
        if (exp_err_hi) return true;
        if (exp_insn_data != seen_insn_data) {
            if (why != nullptr) {
                *why = "not " + pair + " (exp/seen data 0x" +
                       hex32(exp_insn_data) + "/0x" + hex32(seen_insn_data) +
                       ")";
            }
            return false;
        }
        return true;
    }

    // is_state_compatible_1
    bool is_state_compatible_1(uint32_t address, uint32_t seen_insn_data,
                               bool seen_err, const MemState& state,
                               std::string* why) const {
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
            rdata, state.seed, why);
    }

    // is_state_compatible_2
    bool is_state_compatible_2(uint32_t address, uint32_t seen_insn_data,
                               bool seen_err_plus2, const MemState& lo,
                               const MemState& hi, std::string* why) const {
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
                                     exp_err_hi, exp_data, lo.seed, hi.seed,
                                     why);
    }

    bool check_compatible_1(uint32_t address, uint32_t seen_insn_data,
                            bool seen_err, size_t min_idx) {
        for (size_t index = min_idx; index < mem_states_.size(); ++index) {
            if (is_state_compatible_1(address, seen_insn_data, seen_err,
                                      mem_states_[index], nullptr)) {
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
                                      mem_states_[index], mem_states_[index],
                                      nullptr)) {
                last_fetch_age_ = mem_states_.size() - 1 - index;
                return true;
            }
        }
        for (size_t i = min_idx; i < mem_states_.size(); ++i) {
            for (size_t j = min_idx; j < mem_states_.size(); ++j) {
                if (i == j) continue;
                if (is_state_compatible_2(address, seen_insn_data,
                                          seen_err_plus2, mem_states_[i],
                                          mem_states_[j], nullptr)) {
                    last_fetch_age_ = mem_states_.size() - 1 - std::min(i, j);
                    return true;
                }
            }
        }
        return false;
    }

    // The chatty pass upstream makes on a failure, which says why each seed did
    // not match. Upstream runs the whole search again with a flag; this asks
    // the same predicates for their reason, which is the same information
    // without a second traversal. Capped, because the list is unbounded and a
    // failure message is no place to print a thousand lines.
    std::string explain(uint32_t address, uint32_t insn_data, bool err,
                        bool err_plus2, size_t min_idx, bool paired) {
        constexpr size_t kMaxLines = 24;
        std::string out;
        size_t printed = 0;
        for (size_t index = min_idx;
             index < mem_states_.size() && printed < kMaxLines; ++index) {
            std::string why;
            if (paired) {
                is_state_compatible_2(address, insn_data, err && err_plus2,
                                      mem_states_[index], mem_states_[index],
                                      &why);
            } else {
                is_state_compatible_1(address, insn_data, err,
                                      mem_states_[index], &why);
            }
            if (why.empty()) continue;
            out += "\n  " + why;
            ++printed;
        }
        const size_t tried = mem_states_.size() - min_idx;
        if (printed < tried) {
            out += "\n  and " + std::to_string(tried - printed) + " more";
        }
        if (paired) {
            out +=
                "\n  (the diagonal only; the pairs off it were tried and none "
                "matched)";
        }
        return out;
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
        // list, append an entry carrying the same seed and the new shift. Only
        // the combo sequences change it, when a child that wants a different
        // error rate starts.
        if (mem_states_.back().err_shift != mem_err_shift_) {
            mem_states_.push_back({mem_states_.back().seed, mem_err_shift_});
        }

        const bool paired = misaligned && good_bottom_word && uncompressed;
        const bool ok =
            paired ? check_compatible_2(address, insn_data, err && err_plus2,
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
                " available seeds:" +
                explain(address, insn_data, err, err_plus2, min_idx, paired);
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

    // tracking_reset
    void tracking_reset() {
        not_invalidating_ = false;
        window_reset();
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

    // The reasoning behind 250 and 850 is a long comment in
    // ibex_icache_scoreboard.sv; the numbers are copied from it.
    static constexpr uint32_t kMaxWindowWidth = 250;
    static constexpr uint32_t kWindowLen = 850;

    TestContext& test_;
    bool no_mem_errs_;
    uint32_t mem_err_shift_;
    bool disable_caching_ratio_test_ = false;
    bool keep_state_on_reset_ = false;

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

// ibex_icache_core_req_item, and the unit of the item stream that
// ports/ibex_icache_uvm records. Declared here rather than beside the core
// driver so that Env can hold a recorded list of them.
struct Item {
    bool branch = false;
    uint32_t branch_addr = 0;
    bool enable = false;
    bool invalidate = false;
    uint32_t new_seed = 0;
    uint32_t num_insns = 0;
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

    // Nonzero to corrupt one bit of the memory response for that grant, so
    // that a run can demonstrate the scoreboard is live rather than merely
    // quiet. Set with ICACHE_CORRUPT_GRANT; see README.md.
    uint64_t corrupt_grant = 0;

    // The value the core monitor last sampled on valid_o. This is what
    // `driver_cb.valid` holds, and the core driver's wait_valid needs the
    // sampled value rather than the live one.
    bool sampled_valid = false;

    // rst_ni is low. Read by everything that upstream writes as a
    // stop-on-reset fork.
    bool in_reset = false;

    // ibex_icache_combo_vseq::run_sequence has killed the running core
    // sequence and is about to reset the DUT.
    bool stimulus_killed = false;

    // Upstream's core driver keeps driving the item it has in hand when the
    // sequence is killed, and unwinds when the reset arrives a fraction of a
    // cycle later. Here the two are one coroutine, so both flags stop it, and
    // each drive task restores the pin it owns on the way out exactly as the
    // upstream task's early return does.
    bool core_stopped() const { return in_reset || stimulus_killed; }

    // ibex_icache_ram_if::enable_ecc_errors, which ibex_icache_ecc_vseq sets
    // and nothing ever clears.
    bool ecc_enabled = false;
    // ibex_icache_ram_if::dis_err_pct, and a knob that swaps the sparse mask
    // for one the cache's SECDED cannot see. See README.md.
    uint32_t ecc_dis_err_pct = 99;
    bool ecc_alias = false;

    // push_pull_agent_cfg's zero_delays and device_delay_max, drawn once for
    // the run because the agent's config object is randomised once.
    bool key_zero_delays = false;
    uint32_t key_delay_max = 0;

    // Replay. ICACHE_REPLAY drives every DUT input from a recording made by
    // ports/ibex_icache_uvm, in which case nothing here generates stimulus and
    // the memory model is not consulted; ICACHE_ITEMS replays only the core
    // item stream and leaves the rest of the environment to draw its own.
    // See README.md.
    bool replay_pins = false;
    const std::vector<Item>* replay_items = nullptr;
    bool replay_diverged = false;

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

// ibex_icache_combo_vseq::run_sequence's cycles_till_reset:
// dist { [100:500] :/ 1, [501:1000] :/ 4 }.
uint32_t draw_cycles_till_reset(Random& random) {
    return random.randint<uint32_t>(0, 4) == 0
               ? random.randint<uint32_t>(100, 500)
               : random.randint<uint32_t>(501, 1000);
}

// push_pull_agent_cfg::device_delay_max_c, the arm taken when zero_delays is
// clear: dist { [1:10] :/ 1, [11:50] :/ 4, [51:100] :/ 3, [101:500] :/ 2,
// [501:1000] :/ 1 }.
uint32_t draw_key_delay_max(Random& random) {
    const uint32_t pick = random.randint<uint32_t>(0, 10);
    if (pick < 1) return random.randint<uint32_t>(1, 10);
    if (pick < 5) return random.randint<uint32_t>(11, 50);
    if (pick < 8) return random.randint<uint32_t>(51, 100);
    if (pick < 10) return random.randint<uint32_t>(101, 500);
    return random.randint<uint32_t>(501, 1000);
}

// ibex_icache_ram_if's draw_sparse_mask_tag and draw_sparse_mask_data, as the
// indices of the bits the mask sets: one or two, uniform over every such mask,
// which is what `$countones(mask) inside {[1:2]}` asks for.
std::vector<std::size_t> draw_sparse_mask_bits(Random& random,
                                               std::size_t width) {
    std::vector<std::size_t> bits;
    const uint64_t singles = width;
    const uint64_t pairs = static_cast<uint64_t>(width) * (width - 1) / 2;
    bits.push_back(random.randint<std::size_t>(0, width - 1));
    if (random.randint<uint64_t>(0, singles + pairs - 1) >= singles) {
        std::size_t second = bits.front();
        while (second == bits.front()) {
            second = random.randint<std::size_t>(0, width - 1);
        }
        bits.push_back(second);
    }
    return bits;
}

// ibex_icache_ram_if's tag_sel_line and data_sel_line:
// dist { 0 :/ dis_err_pct, [1:$] :/ 100 - dis_err_pct }.
uint32_t draw_sel_line(Random& random, uint32_t dis_err_pct) {
    if (random.randint<uint32_t>(0, 99) < dis_err_pct) return 0;
    return random.randint<uint32_t>(1, (1u << kNumWays) - 1);
}

// prim_secded_inv_39_32_enc's seven parity equations. Only bits 31 down to 0
// appear in any of them, so the order the encoder fills the parity bits in does
// not matter and this is the whole of the code.
constexpr uint64_t kSecded39Masks[7] = {0x00'2606'BD25ull, 0x00'DEBA'8050ull,
                                        0x00'413D'89AAull, 0x00'3123'4ED1ull,
                                        0x00'C2C1'323Bull, 0x00'2DCC'624Cull,
                                        0x00'9850'5586ull};

// The difference between two codewords of a linear code is itself a codeword,
// and the constant inversion both of them carry cancels. So exclusive-oring a
// stored word with the codeword of a nonzero payload leaves the syndrome at
// zero and changes the data: an error the cache's SECDED cannot see. This is
// what ICACHE_ECC_ALIAS injects, and is the ECC counterpart of
// ICACHE_CORRUPT_GRANT. See README.md.
uint64_t secded_39_32_codeword(uint32_t payload) {
    uint64_t word = payload;
    for (std::size_t index = 0; index < 7; ++index) {
        const int ones = std::popcount(payload & kSecded39Masks[index]);
        word |= static_cast<uint64_t>(ones & 1) << (32 + index);
    }
    return word;
}

// ---------------------------------------------------------------------------
// The core-side driver
// ---------------------------------------------------------------------------

// A plain wait of count cycles, entered and left at a drive point. This is
// upstream's `wait_clks(n, stop_on_reset = 0)`, and is what the memory grant
// driver and the key device use.
Task<void> wait_clks(Dut dut, uint32_t count) {
    if (count == 0) co_return;
    co_await clock_cycles(dut.clk_i, count);
    co_await FallingEdge{dut.clk_i};
}

// ibex_icache_core_if::wait_clks with stop_on_reset set, which is its default.
// Upstream forks `@(negedge rst_n)` beside the count and takes whichever comes
// first; here the reset is driven by this same scheduler, so the flag is read
// once a cycle instead and the wait ends at the following drive point.
Task<void> wait_clks_core(Dut dut, Env& env, uint32_t count) {
    if (env.core_stopped()) co_return;
    for (uint32_t index = 0; index < count; ++index) {
        co_await RisingEdge{dut.clk_i};
        co_await FallingEdge{dut.clk_i};
        if (env.core_stopped()) co_return;
    }
}

// ibex_icache_core_if::wait_valid. Upstream writes `wait (driver_cb.valid)`,
// which reads the value sampled at the most recent clocking event and so takes
// no time at all if valid was already high there. env.sampled_valid is that
// value, and this only ever reads it from a drive point, by which time the
// monitor has already run for the preceding edge.
Task<void> wait_valid(Dut dut, Env& env) {
    while (!env.sampled_valid) {
        if (env.core_stopped()) co_return;
        co_await RisingEdge{dut.clk_i};
        co_await FallingEdge{dut.clk_i};
    }
}

// ibex_icache_core_driver::read_insn. Returns the error flag sampled on the
// edge where the fetch was taken, which is what read_insns stops on.
Task<bool> read_insn(Dut dut, TestContext& test, Env& env) {
    auto& random = test.random();

    // One time in ten, wait for valid before even considering ready.
    if (random.randint<uint32_t>(0, 9) == 0) {
        co_await wait_valid(dut, env);
        if (env.core_stopped()) co_return false;
    }
    co_await wait_clks_core(dut, env, random.randint<uint32_t>(0, 3));
    if (env.core_stopped()) co_return false;

    dut.ready_i.set(1);
    bool err = false;
    while (true) {
        co_await RisingEdge{dut.clk_i};
        if (dut.valid_o.get() != 0 || env.core_stopped()) {
            err = dut.valid_o.get() != 0 && dut.err_o.get() != 0;
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
        if (env.core_stopped()) break;
    }
}

// ibex_icache_core_driver::invalidate, through invalidate_pulse
Task<void> maybe_invalidate(Dut dut, TestContext& test, Env& env, bool wanted) {
    if (!wanted) co_return;
    auto& random = test.random();
    // dist { 1 :/ 10, [2:20] :/ 1 }
    const uint32_t cycles = random.randint<uint32_t>(0, 10) < 10
                                ? 1u
                                : random.randint<uint32_t>(2, 20);
    dut.invalidate_i.set(1);
    co_await wait_clks_core(dut, env, cycles);
    dut.invalidate_i.set(0);
}

// ibex_icache_core_driver::lower_req. Upstream leaves req low if it returned
// early on a reset, which is what reset_ifs would have done anyway.
Task<void> lower_req(Dut dut, Env& env, uint32_t cycles) {
    if (cycles == 0) co_return;
    dut.req_i.set(0);
    co_await wait_clks_core(dut, env, cycles);
    if (!env.core_stopped()) dut.req_i.set(1);
}

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
                  maybe_invalidate(dut, test, env, item.invalidate)};
}

// ibex_icache_core_driver::drive_req_trans
Task<void> drive_req(Dut dut, TestContext& test, Env& env, const Item item,
                     bool& saw_error) {
    dut.enable_i.set(item.enable ? 1 : 0);
    if (item.new_seed != 0) env.push_seed(item.new_seed);

    const uint32_t low_cycles =
        draw_req_low_cycles(test.random(), item.num_insns > 0);
    co_await Join{lower_req(dut, env, low_cycles),
                  maybe_invalidate(dut, test, env, item.invalidate)};
    if (!env.core_stopped()) {
        co_await read_insns(dut, test, env, item.num_insns, saw_error);
    }
}

// ---------------------------------------------------------------------------
// The core-side stimulus, ibex_icache_core_base_seq
// ---------------------------------------------------------------------------

struct SeqConfig {
    // ibex_icache_core_base_seq's knobs
    bool constrain_branches = false;
    bool initial_enable = false;
    bool const_enable = false;
    bool avoid_invalidation = false;
    bool must_invalidate = false;
    uint32_t gap_between_invalidations = 49;
    uint32_t gap_between_toggle_enable = 49;
    // ibex_icache_core_back_line_seq in place of ibex_icache_core_base_seq,
    // which ibex_icache_back_line_vseq arranges with a factory override.
    bool back_line = false;
    // ibex_icache_base_vseq::mem_err_shift
    uint32_t mem_err_shift = 3;
    // ibex_icache_ecc_vseq
    bool ecc_errors = false;
    bool disable_caching_ratio_test = false;
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
    bool must_invalidate = cfg.must_invalidate;
    uint32_t insns_since_branch = 0;
    // ibex_icache_core_back_line_seq's req_phase and last_branch.
    bool back_phase = false;
    uint32_t last_branch = 0;

    co_await FallingEdge{dut.clk_i};

    // ICACHE_ITEMS: the item stream came out of a recording of
    // ports/ibex_icache_uvm, so none of the fields below are drawn. Everything
    // downstream of the sequence -- the driver's delays, the grant and
    // response timing, the key device and the ECC masks -- still draws its
    // own, which is what makes this an experiment about the item
    // distribution alone. See README.md.
    if (env.replay_items != nullptr) {
        for (const Item& item : *env.replay_items) {
            if (env.core_stopped()) break;
            ++env.scoreboard.counters.items;
            if (item.branch) ++env.scoreboard.counters.branch_items;
            env.scoreboard.counters.insns_requested += item.num_insns;

            bool saw_error = false;
            if (item.branch) {
                co_await drive_branch(dut, test, env, item, saw_error);
            } else {
                co_await drive_req(dut, test, env, item, saw_error);
            }
        }
        co_return;
    }

    // run_reqs runs num_trans - 1 items.
    for (uint32_t index = 0; index + 1 < num_trans; ++index) {
        if (env.core_stopped()) break;

        Item item;
        if (cfg.back_line) {
            // ibex_icache_core_back_line_seq::run_req, which replaces the base
            // sequence's entirely: every item is a branch, the first phase
            // lands near the base address and the second goes back up to 16
            // bytes from where the first one went.
            item.branch = true;
            const uint32_t min_addr =
                back_phase ? (last_branch < 16 ? 0u : last_branch - 16u)
                           : base_addr;
            const uint32_t max_addr =
                back_phase ? last_branch : top_restricted_addr;
            const uint32_t steps = (max_addr - min_addr) / 2;
            item.branch_addr =
                min_addr + 2 * random.randint<uint32_t>(0, steps);
            item.enable = true;
            item.invalidate = must_invalidate;

            // `num_insns <= 5` against the item's own
            // dist { 0 :/ 5, [1:20] :/ 20, [21:100] :/ 1 }, which leaves
            // weight 5 on zero and weight 1 on each of 1 to 5.
            const uint32_t pick = random.randint<uint32_t>(0, 9);
            item.num_insns = pick < 5 ? 0u : pick - 4u;

            // new_seed's weight is zero with the cache enabled and no
            // invalidation, so a back_line item only ever brings a new seed
            // with an invalidation, which only must_invalidate can ask for.
            const uint32_t seed_weight = item.invalidate ? 1000u : 0u;
            item.new_seed = (seed_weight == 0 ||
                             random.randint<uint32_t>(0, seed_weight) == 0)
                                ? 0u
                                : random.randint<uint32_t>(1, 0xffff'ffffu);

            last_branch = item.branch_addr;
            back_phase = !back_phase;
        } else {
            if (cfg.constrain_branches && insns_since_branch >= 100) {
                force_branch = true;
            }

            // trans_type carries no dist and no constraint but force_branch, so
            // an unconstrained solve is a uniform pick over the two enum
            // values.
            item.branch = force_branch || random.randint<uint32_t>(0, 1) == 0;

            if (item.branch) {
                if (cfg.constrain_branches) {
                    // `branch_addr inside {[base_addr:top_restricted_addr]}`
                    // with the item's own `!branch_addr[0]`, so a uniform pick
                    // over the even addresses in the range.
                    const uint32_t steps =
                        (top_restricted_addr - base_addr) / 2;
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
                    random.randint<uint32_t>(
                        0, cfg.gap_between_invalidations) == 0;
            }

            // new_seed dist { 0 :/ 1, nonzero :/ W }. The zero weight when the
            // cache is enabled is what keeps a new seed away from an enabled
            // cache, which the scoreboard would see as a multi-way hit.
            const uint32_t seed_weight =
                item.invalidate ? 1000u : (item.enable ? 0u : 1u);
            if (seed_weight == 0 ||
                random.randint<uint32_t>(0, seed_weight) == 0) {
                item.new_seed = 0;
            } else {
                item.new_seed = random.randint<uint32_t>(1, 0xffff'ffffu);
            }

            // num_insns: the dist's bucket applied as a preference, the support
            // it implied as a hard bound, and constrain_branches' cap on top. A
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
            item.num_insns =
                bucket_lo <= narrowed_hi
                    ? random.randint<uint32_t>(bucket_lo, narrowed_hi)
                    : random.randint<uint32_t>(0, cap);
        }

        ++env.scoreboard.counters.items;
        if (item.branch) ++env.scoreboard.counters.branch_items;
        env.scoreboard.counters.insns_requested += item.num_insns;

        bool saw_error = false;
        if (item.branch) {
            co_await drive_branch(dut, test, env, item, saw_error);
        } else {
            co_await drive_req(dut, test, env, item, saw_error);
        }

        if (!cfg.back_line) {
            // back_line's run_req does none of this bookkeeping: it overrides
            // run_req rather than extending it, so cache_enabled, stale_seed,
            // force_branch and insns_since_branch keep their initial values.
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
        // run_reqs clears must_invalidate after every item: it is a one-shot.
        must_invalidate = false;
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
    bool last_ecc_error = false;

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

        // ecc_error_o goes to ibex_icache_ram_if::ecc_err upstream, where the
        // only thing that reads it is SVA that Verilator compiles away. It is
        // counted here so that ibex_icache_ecc can say whether the corruption
        // it injected was noticed at all.
        const bool ecc_error = dut.ecc_error_o.get() != 0;
        if (ecc_error && !last_ecc_error) ++env.scoreboard.counters.ecc_errors;
        last_ecc_error = ecc_error;
    }
}

// ibex_icache_mem_monitor, with ibex_icache_mem_resp_seq::take_gnt folded in:
// the response for a grant is decided at the grant, from the seed in force at
// that moment, which is what keeps one fetch tied to exactly one seed.
Task<void> mem_monitor(Dut dut, TestContext& test, Env& env) {
    while (true) {
        co_await RisingEdge{dut.clk_i};

        // ibex_icache_scoreboard::process_mem_fifo discards bus items that
        // arrive while rst_n is low.
        if (env.in_reset) continue;

        if (dut.instr_req_o.get() != 0 && dut.instr_gnt_i.get() != 0) {
            for (const uint32_t seed : env.pending_seeds) env.cur_seed = seed;
            env.pending_seeds.clear();

            // Under ICACHE_REPLAY the response was decided by the recorded
            // environment and is already on the wire, so what is left here is
            // the monitor alone.
            if (!env.replay_pins) {
                const uint32_t address = dut.instr_addr_o.get();
                Response response;
                response.err = is_mem_error(env.disable_mem_errs, env.cur_seed,
                                            address, env.mem_err_shift);
                // Upstream drives 'X with the error. Verilator has no X, so
                // the baseline drives zero here too; a wrong fetch that
                // returned this without the error flag would still fail the
                // scoreboard, because the seed hash is what the data is
                // checked against.
                response.rdata =
                    response.err ? 0u : read_data(env.cur_seed, address);
                if (env.corrupt_grant != 0 &&
                    env.scoreboard.counters.mem_grants + 1 ==
                        env.corrupt_grant) {
                    response.rdata ^= 1u << 17;
                }
                response.delay = draw_response_delay(test.random());
                env.responses.push_back(response);
            }
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
// when the drawn delay is zero, exactly as it does upstream. It carries no
// stop-on-reset: upstream's drive_grant keeps toggling through a reset, and
// req is low there so nothing is granted.
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
// and responses are serialised in the order they were granted. Upstream checks
// rst_n both before waiting out the delay and again before sending, and
// reset_signals flushes rdata_queue; the reset branch below is all three.
Task<void> mem_responder(Dut dut, Env& env) {
    bool driving = false;
    bool busy = false;
    Response current;
    uint32_t countdown = 0;

    while (true) {
        co_await FallingEdge{dut.clk_i};

        if (env.in_reset) {
            if (driving) {
                dut.instr_rvalid_i.set(0);
                dut.instr_err_i.set(0);
                dut.instr_rdata_i.set(0);
                driving = false;
            }
            busy = false;
            env.responses.clear();
            continue;
        }

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

// The scrambling key provider: upstream's push_pull_agent in Pull/Device mode,
// whose 194-bit d_data is randomised per transaction. The bottom bit of that
// value is the valid bit, so about half of every answer refuses the request and
// the cache asks again; the key and the nonce are the rest of it. The delay is
// `device_delay inside {[0:device_delay_max]}` with device_delay_max drawn once
// for the run, and zero_delays takes it out entirely three times in ten.
Task<void> key_device(Dut dut, TestContext& test, Env& env) {
    auto& random = test.random();
    while (true) {
        co_await FallingEdge{dut.clk_i};
        if (env.in_reset) {
            dut.scr_key_valid_i.set(0);
            continue;
        }
        if (dut.scr_key_req_o.get() == 0) continue;

        const uint32_t delay =
            env.key_zero_delays ? 0u
                                : random.randint<uint32_t>(0, env.key_delay_max);
        co_await wait_clks(dut, delay);
        if (env.in_reset) continue;

        const bool valid = random.randint<uint32_t>(0, 1) == 1;
        dut.scr_key_i.set(random.randbits<128>());
        dut.scr_nonce_i.set(random.next_u64());
        dut.scr_key_valid_i.set(valid ? 1 : 0);
        if (valid) {
            ++env.scoreboard.counters.key_answers;
        } else {
            ++env.scoreboard.counters.key_refusals;
        }
        // TwoPhase, which is the default handshake: the answer is driven for a
        // single cycle.
        co_await RisingEdge{dut.clk_i};
        co_await FallingEdge{dut.clk_i};
        dut.scr_key_valid_i.set(0);
    }
}

// ibex_icache_ram_if's two always blocks, which redraw the corruption masks on
// every falling edge where every way is returning read data. The masks reach
// the cache through ibex_icache_tb_top's exclusive-or, which sits where the
// interface's ic_*_rdata_err mux sits, so a mask written at a drive point is
// what the cache samples at the end of that cycle.
Task<void> ecc_corrupter(Dut dut, TestContext& test, Env& env) {
    constexpr uint32_t kAllWays = (1u << kNumWays) - 1u;
    auto& random = test.random();

    while (true) {
        co_await FallingEdge{dut.clk_i};
        if (!env.ecc_enabled || env.in_reset) continue;

        if (dut.ic_tag_rvalid_o.get() == kAllWays) {
            // kNumWays * kTagSizeEcc is 56 bits, which the transport carries
            // as a uint64_t rather than as a Bits<>. Tags are left alone in
            // alias mode: a corrupted tag makes the way miss, and the point
            // there is to have the cache hit and hand back the wrong data.
            uint64_t masks = 0;
            const uint32_t sel =
                env.ecc_alias ? 0u : draw_sel_line(random, env.ecc_dis_err_pct);
            for (std::size_t way = 0; way < kNumWays; ++way) {
                const auto bits = draw_sparse_mask_bits(random, kTagSizeEcc);
                if (((sel >> way) & 1u) == 0) continue;
                for (const std::size_t bit : bits) {
                    masks |= uint64_t{1} << (way * kTagSizeEcc + bit);
                }
            }
            if (sel != 0) ++env.scoreboard.counters.ecc_injections;
            dut.ic_tag_rdata_mask_i.set(masks);
        }

        if (dut.ic_data_rvalid_o.get() == kAllWays) {
            Bits<kNumWays * kLineSizeEcc> masks{};
            const uint32_t sel = draw_sel_line(random, env.ecc_dis_err_pct);
            for (std::size_t way = 0; way < kNumWays; ++way) {
                if (env.ecc_alias) {
                    // One beat of the line, moved to a different but equally
                    // well-formed codeword.
                    const auto beat =
                        random.randint<std::size_t>(0, kLineBeats - 1);
                    const uint32_t delta =
                        random.randint<uint32_t>(1, 0xffff'ffffu);
                    const uint64_t word = secded_39_32_codeword(delta);
                    if (((sel >> way) & 1u) == 0) continue;
                    for (std::size_t bit = 0; bit < kBusSizeEcc; ++bit) {
                        if (((word >> bit) & 1u) == 0) continue;
                        masks.set_bit(
                            way * kLineSizeEcc + beat * kBusSizeEcc + bit,
                            true);
                    }
                    continue;
                }
                const auto bits = draw_sparse_mask_bits(random, kLineSizeEcc);
                if (((sel >> way) & 1u) == 0) continue;
                for (const std::size_t bit : bits) {
                    masks.set_bit(way * kLineSizeEcc + bit, true);
                }
            }
            if (sel != 0) ++env.scoreboard.counters.ecc_injections;
            dut.ic_data_rdata_mask_i.set(masks);
        }
    }
}

// ---------------------------------------------------------------------------
// Replay
//
// ports/ibex_icache_uvm records what its environment did; this drives the same
// thing at the same DUT pins and checks that the DUT answers with the same
// outputs on the same cycles. Two harnesses running independent random streams
// can only be compared on the agreement of their rates, which is a statement
// about two distributions; this is a statement about one run.
//
// The recording is written by the tb.sv overlay in ports/ibex_icache_uvm's
// build_tb.py. Every value in it is read in the Active region of a posedge,
// which is what the design samples at that edge, so the inputs are driven here
// at the drive point before that edge and the outputs are compared at the edge
// itself. See README.md.
// ---------------------------------------------------------------------------

// Bit positions in the two packed fields of a `C` line, as its header names
// them.
enum : uint32_t {
    kInRstN = 0,
    kInReq = 1,
    kInBranch = 2,
    kInReady = 3,
    kInEnable = 4,
    kInInvalidate = 5,
    kInGnt = 6,
    kInRvalid = 7,
    kInErr = 8,
    kInKeyValid = 9,
};
enum : uint32_t {
    kOutValid = 0,
    kOutErr = 1,
    kOutErrPlus2 = 2,
    kOutBusy = 3,
    kOutInstrReq = 4,
    kOutKeyReq = 5,
    kOutEccError = 6,
    kOutTagRvalid = 7,
    kOutDataRvalid = 7 + kNumWays,
};

struct PinCycle {
    uint32_t in = 0;
    uint32_t out = 0;
    uint32_t branch_addr = 0;
    uint32_t instr_rdata = 0;
    uint32_t core_rdata = 0;
    uint32_t core_addr = 0;
    uint32_t instr_addr = 0;
};

struct KeyEvent {
    uint64_t cycle = 0;
    Bits<128> key{};
    uint64_t nonce = 0;
};

struct MaskEvent {
    uint64_t cycle = 0;
    uint64_t tag = 0;
    Bits<kNumWays * kLineSizeEcc> data{};
};

struct SeedEvent {
    // The edge before which the seed reaches the scoreboard.
    uint64_t cycle = 0;
    uint32_t seed = 0;
};

struct Trace {
    std::vector<PinCycle> pins;
    // The wide inputs are written only when they change, so these are sparse
    // and a replay holds the last value.
    std::vector<KeyEvent> keys;
    std::vector<MaskEvent> masks;
    std::vector<SeedEvent> seeds;
    std::vector<Item> items;
};

std::vector<std::string> split_words(const std::string& line) {
    std::vector<std::string> out;
    std::size_t index = 0;
    while (index < line.size()) {
        while (index < line.size() && line[index] == ' ') ++index;
        const std::size_t start = index;
        while (index < line.size() && line[index] != ' ') ++index;
        if (index > start) out.emplace_back(line, start, index - start);
    }
    return out;
}

uint64_t parse_hex(const std::string& text) {
    return std::strtoull(text.c_str(), nullptr, 16);
}

// <prefix>.pins
bool read_pin_trace(const std::string& path, Trace& trace, uint64_t& cycle0,
                    uint64_t& period, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "cannot read " + path;
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        if (line[0] == '#') {
            unsigned long long value = 0;
            if (std::sscanf(line.c_str(), "# cycle0_time %llu", &value) == 1) {
                cycle0 = value;
            } else if (std::sscanf(line.c_str(), "# period_time %llu",
                                   &value) == 1) {
                period = value;
            }
            continue;
        }
        if (line[0] == 'C') {
            unsigned long long cycle = 0;
            PinCycle pin;
            if (std::sscanf(line.c_str(), "C %llu %x %x %x %x %x %x %x", &cycle,
                            &pin.in, &pin.branch_addr, &pin.instr_rdata,
                            &pin.out, &pin.core_rdata, &pin.core_addr,
                            &pin.instr_addr) != 8) {
                error = "malformed C line in " + path + ": " + line;
                return false;
            }
            if (cycle != trace.pins.size()) {
                error = "the pin trace skips a cycle at " + std::to_string(cycle);
                return false;
            }
            trace.pins.push_back(pin);
            continue;
        }
        const auto words = split_words(line);
        if (line[0] == 'K' && words.size() == 4) {
            KeyEvent event;
            event.cycle = std::strtoull(words[1].c_str(), nullptr, 10);
            event.key = Bits<128>::from_hex(words[2]);
            event.nonce = parse_hex(words[3]);
            trace.keys.push_back(event);
            continue;
        }
        if (line[0] == 'M' && words.size() == 4) {
            MaskEvent event;
            event.cycle = std::strtoull(words[1].c_str(), nullptr, 10);
            event.tag = parse_hex(words[2]);
            event.data =
                Bits<kNumWays * kLineSizeEcc>::from_hex(words[3]);
            trace.masks.push_back(event);
            continue;
        }
        error = "unrecognised line in " + path + ": " + line;
        return false;
    }
    if (trace.pins.empty() || period == 0) {
        error = path + " carries no cycles; the recording run did not get far";
        return false;
    }
    return true;
}

// <prefix>.items. The times in it are the simulation times at which the core
// driver took the item and at which it announced a new memory seed; the header
// of the pin trace says which cycle time zero was and how long a cycle is.
bool read_item_trace(const std::string& path, Trace& trace, uint64_t cycle0,
                     uint64_t period, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "cannot read " + path;
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto words = split_words(line);
        if (line[0] == 'I' && words.size() == 8) {
            Item item;
            item.branch = words[2] != "0";
            item.branch_addr = static_cast<uint32_t>(parse_hex(words[3]));
            item.enable = words[4] != "0";
            item.invalidate = words[5] != "0";
            item.new_seed = static_cast<uint32_t>(parse_hex(words[6]));
            item.num_insns =
                static_cast<uint32_t>(std::strtoul(words[7].c_str(), nullptr,
                                                   10));
            trace.items.push_back(item);
            continue;
        }
        if (line[0] == 'S' && words.size() == 3) {
            // Only a pin replay has a use for these, and only a pin replay
            // read the trace that says what a cycle is.
            if (period == 0) continue;
            const uint64_t stamp = std::strtoull(words[1].c_str(), nullptr, 10);
            SeedEvent event;
            // The driver announces the seed just after the edge whose time
            // this is, so the scoreboard has it before the next one.
            event.cycle = stamp < cycle0 ? 0 : (stamp - cycle0) / period + 1;
            event.seed = static_cast<uint32_t>(parse_hex(words[2]));
            trace.seeds.push_back(event);
            continue;
        }
        error = "unrecognised line in " + path + ": " + line;
        return false;
    }
    return true;
}

void apply_pins(Dut dut, const PinCycle& pin) {
    dut.rst_ni.set((pin.in >> kInRstN) & 1u);
    dut.req_i.set((pin.in >> kInReq) & 1u);
    dut.branch_i.set((pin.in >> kInBranch) & 1u);
    dut.branch_addr_i.set(pin.branch_addr);
    dut.ready_i.set((pin.in >> kInReady) & 1u);
    dut.enable_i.set((pin.in >> kInEnable) & 1u);
    dut.invalidate_i.set((pin.in >> kInInvalidate) & 1u);
    dut.instr_gnt_i.set((pin.in >> kInGnt) & 1u);
    dut.instr_rvalid_i.set((pin.in >> kInRvalid) & 1u);
    dut.instr_rdata_i.set(pin.instr_rdata);
    dut.instr_err_i.set((pin.in >> kInErr) & 1u);
    dut.scr_key_valid_i.set((pin.in >> kInKeyValid) & 1u);
}

// Apply everything the recording says about the drive point before the edge
// for `cycle`: the wide inputs, the seeds the core driver announced, and the
// scoreboard's reset hooks on the edges of the recorded rst_n.
struct ReplayCursor {
    std::size_t key = 0;
    std::size_t mask = 0;
    std::size_t seed = 0;
};

void apply_cycle(Dut dut, Env& env, const Trace& trace, uint64_t cycle,
                 bool run_scoreboard, ReplayCursor& cursor) {
    if (cycle > 0) {
        const bool was_reset =
            ((trace.pins[cycle - 1].in >> kInRstN) & 1u) == 0;
        const bool now_reset = ((trace.pins[cycle].in >> kInRstN) & 1u) == 0;
        if (now_reset != was_reset) {
            env.in_reset = now_reset;
            if (run_scoreboard) {
                if (now_reset) {
                    env.scoreboard.start_reset();
                } else {
                    env.scoreboard.end_reset();
                }
            }
        }
    }

    while (cursor.seed < trace.seeds.size() &&
           trace.seeds[cursor.seed].cycle <= cycle) {
        if (run_scoreboard) {
            env.scoreboard.on_new_seed(trace.seeds[cursor.seed].seed);
        }
        ++cursor.seed;
    }

    apply_pins(dut, trace.pins[cycle]);
    while (cursor.key < trace.keys.size() &&
           trace.keys[cursor.key].cycle == cycle) {
        dut.scr_key_i.set(trace.keys[cursor.key].key);
        dut.scr_nonce_i.set(trace.keys[cursor.key].nonce);
        ++cursor.key;
    }
    while (cursor.mask < trace.masks.size() &&
           trace.masks[cursor.mask].cycle == cycle) {
        dut.ic_tag_rdata_mask_i.set(trace.masks[cursor.mask].tag);
        dut.ic_data_rdata_mask_i.set(trace.masks[cursor.mask].data);
        // ibex_icache_ram_if redraws the mask on every falling edge where
        // every way is returning data, and the recording writes it only when
        // it changes, so this counts corruptions rather than the negedges that
        // drew one.
        if (trace.masks[cursor.mask].tag != 0 ||
            trace.masks[cursor.mask].data !=
                Bits<kNumWays * kLineSizeEcc>{}) {
            ++env.scoreboard.counters.ecc_injections;
        }
        ++cursor.mask;
    }
}

// Drive every DUT input from the recording and compare every DUT output
// against it. Driving happens at the drive point before an edge and comparing
// at the edge itself, which are the two points the recording is written from.
//
// Entered at a drive point with the recording's cycle 0 already applied.
Task<void> replay_run(Dut dut, TestContext& test, Env& env, const Trace& trace,
                      bool run_scoreboard, ReplayCursor cursor) {
    for (uint64_t cycle = 0; cycle < trace.pins.size(); ++cycle) {
        co_await RisingEdge{dut.clk_i};
        const PinCycle& pin = trace.pins[cycle];

        uint32_t out = 0;
        out |= (dut.valid_o.get() != 0 ? 1u : 0u) << kOutValid;
        out |= (dut.err_o.get() != 0 ? 1u : 0u) << kOutErr;
        out |= (dut.err_plus2_o.get() != 0 ? 1u : 0u) << kOutErrPlus2;
        out |= (dut.busy_o.get() != 0 ? 1u : 0u) << kOutBusy;
        out |= (dut.instr_req_o.get() != 0 ? 1u : 0u) << kOutInstrReq;
        out |= (dut.scr_key_req_o.get() != 0 ? 1u : 0u) << kOutKeyReq;
        out |= (dut.ecc_error_o.get() != 0 ? 1u : 0u) << kOutEccError;
        out |= static_cast<uint32_t>(dut.ic_tag_rvalid_o.get()) << kOutTagRvalid;
        out |= static_cast<uint32_t>(dut.ic_data_rvalid_o.get())
               << kOutDataRvalid;

        const uint32_t core_rdata = dut.rdata_o.get();
        const uint32_t core_addr = dut.addr_o.get();
        const uint32_t instr_addr = dut.instr_addr_o.get();

        if (out != pin.out || core_rdata != pin.core_rdata ||
            core_addr != pin.core_addr || instr_addr != pin.instr_addr) {
            // Everything after the first divergence is a consequence of it, so
            // this stops rather than printing a run's worth of noise.
            std::printf(
                "cpptb-icache replay divergence at cycle %llu:\n"
                "  recorded out=%04x rdata=%08x addr=%08x instr_addr=%08x\n"
                "  replayed out=%04x rdata=%08x addr=%08x instr_addr=%08x\n",
                static_cast<unsigned long long>(cycle), pin.out, pin.core_rdata,
                pin.core_addr, pin.instr_addr, out, core_rdata, core_addr,
                instr_addr);
            env.replay_diverged = true;
            test.expect(
                "the DUT's outputs match the recording on every cycle, and "
                "they first differ at cycle " +
                    std::to_string(cycle),
                false);
            co_return;
        }

        if (cycle + 1 == trace.pins.size()) break;
        co_await FallingEdge{dut.clk_i};
        apply_cycle(dut, env, trace, cycle + 1, run_scoreboard, cursor);
    }
    test.expect("the DUT's outputs match the recording on every cycle", true);
}

// ---------------------------------------------------------------------------
// Reset, and the virtual sequences that use it
// ---------------------------------------------------------------------------

// dv_base_vseq::dut_init, through clk_rst_if::apply_reset: assert rst_n, hold
// it for 50 to 100 cycles and release it. Entered and left at a drive point,
// so the release is captured by the next rising edge and by no earlier one.
//
// Everything the environment does around a reset is here rather than spread
// through the drivers, because the drivers are coroutines this scheduler owns:
// the memory driver's reset_signals, the core sequence's reset_ifs, and the
// scoreboard's start_reset and reset hooks.
Task<void> apply_reset(Dut dut, TestContext& test, Env& env) {
    const uint32_t width = test.random().randint<uint32_t>(50, 100);

    // ibex_icache_base_vseq::reset_ifs, which the combo sequence calls on the
    // sequence it is about to drop so that no request escapes the DUT while it
    // is held in reset.
    dut.req_i.set(0);
    dut.branch_i.set(0);
    dut.ready_i.set(0);
    dut.invalidate_i.set(0);

    env.in_reset = true;
    dut.rst_ni.set(0);
    env.scoreboard.start_reset();

    // ibex_icache_mem_driver::reset_signals, which drive_resets calls on the
    // falling edge of rst_n: clear the bus and flush the pending responses.
    dut.instr_gnt_i.set(0);
    dut.instr_rvalid_i.set(0);
    dut.instr_err_i.set(0);
    dut.scr_key_valid_i.set(0);
    env.responses.clear();

    co_await wait_clks(dut, width);
    dut.rst_ni.set(1);
    env.in_reset = false;
    env.scoreboard.end_reset();
}

// ibex_icache_base_vseq: pre_start, which writes the sequence's knobs into the
// config object and then resets the DUT if it was asked to, and body, which
// runs the core sequence until it has run all its items.
//
// cycles_till_reset is ibex_icache_combo_vseq::run_sequence's timer. Nonzero
// means this child may be stopped part way through, which is the one path that
// needs the core sequence to be a process of its own; on every other path it
// runs inline, where it has always run.
Task<void> run_child(Dut dut, TestContext& test, Env& env, const SeqConfig cfg,
                     uint32_t num_trans, bool do_dut_init,
                     uint32_t cycles_till_reset) {
    // Written before the reset, as pre_start writes them: the scoreboard and
    // the memory sequence both read mem_err_shift out of the config object.
    env.mem_err_shift = cfg.mem_err_shift;
    env.scoreboard.set_mem_err_shift(cfg.mem_err_shift);
    // ibex_icache_ecc_vseq::pre_start sets enable_ecc_errors on the interface
    // and nothing ever clears it, so once a combo run has taken an ECC child
    // the corruption stays on for the rest of the run.
    if (cfg.ecc_errors) env.ecc_enabled = true;
    env.scoreboard.set_disable_caching_ratio_test(
        cfg.disable_caching_ratio_test);

    if (do_dut_init) co_await apply_reset(dut, test, env);

    ++env.scoreboard.counters.child_sequences;
    env.stimulus_killed = false;

    if (cycles_till_reset == 0) {
        co_await core_stimulus(dut, test, env, cfg, num_trans);
    } else {
        auto stimulus = test.spawn(core_stimulus(dut, test, env, cfg,
                                                 num_trans));
        for (uint32_t index = 0;
             index < cycles_till_reset && !stimulus.done(); ++index) {
            co_await RisingEdge{dut.clk_i};
        }
        if (!stimulus.done()) {
            env.stimulus_killed = true;
            ++env.scoreboard.counters.killed_sequences;
        }
        co_await stimulus;
    }

    env.stimulus_killed = false;
    // ibex_icache_ecc_vseq::body restores the flag once its child is done.
    env.scoreboard.set_disable_caching_ratio_test(false);
}

// ibex_icache_combo_vseq::seq_names, in the order upstream lists them, with
// each entry the knobs that vseq's pre_start sets. ibex_icache_oldval_vseq is
// deliberately not among them: its test has a different checker.
constexpr std::size_t kComboSeqCount = 7;

SeqConfig combo_sequence(std::size_t index) {
    SeqConfig cfg;
    switch (index) {
        case 0:  // ibex_icache_back_line_vseq
            cfg.back_line = true;
            break;
        case 1:  // ibex_icache_base_vseq, the smoke sequence
            break;
        case 2:  // ibex_icache_caching_vseq
            cfg.constrain_branches = true;
            cfg.initial_enable = true;
            cfg.const_enable = true;
            cfg.avoid_invalidation = true;
            break;
        case 3:  // ibex_icache_ecc_vseq, which derives from the caching one
            cfg.constrain_branches = true;
            cfg.initial_enable = true;
            cfg.const_enable = true;
            cfg.avoid_invalidation = true;
            cfg.ecc_errors = true;
            cfg.disable_caching_ratio_test = true;
            break;
        case 4:  // ibex_icache_invalidation_vseq
            cfg.constrain_branches = true;
            cfg.initial_enable = true;
            cfg.const_enable = true;
            break;
        case 5:  // ibex_icache_many_errors_vseq
            cfg.constrain_branches = true;
            cfg.initial_enable = true;
            cfg.mem_err_shift = 1;
            break;
        default:  // ibex_icache_passthru_vseq
            cfg.constrain_branches = true;
            cfg.initial_enable = false;
            cfg.const_enable = true;
            break;
    }
    return cfg;
}

// ibex_icache_combo_vseq::body, and ibex_icache_reset_vseq when random_reset
// is set: run child sequences back to back, resetting the DUT between them,
// each child handing the memory seed in force on to the next.
Task<void> combo_body(Dut dut, TestContext& test, Env& env, uint32_t num_trans,
                      bool random_reset) {
    auto& random = test.random();
    uint32_t trans_so_far = 0;
    uint32_t seqs_so_far = 0;
    bool have_prev = false;

    while (trans_so_far < num_trans) {
        const auto seq_idx = random.randint<std::size_t>(0, kComboSeqCount - 1);
        // Not too many, because the point is the edges between sequences.
        const uint32_t trans_now = random.randint<uint32_t>(50, 100);

        bool should_reset;
        if (trans_so_far == 0) {
            // The reset before this task started counts.
            should_reset = false;
        } else if (random_reset) {
            should_reset = true;
        } else {
            should_reset = random.randint<uint32_t>(0, 1) == 1;
        }

        SeqConfig cfg = combo_sequence(seq_idx);
        // ibex_icache_base_vseq::pre_start: a child that changes the memory
        // error rate has to tell the core to invalidate, because the cache is
        // holding lines fetched at the old one.
        if (have_prev && env.mem_err_shift != cfg.mem_err_shift) {
            cfg.must_invalidate = true;
        }

        // The memory agent holds a granted request until it has answered it,
        // which is what lets a sequence change under the environment's feet
        // when there is no reset. An actual reset has to discard them.
        if (should_reset && seqs_so_far > 0) env.responses.clear();

        const uint32_t cycles_till_reset =
            random_reset ? draw_cycles_till_reset(random) : 0;
        co_await run_child(dut, test, env, cfg, trans_now, should_reset,
                           cycles_till_reset);

        have_prev = true;
        trans_so_far += trans_now;
        ++seqs_so_far;
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
        "actual_old=%llu child_sequences=%llu resets=%llu "
        "killed_sequences=%llu ecc_injections=%llu ecc_errors=%llu "
        "key_answers=%llu key_refusals=%llu cycles=%llu\n",
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
        static_cast<unsigned long long>(counters.child_sequences),
        static_cast<unsigned long long>(counters.resets),
        static_cast<unsigned long long>(counters.killed_sequences),
        static_cast<unsigned long long>(counters.ecc_injections),
        static_cast<unsigned long long>(counters.ecc_errors),
        static_cast<unsigned long long>(counters.key_answers),
        static_cast<unsigned long long>(counters.key_refusals),
        static_cast<unsigned long long>(counters.cycles));
    test.expect("the sequence issued at least one fetch", counters.fetches > 0);
    if (counters.ecc_injections > 0) {
        // Upstream states this as SVA in ibex_icache_ram_if, per way and per
        // cycle, and Verilator compiles it to nothing, so the baseline does not
        // check it at all. The per-cycle form is stronger than it looks: a
        // corrupted way only raises ecc_error_o if the lookup reached it, so
        // what is checked here is the aggregate. See README.md.
        test.expect(
            "the cache reported an ECC error for the RAM data that was "
            "corrupted",
            counters.ecc_errors > 0);
    }
}

// ibex_icache_oldval_test::check_phase. The counters are kept by every run;
// only this test requires anything of them, because the requirement only makes
// sense when the sequence was arranged to do lots of caching.
void check_oldval(TestContext& test, const Counters& counters) {
    test.expect(
        "an oldval run saw at least 1000 fetches where an old value was "
        "possible, and saw " +
            std::to_string(counters.possible_old),
        counters.possible_old >= 1000);
    test.expect("actual_old does not exceed possible_old",
                counters.actual_old <= counters.possible_old);
    if (counters.possible_old == 0) return;
    // A tenth of a per cent, so that a failure can say what it saw.
    const uint64_t frac4 = (1000 * counters.actual_old +
                            counters.possible_old / 2) /
                           counters.possible_old;
    test.expect(
        "at least 5% of the fetches that could have returned an old value did, "
        "and " +
            std::to_string(counters.actual_old) + " of " +
            std::to_string(counters.possible_old) + " (" +
            std::to_string(frac4 / 10) + "." + std::to_string(frac4 % 10) +
            "%) did",
        frac4 >= 50);
}

struct TestPlan {
    SeqConfig cfg;
    // ibex_icache_combo_vseq, and ibex_icache_reset_vseq on top of it.
    bool combo = false;
    bool random_reset = false;
    // ibex_icache_oldval_test rather than ibex_icache_base_test.
    bool check_old_values = false;
};

uint64_t env_number(const char* name, uint64_t fallback) {
    const char* text = std::getenv(name);
    return text != nullptr ? std::strtoull(text, nullptr, 0) : fallback;
}

std::string env_string(const char* name) {
    const char* text = std::getenv(name);
    return text != nullptr ? std::string(text) : std::string();
}

// ICACHE_REPLAY=<prefix> and ICACHE_ITEMS=<prefix>. Both name the prefix
// ports/ibex_icache_uvm was given as +icache_record; the files under it are
// <prefix>.pins and <prefix>.items.
bool load_trace(TestContext& test, const std::string& prefix, bool want_pins,
                Trace& trace) {
    uint64_t cycle0 = 0;
    uint64_t period = 0;
    std::string error;
    if (want_pins &&
        !read_pin_trace(prefix + ".pins", trace, cycle0, period, error)) {
        test.expect(error, false);
        return false;
    }
    if (!read_item_trace(prefix + ".items", trace, cycle0, period, error)) {
        test.expect(error, false);
        return false;
    }
    if (trace.items.empty()) {
        test.expect("the recording carries at least one core item", false);
        return false;
    }
    return true;
}

Task<void> run_icache_test(Dut dut, TestContext& test, const char* name,
                           TestPlan plan) {
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
    dut.ic_tag_rdata_mask_i.set(uint64_t{0});
    dut.ic_data_rdata_mask_i.set(Bits<kNumWays * kLineSizeEcc>{});
    // Every output is read here for the same reason. scr_key_req_o is read
    // only by the replay checker, which a discovery run does not reach.
    static_cast<void>(dut.valid_o.get());
    static_cast<void>(dut.rdata_o.get());
    static_cast<void>(dut.addr_o.get());
    static_cast<void>(dut.err_o.get());
    static_cast<void>(dut.err_plus2_o.get());
    static_cast<void>(dut.busy_o.get());
    static_cast<void>(dut.instr_req_o.get());
    static_cast<void>(dut.instr_addr_o.get());
    static_cast<void>(dut.scr_key_req_o.get());
    static_cast<void>(dut.ic_tag_rvalid_o.get());
    static_cast<void>(dut.ic_data_rvalid_o.get());
    static_cast<void>(dut.ecc_error_o.get());

    Scoreboard scoreboard(test, /*disable_mem_errs=*/false,
                          /*mem_err_shift=*/3);
    Env env{scoreboard};
    // Read unconditionally, so hierarchy discovery sees the same code path a
    // real run takes.
    env.corrupt_grant = env_number("ICACHE_CORRUPT_GRANT", 0);
    env.ecc_dis_err_pct =
        static_cast<uint32_t>(env_number("ICACHE_ECC_ERR_PCT", 99));
    env.ecc_alias = env_number("ICACHE_ECC_ALIAS", 0) != 0;
    scoreboard.set_keep_state_on_reset(
        env_number("ICACHE_KEEP_STATE_ON_RESET", 0) != 0);

    const std::string replay_prefix = env_string("ICACHE_REPLAY");
    const std::string items_prefix = env_string("ICACHE_ITEMS");
    Trace trace;
    if (!replay_prefix.empty() || !items_prefix.empty()) {
        const bool want_pins = !replay_prefix.empty();
        if (!load_trace(test, want_pins ? replay_prefix : items_prefix,
                        want_pins, trace)) {
            co_return;
        }
    }

    // The recorded environment changes mem_err_shift and the caching-ratio
    // flag between child sequences, and neither is visible at a DUT pin, so a
    // combo run's scoreboard cannot be driven from a recording. The pin
    // comparison is what a combo replay is for; its scoreboard is left out
    // rather than run on state it cannot know.
    const bool replay = !replay_prefix.empty();
    const bool replay_scoreboard = replay && !plan.combo;
    env.replay_pins = replay;
    if (!replay && !items_prefix.empty()) env.replay_items = &trace.items;

    test.start_clock(dut.clk_i, kClockPeriod);

    // push_pull_agent_cfg is randomised once, so its delay bounds are drawn
    // here rather than per request. zero_delays carries dist { 0 := 7, 1 := 3 }.
    env.key_zero_delays = test.random().randint<uint32_t>(0, 9) < 3;
    env.key_delay_max = draw_key_delay_max(test.random());

    if (replay) {
        // The recording's cycle 0 is the first posedge of the recorded run, by
        // which time dv_base_vseq::dut_init had already driven rst_n low. The
        // same two idle cycles with rst_ni high are run here so that applying
        // cycle 0 is a real falling edge: without one, the RAMs' four-valued
        // rvalid registers stay at their zero-initialised value, which
        // `mubi4_test_true_loose` reads as true, and the first cycle disagrees
        // for a reason that has nothing to do with the stimulus.
        co_await clock_cycles(dut.clk_i, 2);
        co_await FallingEdge{dut.clk_i};

        ReplayCursor cursor;
        if (replay_scoreboard) {
            scoreboard.set_mem_err_shift(plan.cfg.mem_err_shift);
            scoreboard.set_disable_caching_ratio_test(
                plan.cfg.disable_caching_ratio_test);
            // The stimulus counters come out of the item recording rather than
            // out of the sequence, so that the report line is comparable with
            // a generated run's. The key agent's counters have no counterpart:
            // a refusal carries the same pins as an idle cycle.
            for (const Item& item : trace.items) {
                ++scoreboard.counters.items;
                if (item.branch) ++scoreboard.counters.branch_items;
                scoreboard.counters.insns_requested += item.num_insns;
            }
        }
        apply_cycle(dut, env, trace, 0, replay_scoreboard, cursor);
        env.in_reset = ((trace.pins.front().in >> kInRstN) & 1u) == 0;
        // The falling edge of rst_n that opens the recording is the one
        // apply_reset fires start_reset on.
        if (replay_scoreboard && env.in_reset) scoreboard.start_reset();

        std::optional<Process> monitor_core;
        std::optional<Process> monitor_mem;
        if (replay_scoreboard) {
            monitor_core.emplace(test.spawn(core_monitor(dut, env)));
            monitor_mem.emplace(test.spawn(mem_monitor(dut, test, env)));
        }
        co_await replay_run(dut, test, env, trace, replay_scoreboard, cursor);
        if (monitor_mem.has_value()) {
            monitor_mem->cancel();
            co_await *monitor_mem;
        }
        if (monitor_core.has_value()) {
            monitor_core->cancel();
            co_await *monitor_core;
        }
        if (replay_scoreboard) {
            report(test, name, scoreboard.counters);
        } else {
            std::printf("cpptb-icache %s replay-pins-only cycles=%llu\n", name,
                        static_cast<unsigned long long>(trace.pins.size()));
        }
        co_return;
    }

    // The core monitor is started first so that, when a core item and a memory
    // item land on the same edge, the scoreboard sees them in the order its
    // two forked processes see them upstream.
    auto monitor_core = test.spawn(core_monitor(dut, env));
    auto monitor_mem = test.spawn(mem_monitor(dut, test, env));
    auto grants = test.spawn(mem_grant_driver(dut, test));
    auto responses = test.spawn(mem_responder(dut, env));
    auto keys = test.spawn(key_device(dut, test, env));
    auto ecc = test.spawn(ecc_corrupter(dut, test, env));

    // dv_base_vseq::dut_init before the first sequence. Reset starts released
    // so that asserting it produces the negedge the design's asynchronous
    // resets need.
    co_await clock_cycles(dut.clk_i, 2);
    co_await FallingEdge{dut.clk_i};
    co_await apply_reset(dut, test, env);

    const uint32_t num_trans =
        test.random().randint<uint32_t>(kMinTrans, kMaxTrans);
    if (plan.combo) {
        co_await combo_body(dut, test, env, num_trans, plan.random_reset);
    } else {
        co_await run_child(dut, test, env, plan.cfg, num_trans,
                           /*do_dut_init=*/false, /*cycles_till_reset=*/0);
    }

    // ibex_icache_base_vseq::body ends by killing every sequence still running
    // once the core sequence has run all its items.
    ecc.cancel();
    keys.cancel();
    responses.cancel();
    grants.cancel();
    monitor_mem.cancel();
    monitor_core.cancel();
    co_await ecc;
    co_await keys;
    co_await responses;
    co_await grants;
    co_await monitor_mem;
    co_await monitor_core;

    report(test, name, scoreboard.counters);
    // ICACHE_CHECK_OLDVAL applies ibex_icache_oldval_test's check_phase to any
    // test, which is how a run can show the check is live: a sequence that
    // invalidates or never disables the cache leaves possible_old at zero and
    // fails it. See README.md.
    if (plan.check_old_values || env_number("ICACHE_CHECK_OLDVAL", 0) != 0) {
        check_oldval(test, scoreboard.counters);
    }
}

// ibex_icache_base_vseq: everything at its default, which means the enable
// line toggles, seeds change and the cache is invalidated from time to time.
// It proves the environment runs and that every fetch returned correct data.
// It completes no caching-ratio window, because toggling the enable line
// resets the window, so it says nothing at all about caching.
Task<void> ibex_icache_smoke(Dut dut, TestContext& test) {
    co_await run_icache_test(dut, test, "ibex_icache_smoke", TestPlan{});
}

// ibex_icache_passthru_vseq: branch targets constrained to a 64-byte window
// and the cache held disabled, so every fetch goes to memory. It also sets
// gap_between_seeds, which ibex_icache_core_base_seq declares and no
// constraint reads, so it has no effect on either harness.
Task<void> ibex_icache_passthru(Dut dut, TestContext& test) {
    TestPlan plan;
    plan.cfg.constrain_branches = true;
    plan.cfg.initial_enable = false;
    plan.cfg.const_enable = true;
    co_await run_icache_test(dut, test, "ibex_icache_passthru", plan);
}

// ibex_icache_caching_vseq: the same window with the cache held enabled and
// invalidation avoided, which is what lets the caching-ratio windows complete.
Task<void> ibex_icache_caching(Dut dut, TestContext& test) {
    TestPlan plan;
    plan.cfg.constrain_branches = true;
    plan.cfg.initial_enable = true;
    plan.cfg.const_enable = true;
    plan.cfg.avoid_invalidation = true;
    co_await run_icache_test(dut, test, "ibex_icache_caching", plan);
}

// ibex_icache_invalidation_vseq: the caching sequence without
// avoid_invalidation, so the cache is invalidated about one item in fifty and
// every invalidation is given a new memory seed. Its gap_between_seeds = 19 is
// the same dead knob as passthru's.
Task<void> ibex_icache_invalidation(Dut dut, TestContext& test) {
    TestPlan plan;
    plan.cfg.constrain_branches = true;
    plan.cfg.initial_enable = true;
    plan.cfg.const_enable = true;
    co_await run_icache_test(dut, test, "ibex_icache_invalidation", plan);
}

// ibex_icache_many_errors_vseq: mem_err_shift 1 rather than 3, which makes
// about half of memory error, with the enable line free to toggle.
Task<void> ibex_icache_many_errors(Dut dut, TestContext& test) {
    TestPlan plan;
    plan.cfg.constrain_branches = true;
    plan.cfg.initial_enable = true;
    plan.cfg.mem_err_shift = 1;
    co_await run_icache_test(dut, test, "ibex_icache_many_errors", plan);
}

// ibex_icache_back_line_vseq: ibex_icache_core_back_line_seq in place of the
// base sequence, which branches on every item and alternates between a target
// near the base address and a target up to 16 bytes back from the last one, so
// that a fetch keeps arriving at the end of a cache line the cache has only
// just filled.
Task<void> ibex_icache_back_line(Dut dut, TestContext& test) {
    TestPlan plan;
    plan.cfg.back_line = true;
    co_await run_icache_test(dut, test, "ibex_icache_back_line", plan);
}

// ibex_icache_oldval_vseq with ibex_icache_oldval_test: constrained branches, a
// cache enable line that toggles one item in three, and no invalidation, so the
// cache keeps lines across a disable and returns them from a seed that is no
// longer the newest. check_oldval is that test's check_phase.
Task<void> ibex_icache_oldval(Dut dut, TestContext& test) {
    TestPlan plan;
    plan.cfg.constrain_branches = true;
    plan.cfg.gap_between_toggle_enable = 2;
    plan.cfg.avoid_invalidation = true;
    plan.check_old_values = true;
    co_await run_icache_test(dut, test, "ibex_icache_oldval", plan);
}

// ibex_icache_ecc_vseq: the caching sequence with RAM read data corrupted a way
// at a time. The cache should spot every corruption and behave as if it missed,
// so the data reaching the core is still right; the caching ratio check is
// turned off because the corruption lowers the hit rate.
Task<void> ibex_icache_ecc(Dut dut, TestContext& test) {
    TestPlan plan;
    plan.cfg.constrain_branches = true;
    plan.cfg.initial_enable = true;
    plan.cfg.const_enable = true;
    plan.cfg.avoid_invalidation = true;
    plan.cfg.ecc_errors = true;
    plan.cfg.disable_caching_ratio_test = true;
    co_await run_icache_test(dut, test, "ibex_icache_ecc", plan);
}

// ibex_icache_combo_vseq: 50 to 100 transactions of one of the seven sequences
// above, then the next, with a reset between them one time in two. What it
// tests is the edges between sequences.
Task<void> ibex_icache_stress_all(Dut dut, TestContext& test) {
    TestPlan plan;
    plan.combo = true;
    co_await run_icache_test(dut, test, "ibex_icache_stress_all", plan);
}

// ibex_icache_reset_vseq: the same, with a reset before every child and each
// child stopped after 100 to 1000 cycles rather than being allowed to finish,
// so the reset lands at a time the core sequence would not expect.
Task<void> ibex_icache_stress_all_with_reset(Dut dut, TestContext& test) {
    TestPlan plan;
    plan.combo = true;
    plan.random_reset = true;
    co_await run_icache_test(dut, test, "ibex_icache_stress_all_with_reset",
                             plan);
}

// 100 ms of simulated time is about 5,000,000 cycles at 20 ns, and the longest
// of these runs about 200,000. A sequence that stalls stops here with a wait
// graph rather than running until the harness gives up.
constexpr auto kTestOptions = TestOptions{.simulation_timeout = 100_ms};

CPPTB_REGISTER_TEST_WITH_OPTIONS(ibex_icache_smoke, (kTestOptions));
CPPTB_REGISTER_TEST_WITH_OPTIONS(ibex_icache_passthru, (kTestOptions));
CPPTB_REGISTER_TEST_WITH_OPTIONS(ibex_icache_caching, (kTestOptions));
CPPTB_REGISTER_TEST_WITH_OPTIONS(ibex_icache_invalidation, (kTestOptions));
CPPTB_REGISTER_TEST_WITH_OPTIONS(ibex_icache_many_errors, (kTestOptions));
CPPTB_REGISTER_TEST_WITH_OPTIONS(ibex_icache_back_line, (kTestOptions));
CPPTB_REGISTER_TEST_WITH_OPTIONS(ibex_icache_oldval, (kTestOptions));
CPPTB_REGISTER_TEST_WITH_OPTIONS(ibex_icache_ecc, (kTestOptions));
CPPTB_REGISTER_TEST_WITH_OPTIONS(ibex_icache_stress_all, (kTestOptions));
CPPTB_REGISTER_TEST_WITH_OPTIONS(ibex_icache_stress_all_with_reset,
                                 (kTestOptions));

}  // namespace
}  // namespace cpptb::ports::icache
