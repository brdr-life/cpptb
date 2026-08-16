// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "cpptb/coro_runtime.hpp"
#include "cpptb/dpi_static_binding.hpp"
#include "cpptb/probe.hpp"
#include "cpptb/sim_logging.hpp"
#include "cpptb/test_reporting.hpp"
#include "cpptb/test_result.hpp"
#include "svdpi.h"

// Timing-backend define combinations
// ---------------------------------------------------------------------------
// The SV-DPI pumps are selected by defines that a project sets by hand in
// `design.defines` and `build.cxx_flags`. Only some combinations produce a
// simulator that honours the phase contract in docs/scheduling.md, and the
// difference is one define:
//
//   CPPTB_SV_DPI_TIMING alone            inline pump, contract-violating
//   ... + CPPTB_SV_DPI_NBA_TIMING        passes the conformance contract
//   ... + CPPTB_SV_DPI_CALENDAR_TIMING   passes, and is the faster of the two
//
// The inline pump runs ReadOnly before the design has settled, so a testbench
// built with it reads stale values and captures a ReadWrite-phase write a
// cycle early. It reported nothing: the run completed and the answers were
// wrong. These checks make that a build failure instead.
//
// tests/conformance/runtime measures the inline pump deliberately, to show
// what it does. That is what CPPTB_ALLOW_INVALID_TIMING is for; nothing else
// should define it.

#if defined(CPPTB_SV_DPI_TIMING) && !defined(CPPTB_SV_DPI_NBA_TIMING) && \
    !defined(CPPTB_ALLOW_INVALID_TIMING)
#error \
    "CPPTB_SV_DPI_TIMING on its own selects the inline pump, which runs " \
    "ReadOnly before the design settles and gives wrong answers with no " \
    "diagnostic. Add CPPTB_SV_DPI_NBA_TIMING, or add both that and " \
    "CPPTB_SV_DPI_CALENDAR_TIMING for the faster backend. See " \
    "docs/scheduling.md."
#endif

#if defined(CPPTB_SV_DPI_CALENDAR_TIMING) && !defined(CPPTB_SV_DPI_NBA_TIMING)
#error \
    "CPPTB_SV_DPI_CALENDAR_TIMING requires CPPTB_SV_DPI_NBA_TIMING; the " \
    "calendar pump is built on the NBA one. See docs/scheduling.md."
#endif

#if (defined(CPPTB_SV_DPI_NBA_TIMING) || defined(CPPTB_SV_DPI_CALENDAR_TIMING)) && \
    !defined(CPPTB_SV_DPI_TIMING)
#error \
    "CPPTB_SV_DPI_NBA_TIMING and CPPTB_SV_DPI_CALENDAR_TIMING only select a " \
    "pump when CPPTB_SV_DPI_TIMING is also defined. Without it the generated " \
    "SystemVerilog emits no phase stepping at all. See docs/scheduling.md."
#endif

#if defined(CPPTB_SV_DPI_TIMING) && defined(CPPTB_VERILATOR_DIRECT_TIMING)
#error \
    "CPPTB_SV_DPI_TIMING and CPPTB_VERILATOR_DIRECT_TIMING select different " \
    "timing backends and cannot both be defined. See docs/scheduling.md."
#endif

#if defined(CPPTB_DEFERRED_WRITES) && !defined(CPPTB_VERILATED_TOP)
#error \
    "CPPTB_DEFERRED_WRITES queues every set() until the ReadWrite phase, so " \
    "it needs a timing backend that dispatches phases; without one the " \
    "queue never flushes and every write is silently lost. Set " \
    "build.timing_backend in cpptb.toml, which defines this consistently. " \
    "See docs/scheduling.md."
#endif

namespace cpptb::dpi {

enum class Phase : uint32_t {
    Init = 0,
    Edge = 1,
    Delay = 4,
    ReadWrite = 5,
    ReadOnly = 6,
    NextTimeStep = 7,
};

inline probe::detail::CallbackPhase callback_phase(Phase phase) {
    switch (phase) {
        case Phase::ReadWrite:
            return probe::detail::CallbackPhase::ReadWrite;
        case Phase::ReadOnly:
            return probe::detail::CallbackPhase::ReadOnly;
        case Phase::NextTimeStep:
            return probe::detail::CallbackPhase::NextTimeStep;
        default:
            return probe::detail::CallbackPhase::Evaluation;
    }
}

enum StepResult : uint32_t {
    kStepDone = 1,
    kStepTimerChanged = 8,
    kStepFallingEdges = 16,
    kStepOutputsChanged = 32,
    kStepEdgeInterestChanged = 64,
    kStepNextTickTimer = 128,
    kStepTimerIdle = 256,
    kStepReadWrite = 512,
    kStepReadOnly = 1024,
    kStepNextTimeStep = 2048,
};

template <typename Adapter>
class Runtime {
   public:
    using Dut = typename Adapter::Dut;
    using Result = typename Adapter::Result;
    static constexpr bool static_binding_enabled = [] {
        if constexpr (requires { Dut::cpptb_static_binding; }) {
            return Dut::cpptb_static_binding;
        } else {
            return false;
        }
    }();
    using StaticBindingStorage = std::conditional_t<
        static_binding_enabled, StaticBindingContext, NoStaticBindingContext>;

    Runtime() {
        cpptb::detail::register_sim_log_runtime(this, sim_logs_);
        if constexpr (static_binding_enabled) {
            static_binding_.inputs = inputs_.data();
            static_binding_.outputs = outputs_.data();
            static_binding_.observed_transport_offsets =
                observed_transport_offsets_.data();
            static_binding_.configured_clock = configured_clock_.data();
            static_binding_.edge_observer = edge_observer_.data();
            static_binding_.local_edge_capable = local_edge_capable_.data();
            static_binding_.outputs_dirty = &outputs_dirty_;
            static_binding_.local_edge_delivery_enabled =
                &local_edge_delivery_enabled_;
            static_binding_.dynamic_context = this;
            static_binding_.dynamic_get = signal_get;
            static_binding_.dynamic_set = signal_set;
        }
#if (((defined(VM_VPI) && VM_VPI) || defined(CPPTB_ENABLE_VPI_TIMING)) && \
     !defined(CPPTB_VERILATOR_DIRECT_TIMING))
        phase_callbacks_ = {
            PhaseCallbackData{this, Phase::ReadWrite},
            PhaseCallbackData{this, Phase::ReadOnly},
            PhaseCallbackData{this, Phase::NextTimeStep},
        };
#endif
    }
    ~Runtime() {
        scheduler_.reset();
        cpptb::detail::unregister_sim_log_runtime(this);
    }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    void init(uint32_t iterations, uint64_t timeprecision_fs) {
        probe::detail::DpiCallbackScope callback_scope;
        dpi_scope_ = svGetScope();
        scheduler_.reset();
        sim_logs_.prepare_for_run();
        const char* dpi_scope_name =
            dpi_scope_ ? svGetNameFromScope(dpi_scope_) : nullptr;
        cpptb::detail::update_sim_log_runtime_hierarchy(
            this, dpi_scope_name ? std::string_view{dpi_scope_name}
                                 : std::string_view{});
        inputs_.fill(0);
        outputs_.fill(0);
        driven_.fill(false);
        configured_clock_.fill(false);
        edge_observer_.fill(false);
        local_edge_capable_.fill(false);
        testbench_clock_.fill(false);
        observed_transport_offsets_.fill(kNoTransportOffset);
        on_demand_get_words_.fill(nullptr);
        on_demand_set_words_.fill(nullptr);
        on_demand_base_ids_.fill(0);
        current_inputs_ = nullptr;
        signal_names_.fill("<unknown>");
        result_ = Result{};
        iterations_ = iterations;
        sim_cycles_ = 0;
        outputs_dirty_ = false;
        access_violation_ = false;
        reported_ = false;
        registered_clock_count_ = 0;
        registered_clocks_.fill(RegisteredClock{});
        timeprecision_fs_ = timeprecision_fs;
        clock_registration_open_ = true;
#if (((defined(VM_VPI) && VM_VPI) || defined(CPPTB_ENABLE_VPI_TIMING)) && \
     !defined(CPPTB_VERILATOR_DIRECT_TIMING))
        for (auto& callback : phase_callbacks_) {
            if (callback.pending && callback.handle) {
                vpi_remove_cb(callback.handle);
            }
            callback.pending = false;
            callback.handle = nullptr;
        }
#endif
#ifdef CPPTB_DPI_PROFILE
        profile_step_count_ = 0;
        profile_init_step_count_ = 0;
        profile_clock_edge_step_count_ = 0;
        profile_signal_edge_step_count_ = 0;
        profile_rising_edge_step_count_ = 0;
        profile_falling_edge_step_count_ = 0;
        profile_edge_without_interest_count_ = 0;
        profile_delay_step_count_ = 0;
        profile_next_tick_timer_count_ = 0;
        profile_timer_idle_count_ = 0;
        profile_outputs_changed_count_ = 0;
        profile_output_transfer_count_ = 0;
#endif
#ifdef CPPTB_CORO_FRAME_POOL_DIAGNOSTICS
        coro::detail::coroutine_frame_pool().reset_stats();
#endif

        scheduler_ =
            std::make_unique<coro::Testbench>(coro::SimTime{timeprecision_fs});
        if constexpr (static_binding_enabled) {
            static_binding_.scheduler = scheduler_.get();
            static_binding_.current_inputs = nullptr;
        }

        for (const auto [id, word_count] : Adapter::driven_signal_spans) {
            if (id > driven_.size() || word_count > driven_.size() - id) {
                std::fprintf(stderr, "%s: invalid driven signal span %u+%u\n",
                             Adapter::result_name, id, word_count);
                std::abort();
            }
            for (uint32_t word = 0; word < word_count; ++word) {
                driven_[id + word] = true;
            }
        }

        if constexpr (requires { Adapter::clock_signal_ids; }) {
            for (const auto id : Adapter::clock_signal_ids) {
                if (id >= configured_clock_.size()) {
                    std::fprintf(stderr, "%s: invalid clock signal id %u\n",
                                 Adapter::result_name, id);
                    std::abort();
                }
                configured_clock_[id] = true;
                scheduler_->configure_sticky_edge_source(id);
            }
            local_edge_delivery_enabled_ = true;
        } else {
            local_edge_delivery_enabled_ = false;
        }

        if constexpr (requires { Adapter::edge_observer_signal_ids; }) {
            for (const auto id : Adapter::edge_observer_signal_ids) {
                if (id >= edge_observer_.size()) {
                    std::fprintf(stderr,
                                 "%s: invalid edge observer signal id %u\n",
                                 Adapter::result_name, id);
                    std::abort();
                }
                edge_observer_[id] = true;
            }
        }

        dut_ = Adapter::bind_dut([this](auto&&... args) {
            return make_signal(std::forward<decltype(args)>(args)...);
        });
        validate_transport_completeness();
        start_ = std::chrono::steady_clock::now();
        const coro::ClockRegistrar clocks{this, register_clock_callback};
        if constexpr (requires {
                          Adapter::register_testbench(
                              *scheduler_, dut_, iterations_, result_, clocks,
                              sim_logs_);
                      }) {
            Adapter::register_testbench(*scheduler_, dut_, iterations_,
                                        result_, clocks, sim_logs_);
        } else if constexpr (requires {
                          Adapter::register_testbench(
                              *scheduler_, dut_, iterations_, result_, clocks);
                      }) {
            Adapter::register_testbench(*scheduler_, dut_, iterations_,
                                        result_, clocks);
        } else {
            Adapter::register_testbench(*scheduler_, dut_, iterations_,
                                        result_);
        }
        const char* list_tests = std::getenv("CPPTB_LIST_TESTS");
        const bool listing = list_tests && list_tests[0] != '\0' &&
                             std::string_view{list_tests} != "0" &&
                             std::string_view{list_tests} != "false";
        if (!listing) validate_registered_clock_setup();
        clock_registration_open_ = false;
    }

    int step(uint32_t phase_value, uint64_t sim_time, uint64_t sim_cycles,
             uint32_t event_signal_id, uint32_t event_edge,
             const svOpenArrayHandle in_words) {
        if (!scheduler_) {
            std::fprintf(stderr, "%s: DPI step called before init\n",
                         Adapter::result_name);
            return -1;
        }

        const auto phase = static_cast<Phase>(phase_value);
        probe::detail::DpiCallbackScope callback_scope{callback_phase(phase)};

#ifdef CPPTB_DPI_PROFILE
        ++profile_step_count_;
        switch (phase) {
            case Phase::Init:
                ++profile_init_step_count_;
                break;
            case Phase::Edge:
                if (event_signal_id < configured_clock_.size() &&
                    configured_clock_[event_signal_id]) {
                    ++profile_clock_edge_step_count_;
                } else {
                    ++profile_signal_edge_step_count_;
                }
                if (event_edge == 0u) {
                    ++profile_rising_edge_step_count_;
                } else if (event_edge == 1u) {
                    ++profile_falling_edge_step_count_;
                }
                if (!scheduler_->has_edge_interest(
                        event_signal_id,
                        static_cast<coro::EdgeKind>(event_edge))) {
                    ++profile_edge_without_interest_count_;
                }
                break;
            case Phase::Delay:
                ++profile_delay_step_count_;
                break;
            case Phase::ReadWrite:
                ++profile_read_write_step_count_;
                break;
            case Phase::ReadOnly:
                ++profile_read_only_step_count_;
                break;
            case Phase::NextTimeStep:
                ++profile_next_time_step_count_;
                break;
        }
#endif

        const auto* input_data = array_ptr(in_words, "input");
        InputViewScope input_view{*this, input_data};
        if constexpr (!(requires {
                            Adapter::compact_input_transport;
                            Adapter::observed_signal_word_ids;
                        } && Adapter::compact_input_transport)) {
            std::memcpy(inputs_.data(), input_data,
                        inputs_.size() * sizeof(std::uint32_t));
        }
        sim_cycles_ = sim_cycles;
        scheduler_->set_time(sim_time);

        switch (phase) {
            case Phase::Init:
            case Phase::Delay:
                break;
            case Phase::Edge:
                if (event_edge > static_cast<uint32_t>(coro::EdgeKind::Any)) {
                    std::fprintf(stderr, "%s: unknown edge %u\n",
                                 Adapter::result_name, event_edge);
                    return -1;
                }
                if (event_signal_id < configured_clock_.size() &&
                    configured_clock_[event_signal_id] &&
                    testbench_clock_[event_signal_id]) {
                    outputs_[event_signal_id] =
                        event_edge ==
                                static_cast<uint32_t>(coro::EdgeKind::Rising)
                            ? 1u
                            : 0u;
                }
                scheduler_->notify_edge(
                    event_signal_id, static_cast<coro::EdgeKind>(event_edge));
                break;
            case Phase::ReadWrite:
                scheduler_->notify_read_write();
                break;
            case Phase::ReadOnly:
                scheduler_->notify_read_only();
                break;
            case Phase::NextTimeStep:
                scheduler_->notify_next_time_step();
                break;
            default:
                std::fprintf(stderr, "%s: unknown phase %u\n",
                             Adapter::result_name, phase_value);
                return -1;
        }

        const bool edge_interest_changed = sync_edge_interest_changes();
        if (access_violation_) return -1;

        const bool outputs_changed = outputs_dirty_;
#ifdef CPPTB_DPI_PROFILE
        if (outputs_changed) ++profile_outputs_changed_count_;
#endif
        outputs_dirty_ = false;

        if (Adapter::timed_out(scheduler_->now(), sim_cycles_, iterations_)) {
            std::fprintf(stderr,
                         "%s: timed out at time %llu after %llu cycles\n",
                         Adapter::result_name,
                         static_cast<unsigned long long>(sim_time),
                         static_cast<unsigned long long>(sim_cycles_));
            report_timeout_diagnostics("simulator watchdog expired");
            return -1;
        }

        const int completion = report_if_done();
        if (completion < 0) return -1;

        uint32_t requests = completion > 0 ? kStepDone : 0;
        if (scheduler_->consume_timer_schedule_changed()) {
            const uint64_t deadline = scheduler_->next_timer_deadline();
            if (deadline == std::numeric_limits<uint64_t>::max()) {
                requests |= kStepTimerIdle;
#ifdef CPPTB_DPI_PROFILE
                ++profile_timer_idle_count_;
#endif
            } else if (deadline > scheduler_->now_ticks() &&
                       deadline - scheduler_->now_ticks() == 1u) {
                requests |= kStepNextTickTimer;
#ifdef CPPTB_DPI_PROFILE
                ++profile_next_tick_timer_count_;
#endif
            } else {
                requests |= kStepTimerChanged;
            }
        }
        if (scheduler_->has_falling_edge_waiters()) {
            requests |= kStepFallingEdges;
        }
        if (outputs_changed) {
            requests |= kStepOutputsChanged;
        }
        if (edge_interest_changed) {
            requests |= kStepEdgeInterestChanged;
        }
#ifdef CPPTB_SV_DPI_TIMING
        if (scheduler_->has_read_write_waiters()) {
            requests |= kStepReadWrite;
        }
        if (scheduler_->has_read_only_waiters()) {
            requests |= kStepReadOnly;
        }
        if (scheduler_->has_next_time_step_waiters()) {
            requests |= kStepNextTimeStep;
        }
#endif
        if (!schedule_phase_callbacks()) {
            return -1;
        }
        return static_cast<int>(requests);
    }

    void pull_outputs(const svOpenArrayHandle out_words) const {
        if (!scheduler_) {
            std::fprintf(stderr, "%s: DPI output pull called before init\n",
                         Adapter::result_name);
            std::abort();
        }
        copy_outputs(out_words);
#ifdef CPPTB_DPI_PROFILE
        ++profile_output_transfer_count_;
#endif
    }

    uint64_t next_timer_deadline() {
        if (!scheduler_) return std::numeric_limits<uint64_t>::max();
        return scheduler_->next_timer_deadline();
    }

    uint8_t edge_interest(uint32_t signal_id) const {
        return scheduler_ ? scheduler_->edge_interest(signal_id)
                          : coro::kEdgeInterestNone;
    }

    int report_starvation() {
        if (!scheduler_ || reported_ || scheduler_->done()) return 0;
        report_timeout_diagnostics(
            "simulation ended with active scheduler processes");
        return -1;
    }

    uint32_t pending_phase_mask() const {
        if (!scheduler_) return 0;
        uint32_t phases = 0;
        if (scheduler_->has_read_write_waiters()) {
            phases |= 1u << static_cast<uint32_t>(Phase::ReadWrite);
        }
        if (scheduler_->has_read_only_waiters()) {
            phases |= 1u << static_cast<uint32_t>(Phase::ReadOnly);
        }
        if (scheduler_->has_next_time_step_waiters()) {
            phases |= 1u << static_cast<uint32_t>(Phase::NextTimeStep);
        }
        return phases;
    }

    void dispatch_phase(Phase phase) {
        if (phase != Phase::ReadWrite && phase != Phase::ReadOnly &&
            phase != Phase::NextTimeStep) {
            std::fprintf(stderr, "%s: invalid direct timing phase %u\n",
                         Adapter::result_name,
                         static_cast<uint32_t>(phase));
            std::abort();
        }
        const svScope previous_scope = svGetScope();
        if (!dpi_scope_) {
            std::fprintf(stderr,
                         "%s: direct timing dispatch has no DPI scope\n",
                         Adapter::result_name);
            std::abort();
        }
        svSetScope(dpi_scope_);
        Adapter::dispatch_phase(static_cast<uint32_t>(phase));
        if (previous_scope) svSetScope(previous_scope);
    }

    uint64_t clock_config(uint32_t signal_id, uint32_t field) const {
        for (uint32_t index = 0; index < registered_clock_count_; ++index) {
            const auto& clock = registered_clocks_[index];
            if (clock.signal_id != signal_id) continue;
            switch (field) {
                case 0:
                    return clock.half_period_ticks;
                case 1:
                    return clock.phase_ticks;
                case 2:
                    return clock.primary ? 1u : 0u;
                default:
                    std::fprintf(stderr, "%s: invalid clock field %u\n",
                                 Adapter::result_name, field);
                    return 0;
            }
        }
        return 0;
    }

    uint64_t edge_interest_generation() const {
        return scheduler_ ? scheduler_->edge_interest_generation() : 0;
    }

    std::optional<coro::EdgeInterestChange> consume_edge_interest_change() {
        return scheduler_ ? scheduler_->consume_edge_interest_change()
                          : std::nullopt;
    }

   private:
    struct RegisteredClock {
        uint32_t signal_id = 0;
        uint64_t half_period_ticks = 0;
        uint64_t phase_ticks = 0;
        bool primary = false;
    };

#if (((defined(VM_VPI) && VM_VPI) || defined(CPPTB_ENABLE_VPI_TIMING)) && \
     !defined(CPPTB_VERILATOR_DIRECT_TIMING))
    struct PhaseCallbackData {
        Runtime* runtime = nullptr;
        Phase phase = Phase::ReadWrite;
        vpiHandle handle = nullptr;
        bool pending = false;
    };

    static PLI_INT32 phase_callback(p_cb_data callback) {
        auto* data = reinterpret_cast<PhaseCallbackData*>(callback->user_data);
        if (!data || !data->runtime) {
            std::fprintf(stderr, "cpptb: invalid simulator phase callback\n");
            return 0;
        }
        data->pending = false;
        data->handle = nullptr;
        data->runtime->dispatch_phase(data->phase);
        return 0;
    }

    bool arm_phase_callback(size_t index, PLI_INT32 reason) {
        auto& callback = phase_callbacks_[index];
        if (callback.pending) return true;

        s_cb_data registration{};
        registration.reason = reason;
        registration.cb_rtn = phase_callback;
        registration.user_data =
            reinterpret_cast<PLI_BYTE8*>(&callback);
        callback.handle = vpi_register_cb(&registration);
        if (!callback.handle) {
            std::fprintf(stderr,
                         "%s: simulator rejected the %s timing callback\n",
                         Adapter::result_name, phase_name(callback.phase));
            return false;
        }
        callback.pending = true;
#ifdef CPPTB_DPI_PROFILE
        ++profile_phase_callback_registration_count_[index];
#endif
        return true;
    }

    static const char* phase_name(Phase phase) {
        switch (phase) {
            case Phase::ReadWrite:
                return "ReadWrite";
            case Phase::ReadOnly:
                return "ReadOnly";
            case Phase::NextTimeStep:
                return "NextTimeStep";
            default:
                return "unknown";
        }
    }
#endif

    bool schedule_phase_callbacks() {
        const bool read_write = scheduler_->has_read_write_waiters();
        const bool read_only = scheduler_->has_read_only_waiters();
        const bool next_time_step = scheduler_->has_next_time_step_waiters();
        if (!read_write && !read_only && !next_time_step) return true;

#if defined(CPPTB_VERILATOR_DIRECT_TIMING) || defined(CPPTB_SV_DPI_TIMING)
        return true;
#elif (defined(VM_VPI) && VM_VPI) || defined(CPPTB_ENABLE_VPI_TIMING)
        return (!read_write || arm_phase_callback(0, cbReadWriteSynch)) &&
               (!read_only || arm_phase_callback(1, cbReadOnlySynch)) &&
               (!next_time_step || arm_phase_callback(2, cbNextSimTime));
#else
        std::fprintf(
            stderr,
            "%s: ReadWrite, ReadOnly, and NextTimeStep need a timing backend "
            "that dispatches simulator phases, and this build has none: the "
            "default `cpptb build` links Verilator's own --binary main, which "
            "owns clocks and timers but dispatches no phases.\n"
            "  Select a backend in cpptb.toml -- both hold the complete "
            "documented contract:\n"
            "      [build]\n"
            "      timing_backend = \"verilator-direct\"   # fastest; "
            "Verilator's scheduler, driven directly\n"
            "      timing_backend = \"vpi\"                # standard VPI "
            "callbacks; the portable route\n"
            "  Or wait on a clock edge instead: sample after `co_await "
            "RisingEdge{clk}`, drive after `co_await FallingEdge{clk}` -- "
            "what the ports in experiments/open_core_ports use.\n"
            "  Do not hand-roll `--vpi` in verilator_args: it places writes "
            "on the right edge but fails the contract silently (ReadOnly "
            "does not observe a write settled in ReadWrite); the build tool "
            "now rejects it. See docs/scheduling.md.\n",
            Adapter::result_name);
        return false;
#endif
    }

    static void register_clock_callback(void* context, coro::Signal signal,
                                        coro::SimTime period,
                                        coro::SimTime phase) {
        static_cast<Runtime*>(context)->register_clock(signal, period, phase);
    }

    void register_clock(coro::Signal signal, coro::SimTime period,
                        coro::SimTime phase) {
        if (!clock_registration_open_) {
            std::fprintf(stderr,
                         "%s: clocks must be started before the test's first "
                         "await\n",
                         Adapter::result_name);
            std::abort();
        }
        const uint32_t id = signal.id;
        const RegisteredClockConfig* expected = nullptr;
        if constexpr (requires { Adapter::registered_clock_configs; }) {
            for (const auto& config : Adapter::registered_clock_configs) {
                if (config.signal_id == id) expected = &config;
            }
        }
        if (id >= driven_.size() || (!driven_[id] && !expected) ||
            !local_edge_capable_[id]) {
            std::fprintf(stderr,
                         "%s: clock '%s' must be a writable one-bit DUT port\n",
                         Adapter::result_name,
                         signal.name ? signal.name : "<unnamed>");
            std::abort();
        }
        for (uint32_t index = 0; index < registered_clock_count_; ++index) {
            if (registered_clocks_[index].signal_id == id) {
                std::fprintf(stderr,
                             "%s: clock '%s' was started more than once\n",
                             Adapter::result_name,
                             signal.name ? signal.name : "<unnamed>");
                std::abort();
            }
        }
        if (configured_clock_[id] && !expected) {
            std::fprintf(stderr, "%s: clock '%s' was started more than once\n",
                         Adapter::result_name,
                         signal.name ? signal.name : "<unnamed>");
            std::abort();
        }
        if (period.femtoseconds == 0 ||
            (period.femtoseconds % 2u) != 0) {
            std::fprintf(stderr,
                         "%s: clock '%s' period must be positive and even\n",
                         Adapter::result_name,
                         signal.name ? signal.name : "<unnamed>");
            std::abort();
        }
        const uint64_t half_period_fs = period.femtoseconds / 2u;
        if ((half_period_fs % timeprecision_fs_) != 0 ||
            (phase.femtoseconds % timeprecision_fs_) != 0) {
            std::fprintf(stderr,
                         "%s: clock '%s' timing is not representable at the "
                         "simulator precision\n",
                         Adapter::result_name,
                         signal.name ? signal.name : "<unnamed>");
            std::abort();
        }
        if (expected &&
            (expected->period_fs != period.femtoseconds ||
             expected->phase_fs != phase.femtoseconds ||
             expected->initial_value != (outputs_[id] & 1u))) {
            std::fprintf(
                stderr,
                "%s: clock '%s' timing changed after clock discovery; "
                "regenerate the testbench wrapper\n",
                Adapter::result_name,
                signal.name ? signal.name : "<unnamed>");
            std::abort();
        }
        if (registered_clock_count_ >= registered_clocks_.size()) {
            std::fprintf(stderr, "%s: too many registered clocks\n",
                         Adapter::result_name);
            std::abort();
        }

        registered_clocks_[registered_clock_count_] = RegisteredClock{
            id, half_period_fs / timeprecision_fs_,
            phase.femtoseconds / timeprecision_fs_,
            registered_clock_count_ == 0};
        ++registered_clock_count_;
        configured_clock_[id] = true;
        testbench_clock_[id] = true;
        scheduler_->configure_sticky_edge_source(id);
    }

    void validate_registered_clock_setup() const {
        if constexpr (requires { Adapter::registered_clock_configs; }) {
            for (const auto& expected : Adapter::registered_clock_configs) {
                bool found = false;
                for (uint32_t index = 0; index < registered_clock_count_;
                     ++index) {
                    found = found ||
                            registered_clocks_[index].signal_id ==
                                expected.signal_id;
                }
                if (!found) {
                    std::fprintf(
                        stderr,
                        "%s: a clock captured during discovery is no longer "
                        "started; regenerate the testbench wrapper\n",
                        Adapter::result_name);
                    std::abort();
                }
            }
        }
    }

    static constexpr uint32_t kNoTransportOffset =
        std::numeric_limits<uint32_t>::max();
    class InputViewScope {
       public:
        InputViewScope(Runtime& runtime, const uint32_t* inputs)
            : runtime_(runtime), previous_(runtime.current_inputs_) {
            runtime_.current_inputs_ = inputs;
            if constexpr (static_binding_enabled) {
                runtime_.static_binding_.current_inputs = inputs;
            }
        }

        ~InputViewScope() {
            runtime_.current_inputs_ = previous_;
            if constexpr (static_binding_enabled) {
                runtime_.static_binding_.current_inputs = previous_;
            }
        }

       private:
        Runtime& runtime_;
        const uint32_t* previous_;
    };

    void validate_transport_word(
        uint32_t id, bool expected_driven,
        std::array<bool, Adapter::signal_count>& role_seen) const {
        if (id >= role_seen.size() || role_seen[id] ||
            driven_[id] != expected_driven) {
            std::fprintf(stderr, "%s: invalid DPI transport word %u\n",
                         Adapter::result_name, id);
            std::abort();
        }
        role_seen[id] = true;
    }

    void register_on_demand(uint32_t id, uint32_t word_count,
                            OnDemandGetWordsFn get_words,
                            OnDemandSetWordsFn set_words) {
        if (!get_words || id > on_demand_get_words_.size() ||
            word_count > on_demand_get_words_.size() - id) {
            std::fprintf(stderr, "%s: invalid on-demand signal span %u+%u\n",
                         Adapter::result_name, id, word_count);
            std::abort();
        }
        for (uint32_t word = 0; word < word_count; ++word) {
            const uint32_t word_id = id + word;
            if (on_demand_get_words_[word_id] ||
                driven_[word_id] != (set_words != nullptr)) {
                std::fprintf(stderr,
                             "%s: invalid on-demand signal word %u\n",
                             Adapter::result_name, word_id);
                std::abort();
            }
            on_demand_get_words_[word_id] = get_words;
            on_demand_set_words_[word_id] = set_words;
            on_demand_base_ids_[word_id] = id;
        }
    }

    void validate_transport_completeness() {
        if constexpr (requires {
                          Adapter::observed_signal_word_ids;
                          Adapter::driven_signal_word_ids;
                      }) {
            std::array<bool, Adapter::signal_count> transport_seen{};
            std::array<bool, Adapter::signal_count> observed_seen{};
            std::array<bool, Adapter::signal_count> driven_seen{};
            for (size_t word = 0;
                 word < Adapter::observed_signal_word_ids.size(); ++word) {
                const auto id = Adapter::observed_signal_word_ids[word];
                if (id >= observed_seen.size() || observed_seen[id]) {
                    std::fprintf(stderr,
                                 "%s: invalid observed DPI transport word %u\n",
                                 Adapter::result_name, id);
                    std::abort();
                }
                observed_seen[id] = true;
                transport_seen[id] = true;
                observed_transport_offsets_[id] = static_cast<uint32_t>(word);
            }
            for (const auto id : Adapter::driven_signal_word_ids) {
                validate_transport_word(id, true, driven_seen);
                transport_seen[id] = true;
            }
            for (uint32_t id = 0; id < transport_seen.size(); ++id) {
                if (on_demand_get_words_[id]) {
                    if (transport_seen[id]) {
                        std::fprintf(
                            stderr,
                            "%s: signal word %u has duplicate DPI transport\n",
                            Adapter::result_name, id);
                        std::abort();
                    }
                    transport_seen[id] = true;
                }
            }
            if constexpr (requires {
                              Adapter::transportless_edge_signal_ids;
                          }) {
                for (const auto id : Adapter::transportless_edge_signal_ids) {
                    if (id >= transport_seen.size() || transport_seen[id]) {
                        std::fprintf(
                            stderr,
                            "%s: invalid transportless edge signal %u\n",
                            Adapter::result_name, id);
                        std::abort();
                    }
                    transport_seen[id] = true;
                }
            }
            for (uint32_t id = 0; id < transport_seen.size(); ++id) {
                if (!transport_seen[id]) {
                    std::fprintf(stderr,
                                 "%s: signal word %u is missing from DPI transport\n",
                                 Adapter::result_name, id);
                    std::abort();
                }
            }
        }
    }

    bool sync_edge_interest_changes() {
        bool observer_changed = false;
        while (auto change = scheduler_->consume_edge_interest_change()) {
            const uint32_t id = change->signal_id;
            if (id >= inputs_.size()) {
                std::fprintf(stderr, "%s: invalid edge wait signal id %u\n",
                             Adapter::result_name, id);
                access_violation_ = true;
                continue;
            }
            const bool local_source = driven_[id] && local_edge_capable_[id] &&
                                      !configured_clock_[id];
            const bool available = local_source || configured_clock_[id] ||
                                   edge_observer_[id];
            if (change->interest != coro::kEdgeInterestNone && !available) {
                std::fprintf(stderr,
                             "%s: signal '%s' has no generated edge observer\n",
                             Adapter::result_name, signal_names_[id]);
                access_violation_ = true;
                continue;
            }
            observer_changed = observer_changed || configured_clock_[id] ||
                               edge_observer_[id];
        }
        return observer_changed;
    }

    static uint32_t signal_get(void* context, uint32_t id) {
        return static_cast<Runtime*>(context)->get(id);
    }

    static void signal_set(void* context, uint32_t id, uint32_t value) {
        static_cast<Runtime*>(context)->set(id, value);
    }

    template <bool Driven, uint32_t Id, uint32_t WordCount>
    void register_static_metadata(const char* name) {
        static_assert(Id <= Adapter::signal_count);
        static_assert(WordCount <= Adapter::signal_count - Id);
        for (uint32_t word = 0; word < WordCount; ++word) {
            if (driven_[Id + word] != Driven) {
                std::fprintf(
                    stderr,
                    "%s: static signal word %u has inconsistent direction\n",
                    Adapter::result_name, Id + word);
                std::abort();
            }
            signal_names_[Id + word] = name ? name : "<unnamed>";
        }
    }

    template <bool Driven, uint32_t Id, uint32_t WordCount,
              uint32_t TransportOffset>
    void validate_static_packed_binding() const {
        if constexpr (Driven) {
            static_assert(TransportOffset <=
                          Adapter::driven_signal_word_ids.size());
            static_assert(
                WordCount <= Adapter::driven_signal_word_ids.size() -
                                 TransportOffset);
            for (uint32_t word = 0; word < WordCount; ++word) {
                if (Adapter::driven_signal_word_ids[TransportOffset + word] !=
                    Id + word) {
                    std::fprintf(stderr,
                                 "%s: stale packed static offset for word %u\n",
                                 Adapter::result_name, Id + word);
                    std::abort();
                }
            }
        } else {
            static_assert(TransportOffset <=
                          Adapter::observed_signal_word_ids.size());
            static_assert(
                WordCount <= Adapter::observed_signal_word_ids.size() -
                                 TransportOffset);
            for (uint32_t word = 0; word < WordCount; ++word) {
                if (Adapter::observed_signal_word_ids[TransportOffset + word] !=
                    Id + word) {
                    std::fprintf(stderr,
                                 "%s: stale packed static offset for word %u\n",
                                 Adapter::result_name, Id + word);
                    std::abort();
                }
            }
        }
    }

    template <size_t Width, bool Writable, bool Driven, uint32_t Id,
              uint32_t TransportOffset>
    auto make_signal(
        StaticPackedSignalSpec<Width, Writable, Driven, Id, TransportOffset>,
        const char* name) {
        static_assert(static_binding_enabled);
        constexpr uint32_t word_count = (Width + 31) / 32;
        register_static_metadata<Driven, Id, word_count>(name);
        validate_static_packed_binding<Driven, Id, word_count,
                                       TransportOffset>();
        if constexpr (Width <= 32) local_edge_capable_[Id] = true;
        return StaticPackedSignal<Width, Writable, Driven, Id,
                                  TransportOffset>{&static_binding_, name};
    }

    template <size_t Width, bool Writable, bool Driven, uint32_t Id,
              OnDemandGetWordsFn GetWords, OnDemandSetWordsFn SetWords>
    auto make_signal(
        StaticOnDemandSignalSpec<Width, Writable, Driven, Id, GetWords,
                                 SetWords>,
        const char* name) {
        static_assert(static_binding_enabled);
        constexpr uint32_t word_count = (Width + 31) / 32;
        register_static_metadata<Driven, Id, word_count>(name);
        register_on_demand(Id, word_count, GetWords, SetWords);
        if constexpr (Width <= 32) local_edge_capable_[Id] = true;
        return StaticOnDemandSignal<Width, Writable, Driven, Id>{
            &static_binding_, name, GetWords, SetWords};
    }

    template <size_t Width, bool Writable, bool Driven, uint32_t Id,
              uint32_t TransportOffset, typename... Dimensions>
    auto make_signal(
        StaticPackedArraySpec<Width, Writable, Driven, Id, TransportOffset,
                              Dimensions...>,
        const char* name) {
        static_assert(static_binding_enabled);
        constexpr uint32_t element_count =
            static_cast<uint32_t>((Dimensions::size * ...));
        constexpr uint32_t element_words = (Width + 31) / 32;
        constexpr uint32_t word_count = element_count * element_words;
        register_static_metadata<Driven, Id, word_count>(name);
        validate_static_packed_binding<Driven, Id, word_count,
                                       TransportOffset>();
        if constexpr (Width <= 32) {
            for (uint32_t element = 0; element < element_count; ++element) {
                local_edge_capable_[Id + element] = true;
            }
        }
        return StaticPackedFixedArray<Width, Writable, Driven, Id,
                                      TransportOffset, 0, Dimensions...>{
            &static_binding_, name, 0, 0};
    }

    template <size_t Width, bool Writable, bool Driven, uint32_t Id,
              OnDemandGetWordsFn GetWords, OnDemandSetWordsFn SetWords,
              typename... Dimensions>
    auto make_signal(
        StaticOnDemandArraySpec<Width, Writable, Driven, Id, GetWords,
                                SetWords, Dimensions...>,
        const char* name) {
        static_assert(static_binding_enabled);
        constexpr uint32_t element_count =
            static_cast<uint32_t>((Dimensions::size * ...));
        constexpr uint32_t element_words = (Width + 31) / 32;
        constexpr uint32_t word_count = element_count * element_words;
        register_static_metadata<Driven, Id, word_count>(name);
        register_on_demand(Id, word_count, GetWords, SetWords);
        if constexpr (Width <= 32) {
            for (uint32_t element = 0; element < element_count; ++element) {
                local_edge_capable_[Id + element] = true;
            }
        }
        return StaticOnDemandFixedArray<Width, Writable, Driven, Id, 0,
                                        Dimensions...>{
            &static_binding_, name, GetWords, SetWords, 0, 0};
    }

    template <typename Spec>
    auto make_signal(Spec spec, uint32_t id, const char* name)
        requires(Spec::on_demand)
    {
        register_on_demand(id, spec.word_count, spec.get_words_fn,
                           spec.set_words_fn);
        return make_signal(spec.transport_spec, id, name);
    }

#ifdef CPPTB_CORO_PACKED_SIGNAL_API
    static void signal_get_words(void* context, uint32_t id, uint32_t* words,
                                 uint32_t word_count) {
        static_cast<Runtime*>(context)->get_words(id, words, word_count);
    }

    static void signal_set_words(void* context, uint32_t id,
                                 const uint32_t* words,
                                 uint32_t word_count) {
        static_cast<Runtime*>(context)->set_words(id, words, word_count);
    }
#endif

    coro::Signal make_signal(uint32_t id, const char* name) {
        if (id >= inputs_.size()) {
            std::fprintf(stderr, "%s: invalid signal id %u\n",
                         Adapter::result_name, id);
            std::abort();
        }
        signal_names_[id] = name ? name : "<unnamed>";
        local_edge_capable_[id] = true;
        return coro::Signal{nullptr, id, name, this, signal_get, signal_set};
    }

#ifdef CPPTB_CORO_PACKED_SIGNAL_API
    template <size_t Width, bool Writable>
    auto make_signal(coro::SignalSpec<Width, Writable>, uint32_t id,
                     const char* name) {
        if constexpr (Width <= 32) {
            return make_signal(id, name);
        } else {
            constexpr uint32_t word_count = (Width + 31) / 32;
            if (id > inputs_.size() ||
                word_count > inputs_.size() - id) {
                std::fprintf(stderr, "%s: invalid packed signal id %u\n",
                             Adapter::result_name, id);
                std::abort();
            }
            for (uint32_t word = 0; word < word_count; ++word) {
                signal_names_[id + word] = name ? name : "<unnamed>";
            }
            return coro::PackedSignal<Width, Writable>{
                id, name, this, signal_get_words, signal_set_words};
        }
    }
#endif

#ifdef CPPTB_CORO_UNPACKED_ARRAY_API
    template <size_t Width, int32_t Left, int32_t Right, bool Writable>
    auto make_signal(coro::ArraySpec<Width, Left, Right, Writable>, uint32_t id,
                     const char* name) {
        constexpr uint32_t element_count =
            static_cast<uint32_t>((Left < Right ? Right - Left : Left - Right) + 1);
        constexpr uint32_t element_words = (Width + 31) / 32;
        constexpr uint32_t word_count = element_count * element_words;
        if (id > inputs_.size() || word_count > inputs_.size() - id) {
            std::fprintf(stderr, "%s: invalid unpacked array signal id %u\n",
                         Adapter::result_name, id);
            std::abort();
        }
        for (uint32_t word = 0; word < word_count; ++word) {
            signal_names_[id + word] = name ? name : "<unnamed>";
        }
        if constexpr (Width <= 32) {
            for (uint32_t element = 0; element < element_count; ++element) {
                local_edge_capable_[id + element] = true;
            }
        }
        return coro::UnpackedArray<Width, Left, Right, Writable>{
            id, name, this, signal_get, signal_set, signal_get_words,
            signal_set_words};
    }
#endif

    uint32_t get(uint32_t id) const {
        if (id >= inputs_.size()) {
            std::fprintf(stderr, "%s: invalid get id %u\n",
                         Adapter::result_name, id);
            std::abort();
        }
        if (on_demand_get_words_[id]) {
            uint32_t value = 0;
            on_demand_get_words_[id](id - on_demand_base_ids_[id], &value, 1);
            return value;
        }
        if (driven_[id] && !configured_clock_[id]) return outputs_[id];
        if constexpr (requires {
                          Adapter::compact_input_transport;
                          Adapter::observed_signal_word_ids;
                      } && Adapter::compact_input_transport) {
            if (current_inputs_) {
                const uint32_t offset = observed_transport_offsets_[id];
                if (offset == kNoTransportOffset) {
                    std::fprintf(stderr,
                                 "%s: observed signal %u has no transport offset\n",
                                 Adapter::result_name, id);
                    std::abort();
                }
                return current_inputs_[offset];
            }
        }
        return inputs_[id];
    }

    void set(uint32_t id, uint32_t value) {
        if (id >= outputs_.size()) {
            std::fprintf(stderr, "%s: invalid set id %u\n",
                         Adapter::result_name, id);
            std::abort();
        }
        if (!driven_[id]) {
            std::fprintf(stderr,
                         "%s: cannot drive DUT output signal '%s'\n",
                         Adapter::result_name, signal_names_[id]);
            access_violation_ = true;
            return;
        }
        probe::detail::require_write_allowed(signal_names_[id], "set");
        if (on_demand_set_words_[id]) {
            const uint32_t previous = get(id);
            if (previous == value) return;
            on_demand_set_words_[id](id - on_demand_base_ids_[id], &value, 1);
            deliver_local_edge(id, previous, value);
            return;
        }
        const uint32_t previous = outputs_[id];
        if (previous == value) return;
        outputs_[id] = value;
        outputs_dirty_ = true;
        deliver_local_edge(id, previous, value);
    }

    void deliver_local_edge(uint32_t id, uint32_t previous, uint32_t value) {
        if (!local_edge_delivery_enabled_ || !scheduler_ ||
            !local_edge_capable_[id] || configured_clock_[id] ||
            edge_observer_[id]) {
            return;
        }

        coro::EdgeKind edge = coro::EdgeKind::Any;
        if (previous == 0 && value != 0) {
            edge = coro::EdgeKind::Rising;
        } else if (previous != 0 && value == 0) {
            edge = coro::EdgeKind::Falling;
        }
        if (scheduler_->has_edge_interest(id, edge)) {
            scheduler_->notify_edge(id, edge);
        }
    }

    void get_words(uint32_t id, uint32_t* words,
                   uint32_t word_count) const {
        if (!words || id > inputs_.size() ||
            word_count > inputs_.size() - id) {
            std::fprintf(stderr, "%s: invalid packed get id %u\n",
                         Adapter::result_name, id);
            std::abort();
        }
        if (on_demand_get_words_[id]) {
            on_demand_get_words_[id](id - on_demand_base_ids_[id], words,
                                     word_count);
            return;
        }
        if (driven_[id] && !configured_clock_[id]) {
            for (uint32_t word = 0; word < word_count; ++word) {
                words[word] = outputs_[id + word];
            }
            return;
        }
        if constexpr (requires {
                          Adapter::compact_input_transport;
                          Adapter::observed_signal_word_ids;
                      } && Adapter::compact_input_transport) {
            if (current_inputs_) {
                for (uint32_t word = 0; word < word_count; ++word) {
                    const uint32_t offset = observed_transport_offsets_[id + word];
                    if (offset == kNoTransportOffset) {
                        std::fprintf(
                            stderr,
                            "%s: observed signal word %u has no transport offset\n",
                            Adapter::result_name, id + word);
                        std::abort();
                    }
                    words[word] = current_inputs_[offset];
                }
                return;
            }
        }
        for (uint32_t word = 0; word < word_count; ++word) {
            words[word] = inputs_[id + word];
        }
    }

    void set_words(uint32_t id, const uint32_t* words,
                   uint32_t word_count) {
        if (!words || id > outputs_.size() ||
            word_count > outputs_.size() - id) {
            std::fprintf(stderr, "%s: invalid packed set id %u\n",
                         Adapter::result_name, id);
            std::abort();
        }
        if (!driven_[id]) {
            std::fprintf(stderr,
                         "%s: cannot drive DUT output signal '%s'\n",
                         Adapter::result_name, signal_names_[id]);
            access_violation_ = true;
            return;
        }
        probe::detail::require_write_allowed(signal_names_[id], "set");

        if (on_demand_set_words_[id]) {
            on_demand_set_words_[id](id - on_demand_base_ids_[id], words,
                                     word_count);
            return;
        }

        bool changed = false;
        for (uint32_t word = 0; word < word_count; ++word) {
            changed = changed || outputs_[id + word] != words[word];
            outputs_[id + word] = words[word];
        }
        outputs_dirty_ = outputs_dirty_ || changed;
    }

    static uint32_t* array_ptr(const svOpenArrayHandle words,
                               const char* label) {
        auto* pointer = static_cast<uint32_t*>(svGetArrayPtr(words));
        if (!pointer) {
            std::fprintf(stderr, "%s: cannot access %s array\n",
                         Adapter::result_name, label);
            std::abort();
        }
        return pointer;
    }

    void copy_outputs(const svOpenArrayHandle words) const {
        auto* data = array_ptr(words, "output");
        if constexpr (requires { Adapter::driven_signal_word_ids; }) {
            for (size_t word = 0;
                 word < Adapter::driven_signal_word_ids.size(); ++word) {
                data[word] = outputs_[Adapter::driven_signal_word_ids[word]];
            }
        } else {
            std::memcpy(data, outputs_.data(),
                        outputs_.size() * sizeof(std::uint32_t));
        }
    }

    bool result_succeeded() const {
        if constexpr (std::is_base_of_v<TestResult, Result>) {
            if (result_.status != TestStatus::NotRun &&
                result_.status != TestStatus::Running) {
                return test_status_successful(result_.status);
            }
        }
        return result_.failures == 0;
    }

    void report_timeout_diagnostics(std::string_view reason) {
        if (reported_ || !scheduler_) return;
        const auto graph = scheduler_->wait_graph();
        std::fprintf(stderr, "cpptb: %.*s%s\n",
                     static_cast<int>(reason.size()), reason.data(),
                     graph.deadlocked() ? "; scheduler deadlock detected" : "");
        const auto formatted = format_wait_graph(graph);
        std::fprintf(stderr, "%s\n", formatted.c_str());

        if constexpr (std::is_base_of_v<TestResult, Result>) {
            result_.wait_graph = graph;
            result_.status_reason.assign(reason);
            if (graph.deadlocked()) {
                result_.status_reason.append("; scheduler deadlock detected");
            }
            result_.failure_records.push_back(FailureRecord{
                .kind = FailureKind::Timeout,
                .label = result_.status_reason,
                .simulation_time_fs = scheduler_->now().femtoseconds,
            });
            ++result_.failures;
            result_.status = TestStatus::TimedOut;
            result_.simulation_time_fs = scheduler_->now().femtoseconds;
            result_.finished = true;
            if (const char* path = std::getenv("CPPTB_RESULT_FILE");
                path && path[0] != '\0' &&
                !write_test_result_json(path, result_)) {
                std::fprintf(stderr,
                             "cpptb: cannot write test result JSON to %s\n",
                             path);
                ++result_.failures;
                result_.status = TestStatus::Error;
            }
        }
        reported_ = true;
    }

    int report_if_done() {
        if (!scheduler_->done()) return 0;
        if (reported_) return result_succeeded() ? 1 : -1;

        const auto elapsed = std::chrono::steady_clock::now() - start_;
        const auto wall_us =
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed)
                .count();
        if constexpr (std::is_base_of_v<TestResult, Result>) {
            result_.simulation_time_fs = scheduler_->now().femtoseconds;
            result_.wall_time_ns = static_cast<uint64_t>(wall_us) * 1'000u;
            if (result_.status == TestStatus::Running) {
                result_.status = result_.failures == 0 ? TestStatus::Passed
                                                       : TestStatus::Failed;
                result_.finished = true;
            }
            if (const char* path = std::getenv("CPPTB_RESULT_FILE");
                path && path[0] != '\0' &&
                !write_test_result_json(path, result_)) {
                std::fprintf(stderr,
                             "cpptb: cannot write test result JSON to %s\n",
                             path);
                ++result_.failures;
                result_.status = TestStatus::Error;
                result_.finished = true;
            }
        }
        std::printf(
            "%s iterations=%u checks=%llu sim_cycles=%llu wall_ms=%.3f "
            "failures=%u\n",
            Adapter::result_name, iterations_,
            static_cast<unsigned long long>(result_.checks),
            static_cast<unsigned long long>(sim_cycles_),
            static_cast<double>(wall_us) / 1000.0, result_.failures);
#ifdef CPPTB_DPI_PROFILE
        constexpr size_t input_words = [] {
            if constexpr (requires {
                              Adapter::compact_input_transport;
                              Adapter::observed_signal_word_ids;
                          } && Adapter::compact_input_transport) {
                return Adapter::observed_signal_word_ids.size();
            } else {
                return static_cast<size_t>(Adapter::signal_count);
            }
        }();
        constexpr size_t output_words = [] {
            if constexpr (requires { Adapter::driven_signal_word_ids; }) {
                return Adapter::driven_signal_word_ids.size();
            } else {
                return static_cast<size_t>(Adapter::signal_count);
            }
        }();
        std::printf(
            "CPPTB_DPI_PROFILE steps=%llu init=%llu clock_edges=%llu "
            "signal_edges=%llu rising_edges=%llu falling_edges=%llu "
            "edges_without_interest=%llu delays=%llu next_tick_timers=%llu "
            "timer_idle=%llu outputs_changed=%llu "
            "output_transfers=%llu read_write=%llu read_only=%llu "
            "next_time_step=%llu phase_cb_rw=%llu phase_cb_ro=%llu "
            "phase_cb_nts=%llu input_words=%zu output_words=%zu\n",
            static_cast<unsigned long long>(profile_step_count_),
            static_cast<unsigned long long>(profile_init_step_count_),
            static_cast<unsigned long long>(profile_clock_edge_step_count_),
            static_cast<unsigned long long>(profile_signal_edge_step_count_),
            static_cast<unsigned long long>(profile_rising_edge_step_count_),
            static_cast<unsigned long long>(profile_falling_edge_step_count_),
            static_cast<unsigned long long>(profile_edge_without_interest_count_),
            static_cast<unsigned long long>(profile_delay_step_count_),
            static_cast<unsigned long long>(profile_next_tick_timer_count_),
            static_cast<unsigned long long>(profile_timer_idle_count_),
            static_cast<unsigned long long>(profile_outputs_changed_count_),
            static_cast<unsigned long long>(profile_output_transfer_count_),
            static_cast<unsigned long long>(profile_read_write_step_count_),
            static_cast<unsigned long long>(profile_read_only_step_count_),
            static_cast<unsigned long long>(profile_next_time_step_count_),
            static_cast<unsigned long long>(
                profile_phase_callback_registration_count_[0]),
            static_cast<unsigned long long>(
                profile_phase_callback_registration_count_[1]),
            static_cast<unsigned long long>(
                profile_phase_callback_registration_count_[2]),
            input_words, output_words);
#endif
#ifdef CPPTB_CORO_FRAME_POOL_DIAGNOSTICS
        const auto& frame_pool =
            coro::detail::coroutine_frame_pool().stats();
        std::printf(
            "CPPTB_CORO_FRAME_POOL system_allocations=%llu "
            "reused_allocations=%llu cached_deallocations=%llu "
            "system_deallocations=%llu\n",
            static_cast<unsigned long long>(frame_pool.system_allocations),
            static_cast<unsigned long long>(frame_pool.reused_allocations),
            static_cast<unsigned long long>(frame_pool.cached_deallocations),
            static_cast<unsigned long long>(frame_pool.system_deallocations));
#endif
#ifdef CPPTB_CORO_WAIT_PATH_DIAGNOSTICS
        std::printf(
            "CPPTB_CORO_WAIT_PATH edge_notifications=%llu "
            "edge_notifications_resumed=%llu rising_parks=%llu "
            "falling_parks=%llu any_parks=%llu\n",
            static_cast<unsigned long long>(
                scheduler_->edge_notification_count()),
            static_cast<unsigned long long>(
                scheduler_->edge_notification_resume_count()),
            static_cast<unsigned long long>(
                scheduler_->single_edge_park_count(coro::EdgeKind::Rising)),
            static_cast<unsigned long long>(
                scheduler_->single_edge_park_count(coro::EdgeKind::Falling)),
            static_cast<unsigned long long>(
                scheduler_->single_edge_park_count(coro::EdgeKind::Any)));
#endif
        reported_ = true;
        return result_succeeded() ? 1 : -1;
    }

    std::array<uint32_t, Adapter::signal_count> inputs_{};
    std::array<uint32_t, Adapter::signal_count> outputs_{};
    std::array<bool, Adapter::signal_count> driven_{};
    std::array<bool, Adapter::signal_count> configured_clock_{};
    std::array<bool, Adapter::signal_count> edge_observer_{};
    std::array<bool, Adapter::signal_count> local_edge_capable_{};
    std::array<bool, Adapter::signal_count> testbench_clock_{};
    std::array<const char*, Adapter::signal_count> signal_names_{};
    std::array<uint32_t, Adapter::signal_count> observed_transport_offsets_{};
    std::array<OnDemandGetWordsFn, Adapter::signal_count>
        on_demand_get_words_{};
    std::array<OnDemandSetWordsFn, Adapter::signal_count>
        on_demand_set_words_{};
    std::array<uint32_t, Adapter::signal_count> on_demand_base_ids_{};
    [[no_unique_address]] StaticBindingStorage static_binding_{};
    cpptb::detail::SimLogEndpoint sim_logs_;
    const uint32_t* current_inputs_ = nullptr;
    svScope dpi_scope_ = nullptr;
    std::unique_ptr<coro::Testbench> scheduler_;
    Dut dut_{};
    Result result_{};
    std::chrono::steady_clock::time_point start_;
    uint32_t iterations_ = 0;
    uint64_t sim_cycles_ = 0;
    bool outputs_dirty_ = false;
    bool access_violation_ = false;
    bool reported_ = false;
    bool local_edge_delivery_enabled_ = false;
    std::array<RegisteredClock, Adapter::signal_count> registered_clocks_{};
    uint32_t registered_clock_count_ = 0;
    uint64_t timeprecision_fs_ = 1;
    bool clock_registration_open_ = false;
#if (((defined(VM_VPI) && VM_VPI) || defined(CPPTB_ENABLE_VPI_TIMING)) && \
     !defined(CPPTB_VERILATOR_DIRECT_TIMING))
    std::array<PhaseCallbackData, 3> phase_callbacks_{};
#endif
#ifdef CPPTB_DPI_PROFILE
    uint64_t profile_step_count_ = 0;
    uint64_t profile_init_step_count_ = 0;
    uint64_t profile_clock_edge_step_count_ = 0;
    uint64_t profile_signal_edge_step_count_ = 0;
    uint64_t profile_rising_edge_step_count_ = 0;
    uint64_t profile_falling_edge_step_count_ = 0;
    uint64_t profile_edge_without_interest_count_ = 0;
    uint64_t profile_delay_step_count_ = 0;
    uint64_t profile_read_write_step_count_ = 0;
    uint64_t profile_read_only_step_count_ = 0;
    uint64_t profile_next_time_step_count_ = 0;
    uint64_t profile_next_tick_timer_count_ = 0;
    uint64_t profile_timer_idle_count_ = 0;
    uint64_t profile_outputs_changed_count_ = 0;
    mutable uint64_t profile_output_transfer_count_ = 0;
    std::array<uint64_t, 3> profile_phase_callback_registration_count_{};
#endif
};

}  // namespace cpptb::dpi

#ifdef CPPTB_VERILATOR_DIRECT_TIMING
#define CPPTB_DEFINE_VERILATOR_DIRECT_TIMING_API()                       \
    extern "C" unsigned int cpptb_verilator_pending_phases() {         \
        return g_cpptb_dpi_runtime.pending_phase_mask();                  \
    }                                                                     \
    extern "C" void cpptb_verilator_dispatch_phase(                     \
        unsigned int phase) {                                             \
        g_cpptb_dpi_runtime.dispatch_phase(                               \
            static_cast<cpptb::dpi::Phase>(phase));                       \
    }
#else
#define CPPTB_DEFINE_VERILATOR_DIRECT_TIMING_API()
#endif

#define CPPTB_DEFINE_NAMED_DPI_RUNTIME_BASE(                              \
    AdapterType, InitFunction, StepFunction, PullOutputsFunction,          \
    NextDeadlineFunction, EdgeInterestFunction)                            \
    namespace {                                                           \
    cpptb::dpi::Runtime<AdapterType> g_cpptb_dpi_runtime;                 \
    }                                                                     \
    extern "C" void InitFunction(unsigned int iterations,                \
                                  unsigned long long timeprecision_fs) {  \
        g_cpptb_dpi_runtime.init(iterations, timeprecision_fs);           \
    }                                                                     \
    extern "C" int StepFunction(                                        \
        unsigned int phase, unsigned long long sim_time,                  \
        unsigned long long sim_cycles, unsigned int event_signal_id,      \
        unsigned int event_edge, const svOpenArrayHandle in_words) {      \
        return g_cpptb_dpi_runtime.step(phase, sim_time, sim_cycles,       \
                                        event_signal_id, event_edge,       \
                                        in_words);                         \
    }                                                                     \
    extern "C" void PullOutputsFunction(                                \
        const svOpenArrayHandle out_words) {                              \
        g_cpptb_dpi_runtime.pull_outputs(out_words);                      \
    }                                                                     \
    extern "C" unsigned long long NextDeadlineFunction() {                \
        return g_cpptb_dpi_runtime.next_timer_deadline();                 \
    }                                                                     \
    extern "C" unsigned int EdgeInterestFunction(unsigned int signal_id) { \
        return g_cpptb_dpi_runtime.edge_interest(signal_id);              \
    }                                                                     \
    CPPTB_DEFINE_VERILATOR_DIRECT_TIMING_API()

#define CPPTB_DEFINE_NAMED_DPI_RUNTIME_WITH_STARVATION(                   \
    AdapterType, InitFunction, StepFunction, PullOutputsFunction,          \
    NextDeadlineFunction, EdgeInterestFunction, StarvationFunction)        \
    CPPTB_DEFINE_NAMED_DPI_RUNTIME_BASE(                                  \
        AdapterType, InitFunction, StepFunction, PullOutputsFunction,      \
        NextDeadlineFunction, EdgeInterestFunction)                        \
    extern "C" int StarvationFunction() {                               \
        return g_cpptb_dpi_runtime.report_starvation();                   \
    }

#define CPPTB_DEFINE_NAMED_DPI_RUNTIME(                                   \
    AdapterType, InitFunction, StepFunction, PullOutputsFunction,          \
    NextDeadlineFunction, EdgeInterestFunction)                            \
    CPPTB_DEFINE_NAMED_DPI_RUNTIME_BASE(                                   \
        AdapterType, InitFunction, StepFunction, PullOutputsFunction,      \
        NextDeadlineFunction, EdgeInterestFunction)

#define CPPTB_DEFINE_DPI_RUNTIME(AdapterType)                              \
    CPPTB_DEFINE_NAMED_DPI_RUNTIME_WITH_STARVATION(                        \
        AdapterType, cpptb_dpi_init, cpptb_dpi_step,                      \
        cpptb_dpi_pull_outputs, cpptb_dpi_next_timer_deadline,            \
        cpptb_dpi_edge_interest, cpptb_dpi_report_starvation)

#define CPPTB_DEFINE_NAMED_DPI_CLOCK_API(ClockConfigFunction)             \
    extern "C" unsigned long long ClockConfigFunction(                  \
        unsigned int signal_id, unsigned int field) {                     \
        return g_cpptb_dpi_runtime.clock_config(signal_id, field);         \
    }
