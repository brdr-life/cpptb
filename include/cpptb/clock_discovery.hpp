#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string_view>
#include <utility>
#include <vector>

#include "cpptb/dpi_static_binding.hpp"
#include "cpptb/test_api.hpp"

namespace cpptb::dpi {

namespace detail {

template <size_t SignalCount>
class ClockDiscoveryBinding {
   public:
    ClockDiscoveryBinding() {
        context_.inputs = inputs_.data();
        context_.outputs = outputs_.data();
        context_.current_inputs = nullptr;
        context_.configured_clock = configured_clock_.data();
        context_.edge_observer = edge_observer_.data();
        context_.local_edge_capable = one_bit_.data();
        context_.outputs_dirty = &outputs_dirty_;
        context_.local_edge_delivery_enabled = &local_edge_delivery_enabled_;
        context_.dynamic_context = this;
        context_.dynamic_get = dynamic_get;
        context_.dynamic_set = dynamic_set;
    }

    template <size_t Width, bool Writable, bool Driven, uint32_t Id,
              uint32_t TransportOffset>
    auto make_signal(
        StaticPackedSignalSpec<Width, Writable, Driven, Id, TransportOffset>,
        const char* name) {
        register_signal<Width, Writable, Id>(name);
        return StaticPackedSignal<Width, Writable, Driven, Id,
                                  TransportOffset>{&context_, name};
    }

    template <size_t Width, bool Writable, bool Driven, uint32_t Id,
              OnDemandGetWordsFn GetWords, OnDemandSetWordsFn SetWords>
    auto make_signal(
        StaticOnDemandSignalSpec<Width, Writable, Driven, Id, GetWords,
                                 SetWords>,
        const char* name) {
        register_signal<Width, Writable, Id>(name);
        return StaticOnDemandSignal<Width, Writable, Driven, Id>{
            &context_, name, zero_words, discard_words};
    }

    template <size_t Width, bool Writable, bool Driven, uint32_t Id,
              uint32_t TransportOffset, typename... Dimensions>
    auto make_signal(
        StaticPackedArraySpec<Width, Writable, Driven, Id, TransportOffset,
                              Dimensions...>,
        const char* name) {
        constexpr uint32_t element_count =
            static_cast<uint32_t>((Dimensions::size * ...));
        register_array<Width, Writable, Id>(name, element_count);
        return StaticPackedFixedArray<Width, Writable, Driven, Id,
                                      TransportOffset, 0, Dimensions...>{
            &context_, name, 0, 0};
    }

    template <size_t Width, bool Writable, bool Driven, uint32_t Id,
              OnDemandGetWordsFn GetWords, OnDemandSetWordsFn SetWords,
              typename... Dimensions>
    auto make_signal(
        StaticOnDemandArraySpec<Width, Writable, Driven, Id, GetWords,
                                SetWords, Dimensions...>,
        const char* name) {
        constexpr uint32_t element_count =
            static_cast<uint32_t>((Dimensions::size * ...));
        register_array<Width, Writable, Id>(name, element_count);
        return StaticOnDemandFixedArray<Width, Writable, Driven, Id, 0,
                                        Dimensions...>{
            &context_, name, zero_words, discard_words, 0, 0};
    }

    bool is_writable_one_bit(uint32_t id) const {
        return id < SignalCount && writable_[id] && one_bit_[id];
    }

    const char* signal_name(uint32_t id) const {
        return id < SignalCount ? names_[id] : "<invalid>";
    }

    uint32_t signal_value(uint32_t id) const {
        return id < SignalCount ? outputs_[id] : 0;
    }

   private:
    template <size_t Width, bool Writable, uint32_t Id>
    void register_signal(const char* name) {
        static_assert(Id < SignalCount);
        constexpr uint32_t word_count = (Width + 31) / 32;
        static_assert(word_count <= SignalCount - Id);
        for (uint32_t word = 0; word < word_count; ++word) {
            writable_[Id + word] = Writable;
            names_[Id + word] = name;
        }
        if constexpr (Width == 1) one_bit_[Id] = true;
    }

    template <size_t Width, bool Writable, uint32_t Id>
    void register_array(const char* name, uint32_t element_count) {
        constexpr uint32_t element_words = (Width + 31) / 32;
        const uint32_t word_count = element_count * element_words;
        if (Id > SignalCount || word_count > SignalCount - Id) {
            std::fprintf(stderr,
                         "cpptb: invalid generated array metadata for '%s'\n",
                         name ? name : "<unnamed>");
            std::abort();
        }
        for (uint32_t word = 0; word < word_count; ++word) {
            writable_[Id + word] = Writable;
            names_[Id + word] = name;
        }
        if constexpr (Width == 1) {
            for (uint32_t element = 0; element < element_count; ++element) {
                one_bit_[Id + element] = true;
            }
        }
    }

    static uint32_t dynamic_get(void* opaque, uint32_t id) {
        auto& self = *static_cast<ClockDiscoveryBinding*>(opaque);
        if (id >= SignalCount) return 0;
        return self.writable_[id] ? self.outputs_[id] : self.inputs_[id];
    }

    static void dynamic_set(void* opaque, uint32_t id, uint32_t value) {
        auto& self = *static_cast<ClockDiscoveryBinding*>(opaque);
        if (id >= SignalCount || !self.writable_[id]) {
            std::fprintf(stderr,
                         "cpptb: clock discovery attempted to write signal %u\n",
                         id);
            std::abort();
        }
        self.outputs_[id] = value;
    }

    static void zero_words(uint32_t, uint32_t* words, uint32_t word_count) {
        for (uint32_t word = 0; word < word_count; ++word) words[word] = 0;
    }

    static void discard_words(uint32_t, const uint32_t*, uint32_t) {}

    std::array<uint32_t, SignalCount> inputs_{};
    std::array<uint32_t, SignalCount> outputs_{};
    std::array<bool, SignalCount> configured_clock_{};
    std::array<bool, SignalCount> edge_observer_{};
    std::array<bool, SignalCount> one_bit_{};
    std::array<bool, SignalCount> writable_{};
    std::array<const char*, SignalCount> names_{};
    bool outputs_dirty_ = false;
    bool local_edge_delivery_enabled_ = false;
    StaticBindingContext context_{};
};

struct DiscoveredClock {
    uint32_t signal_id = 0;
    const char* name = "";
    uint64_t period_fs = 0;
    uint64_t phase_fs = 0;
    bool primary = false;
};

template <size_t SignalCount>
class ClockDiscoveryCollector {
   public:
    ClockDiscoveryCollector(const ClockDiscoveryBinding<SignalCount>& binding,
                            uint64_t timeprecision_fs)
        : binding_(binding), timeprecision_fs_(timeprecision_fs) {}

    coro::ClockRegistrar registrar() {
        return coro::ClockRegistrar{this, start_callback};
    }

    bool write_json(const char* path) const {
        std::ofstream output(path);
        if (!output) {
            std::fprintf(stderr,
                         "cpptb: cannot write discovered clock file '%s'\n",
                         path ? path : "<null>");
            return false;
        }
        output << "{\n  \"schema_version\": 1,\n  \"clocks\": [";
        for (size_t index = 0; index < clocks_.size(); ++index) {
            const auto& clock = clocks_[index];
            output << (index == 0 ? "\n" : ",\n")
                   << "    {\"port\": \"" << clock.name
                   << "\", \"period_fs\": " << clock.period_fs
                   << ", \"phase_fs\": " << clock.phase_fs
                   << ", \"initial_value\": "
                   << (binding_.signal_value(clock.signal_id) & 1u)
                   << ", \"primary\": "
                   << (clock.primary ? "true" : "false") << "}";
        }
        output << (clocks_.empty() ? "" : "\n  ") << "]\n}\n";
        return static_cast<bool>(output);
    }

   private:
    static void start_callback(void* opaque, coro::Signal signal,
                               coro::SimTime period, coro::SimTime phase) {
        static_cast<ClockDiscoveryCollector*>(opaque)->start(signal, period,
                                                              phase);
    }

    [[noreturn]] void fail(const char* format, const char* name) const {
        std::fprintf(stderr, format, name ? name : "<unnamed>");
        std::fputc('\n', stderr);
        std::abort();
    }

    void start(coro::Signal signal, coro::SimTime period,
               coro::SimTime phase) {
        const char* name = signal.name ? signal.name : binding_.signal_name(signal.id);
        if (!binding_.is_writable_one_bit(signal.id)) {
            fail("cpptb: clock '%s' must be a writable one-bit DUT port", name);
        }
        if (period.femtoseconds == 0 || (period.femtoseconds % 2u) != 0) {
            fail("cpptb: clock '%s' period must be positive and even", name);
        }
        const uint64_t half_period_fs = period.femtoseconds / 2u;
        if (timeprecision_fs_ == 0 ||
            (half_period_fs % timeprecision_fs_) != 0 ||
            (phase.femtoseconds % timeprecision_fs_) != 0) {
            fail("cpptb: clock '%s' timing is not representable at the simulator precision",
                 name);
        }
        for (const auto& clock : clocks_) {
            if (clock.signal_id == signal.id) {
                fail("cpptb: clock '%s' was started more than once", name);
            }
        }
        clocks_.push_back(DiscoveredClock{signal.id, name,
                                          period.femtoseconds,
                                          phase.femtoseconds,
                                          clocks_.empty()});
    }

    const ClockDiscoveryBinding<SignalCount>& binding_;
    uint64_t timeprecision_fs_ = 0;
    std::vector<DiscoveredClock> clocks_;
};

}  // namespace detail

template <typename Dut, size_t SignalCount, typename Result, typename BindDut,
          typename RegisterTestbench>
int discover_clocks(const char* output_path, uint64_t timeprecision_fs,
                    BindDut&& bind_dut,
                    RegisterTestbench&& register_testbench) {
    detail::ClockDiscoveryBinding<SignalCount> binding;
    Dut dut = std::forward<BindDut>(bind_dut)([&binding](auto spec,
                                                        const char* name) {
        return binding.make_signal(spec, name);
    });
    detail::ClockDiscoveryCollector<SignalCount> collector(binding,
                                                            timeprecision_fs);
    coro::Testbench scheduler{coro::SimTime{1}};
    Result result{};
    if (!std::forward<RegisterTestbench>(register_testbench)(
            scheduler, dut, result, collector.registrar())) {
        return 1;
    }
    return collector.write_json(output_path) ? 0 : 1;
}

template <typename Dut, size_t SignalCount, typename BindDut>
int discover_registered_clocks(const char* output_path,
                               uint64_t timeprecision_fs,
                               BindDut&& bind_dut) {
    return discover_clocks<Dut, SignalCount, TestResult>(
        output_path, timeprecision_fs, std::forward<BindDut>(bind_dut),
        [](coro::Testbench& scheduler, Dut dut, TestResult& result,
           coro::ClockRegistrar clocks) {
            return run_registered_test(scheduler, dut, result, clocks);
        });
}

}  // namespace cpptb::dpi
