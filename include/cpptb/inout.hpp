#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include "cpptb/coro_runtime.hpp"

namespace cpptb {

template <std::size_t Width, typename Observed, auto DriveFn, auto HighZFn>
class InoutRef {
   public:
    using value_type = coro::PackedSignalValue<Width>;
    static constexpr std::size_t width = Width;

    Observed observed;
    std::int32_t linear_index = 0;

    [[nodiscard]] value_type get() const {
        if constexpr (Width <= 32) {
            return static_cast<value_type>(observed.get());
        } else {
            return observed.get();
        }
    }

    void drive(value_type value) const {
#ifdef CPPTB_HIERARCHY_DISCOVERY
        (void)value;
#else
        DriveFn(linear_index, std::move(value));
#endif
    }

    void high_z() const {
#ifndef CPPTB_HIERARCHY_DISCOVERY
        HighZFn(linear_index);
#endif
    }

    operator coro::Signal() const requires(Width == 1) {
        return static_cast<coro::Signal>(observed);
    }
};

template <std::size_t Width, typename Observed, auto DriveFn, auto HighZFn,
          std::size_t DimensionIndex, typename Dimension,
          typename... RemainingDimensions>
class InoutArray {
   public:
    using value_type = coro::PackedSignalValue<Width>;
    static constexpr std::size_t width = Width;
    static constexpr std::size_t rank = 1 + sizeof...(RemainingDimensions);
    static constexpr std::int32_t left() { return Dimension::left; }
    static constexpr std::int32_t right() { return Dimension::right; }
    static constexpr std::int32_t low() { return Dimension::low; }
    static constexpr std::int32_t high() { return Dimension::high; }
    static constexpr std::size_t size() { return Dimension::size; }

    Observed observed;
    std::int32_t linear_index = 0;

    [[nodiscard]] auto operator[](std::int32_t index) const {
        const auto selected = observed[index];
        const auto next_linear = static_cast<std::int32_t>(
            linear_index * Dimension::size + (index - Dimension::low));
        if constexpr (sizeof...(RemainingDimensions) == 0) {
            return InoutRef<Width, decltype(selected), DriveFn, HighZFn>{
                selected, next_linear};
        } else {
            return InoutArray<Width, decltype(selected), DriveFn, HighZFn,
                              DimensionIndex + 1,
                              RemainingDimensions...>{selected, next_linear};
        }
    }

    [[nodiscard]] auto at(std::int32_t index) const {
        return (*this)[index];
    }
};

}  // namespace cpptb
