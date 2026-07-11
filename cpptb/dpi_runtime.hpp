#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>

#include "cpptb/coro_runtime.hpp"
#include "cpptb/test_result.hpp"
#include "svdpi.h"

namespace cpptb::dpi {

enum class Phase : uint32_t {
    Init = 0,
    Edge = 1,
    Delay = 4,
};

enum StepResult : uint32_t {
    kStepDone = 1,
    kStepTimerChanged = 8,
    kStepFallingEdges = 16,
    kStepOutputsChanged = 32,
};

template <typename Adapter>
class Runtime {
   public:
    using Dut = typename Adapter::Dut;
    using Result = typename Adapter::Result;

    Runtime() = default;
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    void init(uint32_t iterations, uint64_t timeprecision_fs) {
        scheduler_.reset();
        inputs_.fill(0);
        outputs_.fill(0);
        driven_.fill(false);
        signal_names_.fill("<unknown>");
        result_ = Result{};
        iterations_ = iterations;
        sim_cycles_ = 0;
        outputs_dirty_ = false;
        access_violation_ = false;
        reported_ = false;

        for (const auto id : Adapter::driven_signal_ids) {
            if (id >= driven_.size()) {
                std::fprintf(stderr, "%s: invalid driven signal id %u\n",
                             Adapter::result_name, id);
                std::abort();
            }
            driven_[id] = true;
        }

        dut_ = Adapter::bind_dut([this](uint32_t id, const char* name) {
            return make_signal(id, name);
        });
        scheduler_ =
            std::make_unique<coro::Testbench>(coro::SimTime{timeprecision_fs});
        start_ = std::chrono::steady_clock::now();
        Adapter::register_testbench(*scheduler_, dut_, iterations_, result_);
    }

    int step(uint32_t phase_value, uint64_t sim_time, uint64_t sim_cycles,
             uint32_t event_signal_id, uint32_t event_edge,
             const svOpenArrayHandle in_words,
             const svOpenArrayHandle out_words) {
        if (!scheduler_) {
            std::fprintf(stderr, "%s: DPI step called before init\n",
                         Adapter::result_name);
            return -1;
        }

        const auto phase = static_cast<Phase>(phase_value);

        copy_inputs(in_words);
        sim_cycles_ = sim_cycles;
        scheduler_->set_time(sim_time);

        switch (phase) {
            case Phase::Init:
            case Phase::Delay:
                break;
            case Phase::Edge:
                if (event_edge >
                    static_cast<uint32_t>(coro::EdgeKind::Falling)) {
                    std::fprintf(stderr, "%s: unknown edge %u\n",
                                 Adapter::result_name, event_edge);
                    return -1;
                }
                scheduler_->notify_edge(
                    event_signal_id, static_cast<coro::EdgeKind>(event_edge));
                break;
            default:
                std::fprintf(stderr, "%s: unknown phase %u\n",
                             Adapter::result_name, phase_value);
                return -1;
        }

        if (access_violation_) return -1;

        const bool outputs_changed = outputs_dirty_;
        copy_outputs(out_words);
        outputs_dirty_ = false;

        if (Adapter::timed_out(scheduler_->now(), sim_cycles_, iterations_)) {
            std::fprintf(stderr,
                         "%s: timed out at time %llu after %llu cycles\n",
                         Adapter::result_name,
                         static_cast<unsigned long long>(sim_time),
                         static_cast<unsigned long long>(sim_cycles_));
            return -1;
        }

        const int completion = report_if_done();
        if (completion < 0) return -1;

        uint32_t requests = completion > 0 ? kStepDone : 0;
        if (scheduler_->consume_timer_schedule_changed()) {
            requests |= kStepTimerChanged;
        }
        if (scheduler_->has_falling_edge_waiters()) {
            requests |= kStepFallingEdges;
        }
        if (outputs_changed) {
            requests |= kStepOutputsChanged;
        }
        return static_cast<int>(requests);
    }

    uint64_t next_timer_deadline() {
        if (!scheduler_) return std::numeric_limits<uint64_t>::max();
        return scheduler_->next_timer_deadline();
    }

   private:
    static uint32_t signal_get(void* context, uint32_t id) {
        return static_cast<Runtime*>(context)->get(id);
    }

    static void signal_set(void* context, uint32_t id, uint32_t value) {
        static_cast<Runtime*>(context)->set(id, value);
    }

    coro::Signal make_signal(uint32_t id, const char* name) {
        if (id >= inputs_.size()) {
            std::fprintf(stderr, "%s: invalid signal id %u\n",
                         Adapter::result_name, id);
            std::abort();
        }
        signal_names_[id] = name ? name : "<unnamed>";
        return coro::Signal{nullptr, id, name, this, signal_get, signal_set};
    }

    uint32_t get(uint32_t id) const {
        if (id >= inputs_.size()) {
            std::fprintf(stderr, "%s: invalid get id %u\n",
                         Adapter::result_name, id);
            std::abort();
        }
        return driven_[id] ? outputs_[id] : inputs_[id];
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
        if (outputs_[id] == value) return;
        outputs_[id] = value;
        outputs_dirty_ = true;
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

    void copy_inputs(const svOpenArrayHandle words) {
        const auto* data = array_ptr(words, "input");
        for (uint32_t index = 0; index < inputs_.size(); ++index) {
            inputs_[index] = data[index];
        }
    }

    void copy_outputs(const svOpenArrayHandle words) const {
        auto* data = array_ptr(words, "output");
        for (uint32_t index = 0; index < outputs_.size(); ++index) {
            data[index] = outputs_[index];
        }
    }

    int report_if_done() {
        if (!scheduler_->done()) return 0;
        if (reported_) return result_.failures == 0 ? 1 : -1;

        const auto elapsed = std::chrono::steady_clock::now() - start_;
        const auto wall_us =
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed)
                .count();
        std::printf(
            "%s iterations=%u checks=%llu sim_cycles=%llu wall_ms=%.3f "
            "failures=%u\n",
            Adapter::result_name, iterations_,
            static_cast<unsigned long long>(result_.checks),
            static_cast<unsigned long long>(sim_cycles_),
            static_cast<double>(wall_us) / 1000.0, result_.failures);
        reported_ = true;
        return result_.failures == 0 ? 1 : -1;
    }

    std::array<uint32_t, Adapter::signal_count> inputs_{};
    std::array<uint32_t, Adapter::signal_count> outputs_{};
    std::array<bool, Adapter::signal_count> driven_{};
    std::array<const char*, Adapter::signal_count> signal_names_{};
    std::unique_ptr<coro::Testbench> scheduler_;
    Dut dut_{};
    Result result_{};
    std::chrono::steady_clock::time_point start_;
    uint32_t iterations_ = 0;
    uint64_t sim_cycles_ = 0;
    bool outputs_dirty_ = false;
    bool access_violation_ = false;
    bool reported_ = false;
};

}  // namespace cpptb::dpi

#define CPPTB_DEFINE_NAMED_DPI_RUNTIME(                                   \
    AdapterType, InitFunction, StepFunction, NextDeadlineFunction)         \
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
        unsigned int event_edge, const svOpenArrayHandle in_words,        \
        const svOpenArrayHandle out_words) {                              \
        return g_cpptb_dpi_runtime.step(phase, sim_time, sim_cycles,       \
                                        event_signal_id, event_edge,       \
                                        in_words, out_words);              \
    }                                                                     \
    extern "C" unsigned long long NextDeadlineFunction() {                \
        return g_cpptb_dpi_runtime.next_timer_deadline();                 \
    }

#define CPPTB_DEFINE_DPI_RUNTIME(AdapterType)                              \
    CPPTB_DEFINE_NAMED_DPI_RUNTIME(AdapterType, cpptb_dpi_init,            \
                                   cpptb_dpi_step,                         \
                                   cpptb_dpi_next_timer_deadline)
