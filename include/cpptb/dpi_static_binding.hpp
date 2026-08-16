// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "cpptb/access_discovery.hpp"
#include "cpptb/coro_runtime.hpp"
#include "cpptb/probe.hpp"

namespace cpptb::dpi {

using OnDemandGetWordsFn = void (*)(uint32_t, uint32_t*, uint32_t);
using OnDemandSetWordsFn = void (*)(uint32_t, const uint32_t*, uint32_t);

struct NoStaticBindingContext {};

struct StaticBindingContext {
    uint32_t* inputs = nullptr;
    uint32_t* outputs = nullptr;
    const uint32_t* current_inputs = nullptr;
    const uint32_t* observed_transport_offsets = nullptr;
    bool* configured_clock = nullptr;
    bool* edge_observer = nullptr;
    bool* local_edge_capable = nullptr;
    bool* outputs_dirty = nullptr;
    bool* local_edge_delivery_enabled = nullptr;
    coro::Testbench* scheduler = nullptr;
    void* dynamic_context = nullptr;
    coro::Signal::GetFn dynamic_get = nullptr;
    coro::Signal::SetFn dynamic_set = nullptr;

    void deliver_local_edge(uint32_t id, uint32_t previous,
                            uint32_t value) const {
        if (!*local_edge_delivery_enabled || !scheduler ||
            !local_edge_capable[id] || configured_clock[id] ||
            (edge_observer && edge_observer[id])) {
            return;
        }
        coro::EdgeKind edge = coro::EdgeKind::Any;
        if (previous == 0 && value != 0) {
            edge = coro::EdgeKind::Rising;
        } else if (previous != 0 && value == 0) {
            edge = coro::EdgeKind::Falling;
        }
        if (scheduler->has_edge_interest(id, edge)) {
            scheduler->notify_edge(id, edge);
        }
    }

    void set_packed_scalar(uint32_t id, uint32_t value) const {
        const uint32_t previous = outputs[id];
        if (previous == value) return;
        outputs[id] = value;
        *outputs_dirty = true;
        deliver_local_edge(id, previous, value);
    }

};

struct StaticPackedBindingSpan {
    uint32_t id = 0;
    uint32_t word_count = 0;
    uint32_t transport_offset = 0;
    bool driven = false;
};

struct RegisteredClockConfig {
    uint32_t signal_id = 0;
    uint64_t period_fs = 0;
    uint64_t phase_fs = 0;
    uint32_t initial_value = 0;
};

template <size_t SpanCount, size_t ObservedCount, size_t DrivenCount>
consteval bool validate_static_packed_binding_spans(
    const std::array<StaticPackedBindingSpan, SpanCount>& spans,
    const std::array<uint32_t, ObservedCount>& observed_word_ids,
    const std::array<uint32_t, DrivenCount>& driven_word_ids) {
    size_t observed_words = 0;
    size_t driven_words = 0;
    for (const auto span : spans) {
        const size_t table_size = span.driven ? DrivenCount : ObservedCount;
        if (span.transport_offset > table_size ||
            span.word_count > table_size - span.transport_offset) {
            return false;
        }
        for (uint32_t word = 0; word < span.word_count; ++word) {
            const auto table_id =
                span.driven
                    ? driven_word_ids[span.transport_offset + word]
                    : observed_word_ids[span.transport_offset + word];
            if (table_id != span.id + word) return false;
        }
        if (span.driven) {
            driven_words += span.word_count;
        } else {
            observed_words += span.word_count;
        }
    }
    return observed_words == ObservedCount && driven_words == DrivenCount;
}

template <size_t Width, bool Writable, bool Driven, uint32_t Id,
          uint32_t TransportOffset>
    requires(!Driven || Writable)
struct StaticPackedSignalSpec {
    static_assert(Width > 0, "signal width must be positive");
};

template <size_t Width, bool Writable, bool Driven, uint32_t Id,
          OnDemandGetWordsFn GetWords, OnDemandSetWordsFn SetWords>
    requires((!Driven || Writable) && GetWords != nullptr &&
             Writable == (SetWords != nullptr))
struct StaticOnDemandSignalSpec {
    static_assert(Width > 0, "signal width must be positive");
};

template <size_t Width, bool Writable, bool Driven, uint32_t Id,
          uint32_t TransportOffset, typename... Dimensions>
    requires(!Driven || Writable)
struct StaticPackedArraySpec {
    static_assert(Width > 0, "array element width must be positive");
    static_assert(sizeof...(Dimensions) > 0);
};

template <size_t Width, bool Writable, bool Driven, uint32_t Id,
          OnDemandGetWordsFn GetWords, OnDemandSetWordsFn SetWords,
          typename... Dimensions>
    requires((!Driven || Writable) && GetWords != nullptr &&
             Writable == (SetWords != nullptr))
struct StaticOnDemandArraySpec {
    static_assert(Width > 0, "array element width must be positive");
    static_assert(sizeof...(Dimensions) > 0);
};

namespace detail {

template <typename... Dimensions>
inline constexpr size_t static_array_element_count_v =
    (Dimensions::size * ... * 1);

template <size_t Width>
auto words_to_value(const uint32_t* words) {
    typename cpptb::Bits<Width>::word_array storage{};
    for (size_t word = 0; word < storage.size(); ++word) {
        storage[word] = words[word];
    }
    const auto bits = cpptb::Bits<Width>::from_words(storage);
    if constexpr (Width <= 64) {
        return bits.to_uint64();
    } else {
        return bits;
    }
}

template <size_t Width>
auto value_to_words(coro::PackedSignalValue<Width> value) {
    cpptb::Bits<Width> bits;
    if constexpr (Width <= 64) {
        bits = cpptb::Bits<Width>::from_uint(value);
    } else {
        bits = value;
    }
    return bits.words();
}

template <size_t Width>
constexpr uint32_t normalize_scalar(uint32_t value) {
    static_assert(Width > 0 && Width <= 32);
    if constexpr (Width < 32) {
        return value & ((uint32_t{1} << Width) - 1);
    }
    return value;
}

template <size_t Width>
uint32_t static_dynamic_get(void* opaque, uint32_t id) {
    auto* context = static_cast<StaticBindingContext*>(opaque);
    return normalize_scalar<Width>(
        context->dynamic_get(context->dynamic_context, id));
}

template <size_t Width>
void static_dynamic_set(void* opaque, uint32_t id, uint32_t value) {
    auto* context = static_cast<StaticBindingContext*>(opaque);
    context->dynamic_set(context->dynamic_context, id,
                         normalize_scalar<Width>(value));
}

template <size_t Width>
coro::Signal static_dynamic_signal(StaticBindingContext* context, uint32_t id,
                                   const char* name) {
    static_assert(Width > 0 && Width <= 32);
    return {nullptr, id, name, context, static_dynamic_get<Width>,
            static_dynamic_set<Width>};
}

}  // namespace detail

template <size_t Width, bool Writable, bool Driven>
    requires(!Driven || Writable)
class StaticPackedRef {
   public:
    using value_type = coro::PackedSignalValue<Width>;
    static constexpr size_t word_count = (Width + 31) / 32;

    StaticBindingContext* context = nullptr;
    uint32_t id = 0;
    uint32_t transport_offset = 0;
    const char* name = "";

    value_type get() const {
        typename cpptb::Bits<Width>::word_array words{};
        for (size_t word = 0; word < word_count; ++word) {
            const bool scheduler_owned =
                context->configured_clock && context->configured_clock[id];
            if constexpr (Driven) {
                if (scheduler_owned && context->current_inputs) {
                    const auto offset =
                        context->observed_transport_offsets[id + word];
                    words[word] = context->current_inputs[offset];
                } else {
                    words[word] = context->outputs[id + word];
                }
            } else if (context->current_inputs) {
                words[word] = context->current_inputs[transport_offset + word];
            } else {
                words[word] = context->inputs[id + word];
            }
        }
        if constexpr (Width <= 32) {
            return detail::normalize_scalar<Width>(words[0]);
        } else {
            return detail::words_to_value<Width>(words.data());
        }
    }

    // The immediate deposit -- cocotb's setimmediatevalue(). Under
    // deferred_writes this is the escape hatch; otherwise set() is this.
    void set_now(value_type value) const requires(Writable) {
        probe::detail::require_write_allowed(name, "set");
        if constexpr (Width <= 32) {
            context->set_packed_scalar(
                id, detail::normalize_scalar<Width>(value));
        } else {
            const auto words = detail::value_to_words<Width>(value);
            bool changed = false;
            for (size_t word = 0; word < word_count; ++word) {
                changed = changed || context->outputs[id + word] != words[word];
                context->outputs[id + word] = words[word];
            }
            *context->outputs_dirty = *context->outputs_dirty || changed;
        }
    }

    void set(value_type value) const requires(Writable) {
#ifdef CPPTB_DEFERRED_WRITES
        // Queued, applied at the start of the ReadWrite phase: a write after
        // `co_await RisingEdge{}` misses the edge just awaited and lands on
        // the next one, which is the cocotb write model. Legality is checked
        // here, at the call, so writing from ReadOnly still fails at the
        // offending line.
        probe::detail::require_write_allowed(name, "set");
        context->scheduler->defer_write(
            [self = *this, value] { self.set_now(value); });
#else
        set_now(value);
#endif
    }

    template <typename Value>
    void force(Value&&) const {
        coro::detail::unsupported_port_force<Value>();
    }

    template <typename Port = StaticPackedRef>
    void release() const {
        coro::detail::unsupported_port_release<Port>();
    }

    operator coro::Signal() const requires(Width <= 32) {
        return detail::static_dynamic_signal<Width>(context, id, name);
    }
};

template <size_t Width, bool Writable, bool Driven>
    requires(!Driven || Writable)
class StaticOnDemandRef {
   public:
    using value_type = coro::PackedSignalValue<Width>;
    static constexpr size_t word_count = (Width + 31) / 32;

    StaticBindingContext* context = nullptr;
    uint32_t id = 0;
    uint32_t word_offset = 0;
    const char* name = "";
    OnDemandGetWordsFn get_words_fn = nullptr;
    OnDemandSetWordsFn set_words_fn = nullptr;

    value_type get() const {
        typename cpptb::Bits<Width>::word_array words{};
        get_words_fn(word_offset, words.data(),
                     static_cast<uint32_t>(word_count));
        if constexpr (Width <= 32) {
            return detail::normalize_scalar<Width>(words[0]);
        } else {
            return detail::words_to_value<Width>(words.data());
        }
    }

    void set_now(value_type value) const requires(Writable) {
        probe::detail::require_write_allowed(name, "set");
        if constexpr (Width <= 32) {
            value = detail::normalize_scalar<Width>(value);
        }
        const auto words = detail::value_to_words<Width>(value);
        if constexpr (Width <= 32) {
            const uint32_t previous = get();
            if (previous == words[0]) return;
            set_words_fn(word_offset, words.data(), 1);
            context->deliver_local_edge(id, previous, words[0]);
        } else {
            set_words_fn(word_offset, words.data(),
                         static_cast<uint32_t>(word_count));
        }
    }

    void set(value_type value) const requires(Writable) {
#ifdef CPPTB_DEFERRED_WRITES
        probe::detail::require_write_allowed(name, "set");
        context->scheduler->defer_write(
            [self = *this, value] { self.set_now(value); });
#else
        set_now(value);
#endif
    }

    template <typename Value>
    void force(Value&&) const {
        coro::detail::unsupported_port_force<Value>();
    }

    template <typename Port = StaticOnDemandRef>
    void release() const {
        coro::detail::unsupported_port_release<Port>();
    }

    operator coro::Signal() const requires(Width <= 32) {
        return detail::static_dynamic_signal<Width>(context, id, name);
    }
};

template <size_t Width, bool Writable, bool Driven, uint32_t Id,
          uint32_t TransportOffset>
    requires(!Driven || Writable)
class StaticPackedSignal {
   public:
    using value_type = coro::PackedSignalValue<Width>;
    static constexpr size_t width = Width;
    static constexpr size_t word_count = (Width + 31) / 32;
    static constexpr bool writable = Writable;
    static constexpr bool driven = Driven;
    static constexpr uint32_t global_id = Id;
    static constexpr uint32_t transport_offset = TransportOffset;

    StaticBindingContext* context = nullptr;
    const char* name = "";

    value_type get() const {
        return StaticPackedRef<Width, Writable, Driven>{
            context, Id, TransportOffset, name}.get();
    }

    void set(value_type value) const requires(Writable) {
        StaticPackedRef<Width, Writable, Driven>{
            context, Id, TransportOffset, name}.set(value);
    }

    void set_now(value_type value) const requires(Writable) {
        StaticPackedRef<Width, Writable, Driven>{
            context, Id, TransportOffset, name}.set_now(value);
    }

    template <typename Value>
    void force(Value&&) const {
        coro::detail::unsupported_port_force<Value>();
    }

    template <typename Port = StaticPackedSignal>
    void release() const {
        coro::detail::unsupported_port_release<Port>();
    }

    operator coro::Signal() const requires(Width <= 32) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        discovery::mark_port_edge<Id>();
        return {nullptr, Id, name};
#else
        return detail::static_dynamic_signal<Width>(context, Id, name);
#endif
    }
};

template <size_t Width, bool Writable, bool Driven, uint32_t Id>
    requires(!Driven || Writable)
class StaticOnDemandSignal {
   public:
    using value_type = coro::PackedSignalValue<Width>;
    static constexpr size_t width = Width;
    static constexpr size_t word_count = (Width + 31) / 32;
    static constexpr bool writable = Writable;
    static constexpr bool driven = Driven;
    static constexpr uint32_t global_id = Id;

    StaticBindingContext* context = nullptr;
    const char* name = "";
    OnDemandGetWordsFn get_words_fn = nullptr;
    OnDemandSetWordsFn set_words_fn = nullptr;

    value_type get() const {
        return StaticOnDemandRef<Width, Writable, Driven>{
            context, Id, 0, name, get_words_fn, set_words_fn}.get();
    }

    void set(value_type value) const requires(Writable) {
        StaticOnDemandRef<Width, Writable, Driven>{
            context, Id, 0, name, get_words_fn, set_words_fn}.set(value);
    }

    void set_now(value_type value) const requires(Writable) {
        StaticOnDemandRef<Width, Writable, Driven>{
            context, Id, 0, name, get_words_fn, set_words_fn}.set_now(value);
    }

    template <typename Value>
    void force(Value&&) const {
        coro::detail::unsupported_port_force<Value>();
    }

    template <typename Port = StaticOnDemandSignal>
    void release() const {
        coro::detail::unsupported_port_release<Port>();
    }

    operator coro::Signal() const requires(Width <= 32) {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        discovery::mark_port_edge<Id>();
        return {nullptr, Id, name};
#else
        return detail::static_dynamic_signal<Width>(context, Id, name);
#endif
    }
};

template <size_t ElementWidth, bool Writable, bool Driven, uint32_t BaseId,
          uint32_t BaseTransportOffset, size_t DimensionIndex,
          typename Dimension, typename... RemainingDimensions>
    requires(!Driven || Writable)
class StaticPackedFixedArray {
   public:
    static constexpr size_t element_width = ElementWidth;
    static constexpr size_t element_word_count = (ElementWidth + 31) / 32;
    static constexpr size_t rank = 1 + sizeof...(RemainingDimensions);
    static constexpr size_t element_count =
        Dimension::size *
        detail::static_array_element_count_v<RemainingDimensions...>;
    static constexpr size_t word_count = element_count * element_word_count;
    static constexpr bool writable = Writable;
    static constexpr bool driven = Driven;
    static constexpr uint32_t base_id = BaseId;
    static constexpr uint32_t base_transport_offset = BaseTransportOffset;
    static constexpr int32_t left() { return Dimension::left; }
    static constexpr int32_t right() { return Dimension::right; }
    static constexpr int32_t low() { return Dimension::low; }
    static constexpr int32_t high() { return Dimension::high; }
    static constexpr size_t size() { return Dimension::size; }

    StaticBindingContext* context = nullptr;
    const char* name = "";
    uint32_t id_offset = 0;
    uint32_t transport_offset_delta = 0;

    [[nodiscard]] auto at(int32_t index) const {
        check_bounds(index);
        constexpr uint32_t stride_words = static_cast<uint32_t>(
            element_word_count *
            detail::static_array_element_count_v<RemainingDimensions...>);
        const uint32_t offset =
            static_cast<uint32_t>(index - low()) * stride_words;
        if constexpr (sizeof...(RemainingDimensions) != 0) {
            return StaticPackedFixedArray<
                ElementWidth, Writable, Driven, BaseId, BaseTransportOffset,
                DimensionIndex + 1, RemainingDimensions...>{
                context, name, id_offset + offset,
                transport_offset_delta + offset};
        } else {
            return StaticPackedRef<ElementWidth, Writable, Driven>{
                context, BaseId + id_offset + offset,
                BaseTransportOffset + transport_offset_delta + offset, name};
        }
    }

    [[nodiscard]] auto operator[](int32_t index) const {
        return at(index);
    }

   private:
    void check_bounds(int32_t index) const {
        if (index >= low() && index <= high()) return;
        std::fprintf(stderr,
                     "cpptb: unpacked array '%s' dimension %zu index %d "
                     "is out of bounds [%d:%d]\n",
                     name, DimensionIndex + 1, index, Dimension::left,
                     Dimension::right);
        std::abort();
    }
};

template <size_t ElementWidth, bool Writable, bool Driven, uint32_t BaseId,
          size_t DimensionIndex, typename Dimension,
          typename... RemainingDimensions>
    requires(!Driven || Writable)
class StaticOnDemandFixedArray {
   public:
    static constexpr size_t element_width = ElementWidth;
    static constexpr size_t element_word_count = (ElementWidth + 31) / 32;
    static constexpr size_t rank = 1 + sizeof...(RemainingDimensions);
    static constexpr size_t element_count =
        Dimension::size *
        detail::static_array_element_count_v<RemainingDimensions...>;
    static constexpr size_t word_count = element_count * element_word_count;
    static constexpr bool writable = Writable;
    static constexpr bool driven = Driven;
    static constexpr uint32_t base_id = BaseId;
    static constexpr int32_t left() { return Dimension::left; }
    static constexpr int32_t right() { return Dimension::right; }
    static constexpr int32_t low() { return Dimension::low; }
    static constexpr int32_t high() { return Dimension::high; }
    static constexpr size_t size() { return Dimension::size; }

    StaticBindingContext* context = nullptr;
    const char* name = "";
    OnDemandGetWordsFn get_words_fn = nullptr;
    OnDemandSetWordsFn set_words_fn = nullptr;
    uint32_t id_offset = 0;
    uint32_t word_offset_delta = 0;

    [[nodiscard]] auto at(int32_t index) const {
        check_bounds(index);
        constexpr uint32_t stride_words = static_cast<uint32_t>(
            element_word_count *
            detail::static_array_element_count_v<RemainingDimensions...>);
        const uint32_t offset =
            static_cast<uint32_t>(index - low()) * stride_words;
        if constexpr (sizeof...(RemainingDimensions) != 0) {
            return StaticOnDemandFixedArray<
                ElementWidth, Writable, Driven, BaseId, DimensionIndex + 1,
                RemainingDimensions...>{
                context, name, get_words_fn, set_words_fn,
                id_offset + offset, word_offset_delta + offset};
        } else {
            return StaticOnDemandRef<ElementWidth, Writable, Driven>{
                context, BaseId + id_offset + offset,
                word_offset_delta + offset, name, get_words_fn, set_words_fn};
        }
    }

    [[nodiscard]] auto operator[](int32_t index) const {
        return at(index);
    }

   private:
    void check_bounds(int32_t index) const {
        if (index >= low() && index <= high()) return;
        std::fprintf(stderr,
                     "cpptb: unpacked array '%s' dimension %zu index %d "
                     "is out of bounds [%d:%d]\n",
                     name, DimensionIndex + 1, index, Dimension::left,
                     Dimension::right);
        std::abort();
    }
};

}  // namespace cpptb::dpi
