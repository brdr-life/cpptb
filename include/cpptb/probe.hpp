#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

#include "cpptb/packed_bits.hpp"

namespace cpptb::probe {

namespace detail {

enum class CallbackPhase : uint8_t {
    Evaluation,
    ReadWrite,
    ReadOnly,
    NextTimeStep,
};

struct DpiCallbackState {
    uint32_t depth = 0;
    uint64_t epoch = 0;
    CallbackPhase phase = CallbackPhase::Evaluation;
};

inline thread_local DpiCallbackState dpi_callback_state;

class DpiCallbackScope {
   public:
    explicit DpiCallbackScope(
        CallbackPhase phase = CallbackPhase::Evaluation)
        : previous_phase_(dpi_callback_state.phase) {
        auto& state = dpi_callback_state;
        state.phase = phase;
        if (state.depth++ == 0) ++state.epoch;
    }
    DpiCallbackScope(const DpiCallbackScope&) = delete;
    DpiCallbackScope& operator=(const DpiCallbackScope&) = delete;
    ~DpiCallbackScope() {
        auto& state = dpi_callback_state;
        --state.depth;
        state.phase = previous_phase_;
    }

   private:
    CallbackPhase previous_phase_;
};

inline uint64_t current_callback_epoch() { return dpi_callback_state.epoch; }

inline CallbackPhase current_callback_phase() {
    return dpi_callback_state.phase;
}

[[noreturn]] inline void fail_write_not_allowed(const char* name,
                                                const char* operation) {
    std::fprintf(
        stderr,
        "cpptb: %s() is not allowed during ReadOnly for signal '%s'; "
        "await NextTimeStep{}, Delay{...}, or an edge before driving it\n",
        operation ? operation : "write", name ? name : "<unnamed>");
    std::abort();
}

[[noreturn]] inline void fail_outside_callback(const char* name) {
    std::fprintf(stderr,
                 "cpptb: internal probe '%s' used outside a DPI callback\n",
                 name ? name : "<unnamed>");
    std::abort();
}

inline void require_write_allowed(const char* name, const char* operation) {
    const auto& state = dpi_callback_state;
    if (state.depth == 0 || state.phase != CallbackPhase::ReadOnly) {
        return;
    }
    fail_write_not_allowed(name, operation);
}

inline void require_callback(const char* name) {
    if (dpi_callback_state.depth != 0) return;
    fail_outside_callback(name);
}

inline void require_write_callback(const char* name, const char* operation) {
    const auto& state = dpi_callback_state;
    if (state.depth == 0) fail_outside_callback(name);
    if (state.phase != CallbackPhase::ReadOnly) return;
    fail_write_not_allowed(name, operation);
}

inline void require_signal_callback(const char* name) {
    if (dpi_callback_state.depth != 0) return;
    std::fprintf(stderr,
                 "cpptb: on-demand signal '%s' used outside a DPI callback\n",
                 name ? name : "<unnamed>");
    std::abort();
}

}  // namespace detail

template <size_t Width>
using Value = std::conditional_t<
    (Width <= 32), uint32_t,
    std::conditional_t<(Width <= 64), uint64_t, cpptb::Bits<Width>>>;

template <size_t Width, bool Writable, bool Forceable = false>
class Probe {
   public:
    static_assert(Width > 0, "probe width must be positive");

    using value_type = Value<Width>;
    using GetFn = value_type (*)(int32_t index);
    using DepositFn = void (*)(int32_t index, value_type value);
    using ForceFn = void (*)(int32_t index, value_type value);
    using ReleaseFn = void (*)(int32_t index);

    static constexpr size_t width = Width;
    static constexpr size_t word_count = (Width + 31) / 32;

    int32_t index = 0;
    const char* name = "";
    GetFn get_fn = nullptr;
    DepositFn deposit_fn = nullptr;
    ForceFn force_fn = nullptr;
    ReleaseFn release_fn = nullptr;

    value_type get() const {
        detail::require_callback(name);
        if (!get_fn) {
            std::fprintf(stderr,
                         "cpptb: internal probe '%s' has no read callback\n",
                         name ? name : "<unnamed>");
            std::abort();
        }

        return normalize(get_fn(index));
    }

    void deposit(value_type value) const requires(Writable) {
        detail::require_write_callback(name, "deposit");
        if (!deposit_fn) {
            std::fprintf(stderr,
                         "cpptb: internal probe '%s' has no deposit callback\n",
                         name ? name : "<unnamed>");
            std::abort();
        }

        deposit_fn(index, normalize(value));
    }

    void force(value_type value) const requires(Forceable) {
        detail::require_write_callback(name, "force");
        if (!force_fn) {
            std::fprintf(stderr,
                         "cpptb: internal probe '%s' has no force callback\n",
                         name ? name : "<unnamed>");
            std::abort();
        }

        force_fn(index, normalize(value));
    }

    void release() const requires(Forceable) {
        detail::require_write_callback(name, "release");
        if (!release_fn) {
            std::fprintf(stderr,
                         "cpptb: internal probe '%s' has no release callback\n",
                         name ? name : "<unnamed>");
            std::abort();
        }

        release_fn(index);
    }

   private:
    static value_type normalize(value_type value) {
        if constexpr (Width < 32) {
            return value & ((uint32_t{1} << Width) - 1);
        } else if constexpr (Width > 32 && Width < 64) {
            return value & ((uint64_t{1} << Width) - 1);
        } else {
            return value;
        }
    }
};

template <size_t Width, int32_t Left, int32_t Right, bool Writable,
          bool Forceable = false>
class MemoryProbe {
   public:
    using Element = Probe<Width, Writable, Forceable>;
    using GetFn = typename Element::GetFn;
    using DepositFn = typename Element::DepositFn;
    using ForceFn = typename Element::ForceFn;
    using ReleaseFn = typename Element::ReleaseFn;

    static constexpr size_t width = Width;
    static constexpr int32_t left() { return Left; }
    static constexpr int32_t right() { return Right; }
    static constexpr int32_t low() { return Left < Right ? Left : Right; }
    static constexpr int32_t high() { return Left < Right ? Right : Left; }
    static constexpr size_t size() {
        return static_cast<size_t>(high() - low()) + 1;
    }

    const char* name = "";
    GetFn get_fn = nullptr;
    DepositFn deposit_fn = nullptr;
    ForceFn force_fn = nullptr;
    ReleaseFn release_fn = nullptr;

    [[nodiscard]] Element at(int32_t index) const {
        if (index < low() || index > high()) {
            std::fprintf(stderr,
                         "cpptb: internal memory probe '%s' index %d is out "
                         "of bounds [%d:%d]\n",
                         name ? name : "<unnamed>", index, Left, Right);
            std::abort();
        }
        return Element{index, name, get_fn, deposit_fn, force_fn, release_fn};
    }
};

}  // namespace cpptb::probe
