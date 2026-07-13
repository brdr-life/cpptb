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
#include <utility>

#include "cpptb/coro_runtime.hpp"
#include "cpptb/probe.hpp"
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
    kStepEdgeInterestChanged = 64,
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
        probe::detail::DpiCallbackScope callback_scope;
        scheduler_.reset();
        inputs_.fill(0);
        outputs_.fill(0);
        driven_.fill(false);
        configured_clock_.fill(false);
        edge_observer_.fill(false);
        local_edge_capable_.fill(false);
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
#ifdef CPPTB_DPI_PROFILE
        profile_step_count_ = 0;
        profile_outputs_changed_count_ = 0;
        profile_output_transfer_count_ = 0;
#endif

        scheduler_ =
            std::make_unique<coro::Testbench>(coro::SimTime{timeprecision_fs});

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
                scheduler_->configure_static_edge_source(id);
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
        Adapter::register_testbench(*scheduler_, dut_, iterations_, result_);
    }

    int step(uint32_t phase_value, uint64_t sim_time, uint64_t sim_cycles,
             uint32_t event_signal_id, uint32_t event_edge,
             const svOpenArrayHandle in_words) {
        probe::detail::DpiCallbackScope callback_scope;
        if (!scheduler_) {
            std::fprintf(stderr, "%s: DPI step called before init\n",
                         Adapter::result_name);
            return -1;
        }

        const auto phase = static_cast<Phase>(phase_value);

#ifdef CPPTB_DPI_PROFILE
        ++profile_step_count_;
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
                scheduler_->notify_edge(
                    event_signal_id, static_cast<coro::EdgeKind>(event_edge));
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
        if (edge_interest_changed) {
            requests |= kStepEdgeInterestChanged;
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

    uint64_t edge_interest_generation() const {
        return scheduler_ ? scheduler_->edge_interest_generation() : 0;
    }

    std::optional<coro::EdgeInterestChange> consume_edge_interest_change() {
        return scheduler_ ? scheduler_->consume_edge_interest_change()
                          : std::nullopt;
    }

   private:
    static constexpr uint32_t kNoTransportOffset =
        std::numeric_limits<uint32_t>::max();
    using OnDemandGetWordsFn = void (*)(uint32_t, uint32_t*, uint32_t);
    using OnDemandSetWordsFn = void (*)(uint32_t, const uint32_t*, uint32_t);

    class InputViewScope {
       public:
        InputViewScope(Runtime& runtime, const uint32_t* inputs)
            : runtime_(runtime), previous_(runtime.current_inputs_) {
            runtime_.current_inputs_ = inputs;
        }

        ~InputViewScope() { runtime_.current_inputs_ = previous_; }

       private:
        Runtime& runtime_;
        const uint32_t* previous_;
    };

    void validate_transport_word(
        uint32_t id, bool expected_driven,
        std::array<bool, Adapter::signal_count>& transport_seen) const {
        if (id >= transport_seen.size() || transport_seen[id] ||
            driven_[id] != expected_driven) {
            std::fprintf(stderr, "%s: invalid DPI transport word %u\n",
                         Adapter::result_name, id);
            std::abort();
        }
        transport_seen[id] = true;
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
            for (size_t word = 0;
                 word < Adapter::observed_signal_word_ids.size(); ++word) {
                const auto id = Adapter::observed_signal_word_ids[word];
                validate_transport_word(id, false, transport_seen);
                observed_transport_offsets_[id] = static_cast<uint32_t>(word);
            }
            for (const auto id : Adapter::driven_signal_word_ids) {
                validate_transport_word(id, true, transport_seen);
            }
            for (uint32_t id = 0; id < transport_seen.size(); ++id) {
                if (on_demand_get_words_[id]) {
                    validate_transport_word(id, driven_[id], transport_seen);
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
            observer_changed = observer_changed || edge_observer_[id];
        }
        return observer_changed;
    }

    static uint32_t signal_get(void* context, uint32_t id) {
        return static_cast<Runtime*>(context)->get(id);
    }

    static void signal_set(void* context, uint32_t id, uint32_t value) {
        static_cast<Runtime*>(context)->set(id, value);
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
        if (driven_[id]) return outputs_[id];
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
            !local_edge_capable_[id] || configured_clock_[id]) {
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
        if (driven_[id]) {
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
            "CPPTB_DPI_PROFILE steps=%llu outputs_changed=%llu "
            "output_transfers=%llu input_words=%zu output_words=%zu\n",
            static_cast<unsigned long long>(profile_step_count_),
            static_cast<unsigned long long>(profile_outputs_changed_count_),
            static_cast<unsigned long long>(profile_output_transfer_count_),
            input_words, output_words);
#endif
        reported_ = true;
        return result_.failures == 0 ? 1 : -1;
    }

    std::array<uint32_t, Adapter::signal_count> inputs_{};
    std::array<uint32_t, Adapter::signal_count> outputs_{};
    std::array<bool, Adapter::signal_count> driven_{};
    std::array<bool, Adapter::signal_count> configured_clock_{};
    std::array<bool, Adapter::signal_count> edge_observer_{};
    std::array<bool, Adapter::signal_count> local_edge_capable_{};
    std::array<const char*, Adapter::signal_count> signal_names_{};
    std::array<uint32_t, Adapter::signal_count> observed_transport_offsets_{};
    std::array<OnDemandGetWordsFn, Adapter::signal_count>
        on_demand_get_words_{};
    std::array<OnDemandSetWordsFn, Adapter::signal_count>
        on_demand_set_words_{};
    std::array<uint32_t, Adapter::signal_count> on_demand_base_ids_{};
    const uint32_t* current_inputs_ = nullptr;
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
#ifdef CPPTB_DPI_PROFILE
    uint64_t profile_step_count_ = 0;
    uint64_t profile_outputs_changed_count_ = 0;
    mutable uint64_t profile_output_transfer_count_ = 0;
#endif
};

}  // namespace cpptb::dpi

#define CPPTB_DEFINE_NAMED_DPI_RUNTIME(                                   \
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
    }

#define CPPTB_DEFINE_DPI_RUNTIME(AdapterType)                              \
    CPPTB_DEFINE_NAMED_DPI_RUNTIME(AdapterType, cpptb_dpi_init,            \
                                   cpptb_dpi_step,                         \
                                   cpptb_dpi_pull_outputs,                 \
                                   cpptb_dpi_next_timer_deadline,          \
                                   cpptb_dpi_edge_interest)
