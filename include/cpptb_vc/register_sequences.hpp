// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <span>
#include <type_traits>

#include "cpptb_vc/register_model.hpp"

namespace cpptb::vc {

struct RegisterSequenceResult {
    uint64_t registers_visited = 0;
    uint64_t registers_tested = 0;
    uint64_t registers_skipped = 0;
    uint64_t bits_tested = 0;
    uint64_t frontdoor_reads = 0;
    uint64_t frontdoor_writes = 0;
    uint64_t backdoor_reads = 0;
    uint64_t backdoor_writes = 0;
};

struct RegisterResetCheckOptions {
    AccessPath path = AccessPath::Frontdoor;
};

struct RegisterBitBashOptions {
    AccessPath path = AccessPath::Frontdoor;
};

namespace register_sequence_detail {

template <typename Data>
[[nodiscard]] bool bit(const Data& value, std::size_t index) {
    if constexpr (std::unsigned_integral<Data>) {
        return ((value >> index) & Data{1}) != 0;
    } else {
        return value.bit(index);
    }
}

template <typename Data>
void set_bit(Data& value, std::size_t index, bool enabled) {
    if constexpr (std::unsigned_integral<Data>) {
        const Data mask = Data{1} << index;
        value = enabled ? static_cast<Data>(value | mask)
                        : static_cast<Data>(value & ~mask);
    } else {
        value.set_bit(index, enabled);
    }
}

template <typename Data>
[[nodiscard]] Data value_from_words(std::span<const uint32_t> words,
                                    uint64_t fallback,
                                    std::size_t width) {
    Data result{};
    for (std::size_t index = 0; index < width; ++index) {
        const bool enabled = words.empty()
                                 ? index < 64 && ((fallback >> index) & 1u) != 0
                                 : index / 32 < words.size() &&
                                       ((words[index / 32] >> (index % 32)) &
                                        1u) != 0;
        set_bit(result, index, enabled);
    }
    return result;
}

template <typename Data>
[[nodiscard]] Data full_mask(std::size_t width) {
    Data result{};
    for (std::size_t index = 0; index < width; ++index) {
        set_bit(result, index, true);
    }
    return result;
}

template <typename Data>
[[nodiscard]] Data intersect(const Data& left, const Data& right,
                             std::size_t width) {
    Data result{};
    for (std::size_t index = 0; index < width; ++index) {
        set_bit(result, index, bit(left, index) && bit(right, index));
    }
    return result;
}

template <typename Data>
[[nodiscard]] Data masked(const Data& value, const Data& mask,
                          std::size_t width) {
    return intersect(value, mask, width);
}

template <typename Data>
[[nodiscard]] bool any(const Data& value, std::size_t width) {
    for (std::size_t index = 0; index < width; ++index) {
        if (bit(value, index)) return true;
    }
    return false;
}

template <typename Data>
void enable_field(Data& value, const RegisterFieldDescriptor& field,
                  std::size_t width) {
    const std::size_t end =
        std::min<std::size_t>(width, field.lsb + field.width);
    for (std::size_t index = field.lsb; index < end; ++index) {
        set_bit(value, index, true);
    }
}

template <typename Data>
[[nodiscard]] Data stable_read_mask(const RegisterDescriptor& descriptor) {
    if (descriptor.fields.empty()) return full_mask<Data>(descriptor.width);
    for (const auto& field : descriptor.fields) {
        if (register_readable(field.access) &&
            field.read_effect != RegisterReadEffect::None) {
            return {};
        }
    }
    Data result{};
    for (const auto& field : descriptor.fields) {
        if (!register_readable(field.access) || field.volatile_value) {
            continue;
        }
        enable_field(result, field, descriptor.width);
    }
    return result;
}

template <typename Data>
[[nodiscard]] Data stable_read_write_mask(
    const RegisterDescriptor& descriptor) {
    if (descriptor.fields.empty()) return full_mask<Data>(descriptor.width);
    Data result{};
    for (const auto& field : descriptor.fields) {
        if (register_writable(field.access) &&
            (field.access != RegisterAccess::ReadWrite ||
             field.write_effect != RegisterWriteEffect::None)) {
            return {};
        }
        if (register_readable(field.access) &&
            field.read_effect != RegisterReadEffect::None) {
            return {};
        }
        if (field.access == RegisterAccess::ReadWrite &&
            !field.volatile_value) {
            enable_field(result, field, descriptor.width);
        }
    }
    return result;
}

template <typename Data>
[[nodiscard]] Data alternating_pattern(const RegisterDescriptor& descriptor,
                                       bool inverted) {
    Data result{};
    const bool phase = ((descriptor.address >> 2u) & 1u) != 0;
    for (std::size_t index = 0; index < descriptor.width; ++index) {
        set_bit(result, index,
                (((index & 1u) != 0) != phase) != inverted);
    }
    return result;
}

[[noreturn]] inline void fail_missing_backdoor(std::string_view sequence,
                                               std::string_view path) {
    throw std::logic_error("cpptb-vc: " + std::string{sequence} +
                           " requires a generated register backdoor: " +
                           std::string{path});
}

class ResetCheckVisitor {
   public:
    ResetCheckVisitor(const TestContext& test, RegisterResetCheckOptions options)
        : test_(&test), options_(options) {}

    template <typename Handle>
    coro::Task<void> operator()(Handle& handle) {
        using data_type = typename Handle::data_type;
        const auto& descriptor = handle.descriptor();
        const std::size_t width = handle.width();
        ++result_.registers_visited;

        const auto known_reset = value_from_words<data_type>(
            descriptor.reset_mask_words, descriptor.reset_mask, width);
        const auto compare_mask = intersect(
            known_reset, stable_read_mask<data_type>(descriptor), width);
        if (!any(compare_mask, width)) {
            ++result_.registers_skipped;
            co_return;
        }
        ++result_.registers_tested;
        const auto expected = masked(
            value_from_words<data_type>(descriptor.reset_value_words,
                                        descriptor.reset_value, width),
            compare_mask, width);

        if (options_.path == AccessPath::Frontdoor) {
            const auto response = co_await handle.read();
            ++result_.frontdoor_reads;
            test_->expect(handle.path(), response.okay());
            if (!response.okay()) co_return;
            const auto valid_mask =
                intersect(compare_mask, response.valid_mask, width);
            test_->expect_eq(handle.path(),
                             masked(response.data, valid_mask, width),
                             masked(expected, valid_mask, width));
            co_return;
        }

        if constexpr (requires(Handle& candidate) {
                          { candidate.peek() } -> std::same_as<data_type>;
                      }) {
            if (!handle.has_backdoor()) {
                fail_missing_backdoor("register reset check", handle.path());
            }
            const auto actual = handle.peek();
            ++result_.backdoor_reads;
            test_->expect_eq(handle.path(),
                             masked(actual, compare_mask, width), expected);
        } else {
            fail_missing_backdoor("register reset check", handle.path());
        }
    }

    [[nodiscard]] RegisterSequenceResult result() const noexcept {
        return result_;
    }

   private:
    const TestContext* test_;
    RegisterResetCheckOptions options_;
    RegisterSequenceResult result_;
};

class AccessCheckVisitor {
   public:
    explicit AccessCheckVisitor(const TestContext& test) : test_(&test) {}

    template <typename Handle>
    coro::Task<void> operator()(Handle& handle) {
        using data_type = typename Handle::data_type;
        const auto& descriptor = handle.descriptor();
        const std::size_t width = handle.width();
        ++result_.registers_visited;
        const auto read_mask = stable_read_mask<data_type>(descriptor);
        const auto write_mask = stable_read_write_mask<data_type>(descriptor);
        if (!any(read_mask, width) && !any(write_mask, width)) {
            ++result_.registers_skipped;
            co_return;
        }

        if constexpr (requires(Handle& candidate, const data_type& value) {
                          { candidate.peek() } -> std::same_as<data_type>;
                          candidate.poke(value);
                      }) {
            if (!handle.has_backdoor()) {
                fail_missing_backdoor("register access check", handle.path());
            }
            ++result_.registers_tested;
            const auto original = handle.peek();
            ++result_.backdoor_reads;

            if (any(read_mask, width)) {
                const auto deposited =
                    alternating_pattern<data_type>(descriptor, false);
                handle.poke(deposited);
                ++result_.backdoor_writes;
                const auto response = co_await handle.read();
                ++result_.frontdoor_reads;
                test_->expect(handle.path(), response.okay());
                if (response.okay()) {
                    const auto valid_mask =
                        intersect(read_mask, response.valid_mask, width);
                    test_->expect_eq(
                        handle.path(), masked(response.data, valid_mask, width),
                        masked(deposited, valid_mask, width));
                }
            }

            if (any(write_mask, width)) {
                const auto written =
                    alternating_pattern<data_type>(descriptor, true);
                const auto response = co_await handle.write(written);
                ++result_.frontdoor_writes;
                test_->expect(handle.path(), response.okay());
                const auto actual = handle.peek();
                ++result_.backdoor_reads;
                test_->expect_eq(handle.path(),
                                 masked(actual, write_mask, width),
                                 masked(written, write_mask, width));
            }

            handle.poke(original);
            ++result_.backdoor_writes;
            co_return;
        }
        fail_missing_backdoor("register access check", handle.path());
    }

    [[nodiscard]] RegisterSequenceResult result() const noexcept {
        return result_;
    }

   private:
    const TestContext* test_;
    RegisterSequenceResult result_;
};

class BitBashVisitor {
   public:
    BitBashVisitor(const TestContext& test, RegisterBitBashOptions options)
        : test_(&test), options_(options) {}

    template <typename Handle>
    coro::Task<void> operator()(Handle& handle) {
        using data_type = typename Handle::data_type;
        const auto& descriptor = handle.descriptor();
        const std::size_t width = handle.width();
        ++result_.registers_visited;
        const auto test_mask = stable_read_write_mask<data_type>(descriptor);
        if (!any(test_mask, width)) {
            ++result_.registers_skipped;
            co_return;
        }

        data_type original{};
        if (options_.path == AccessPath::Frontdoor) {
            const auto response = co_await handle.read();
            ++result_.frontdoor_reads;
            test_->expect(handle.path(), response.okay());
            if (!response.okay()) co_return;
            original = response.data;
        } else if constexpr (requires(Handle& candidate) {
                                 { candidate.peek() } -> std::same_as<data_type>;
                             }) {
            if (!handle.has_backdoor()) {
                fail_missing_backdoor("register bit bash", handle.path());
            }
            original = handle.peek();
            ++result_.backdoor_reads;
        } else {
            fail_missing_backdoor("register bit bash", handle.path());
        }

        ++result_.registers_tested;
        for (std::size_t index = 0; index < width; ++index) {
            if (!bit(test_mask, index)) continue;
            auto candidate = original;
            set_bit(candidate, index, !bit(original, index));
            data_type actual{};

            if (options_.path == AccessPath::Frontdoor) {
                const auto write = co_await handle.write(candidate);
                ++result_.frontdoor_writes;
                test_->expect(handle.path(), write.okay());
                if (!write.okay()) continue;
                const auto read = co_await handle.read();
                ++result_.frontdoor_reads;
                test_->expect(handle.path(), read.okay());
                if (!read.okay()) continue;
                actual = read.data;
            } else if constexpr (requires(Handle& selected,
                                          const data_type& value) {
                                     selected.poke(value);
                                     { selected.peek() } ->
                                         std::same_as<data_type>;
                                 }) {
                handle.poke(candidate);
                ++result_.backdoor_writes;
                actual = handle.peek();
                ++result_.backdoor_reads;
            }

            test_->expect_eq(handle.path(), bit(actual, index),
                             bit(candidate, index));
            ++result_.bits_tested;
        }

        if (options_.path == AccessPath::Frontdoor) {
            const auto restore = co_await handle.write(original);
            ++result_.frontdoor_writes;
            test_->expect(handle.path(), restore.okay());
        } else if constexpr (requires(Handle& selected,
                                      const data_type& value) {
                                 selected.poke(value);
                             }) {
            handle.poke(original);
            ++result_.backdoor_writes;
        }
    }

    [[nodiscard]] RegisterSequenceResult result() const noexcept {
        return result_;
    }

   private:
    const TestContext* test_;
    RegisterBitBashOptions options_;
    RegisterSequenceResult result_;
};

}  // namespace register_sequence_detail

template <typename Model>
coro::Task<RegisterSequenceResult> register_reset_check(
    const TestContext& test, Model& model,
    RegisterResetCheckOptions options = {}) {
    register_sequence_detail::ResetCheckVisitor visitor{test, options};
    co_await model.for_each_register_async(visitor);
    co_return visitor.result();
}

template <typename Model>
coro::Task<RegisterSequenceResult> register_access_check(
    const TestContext& test, Model& model) {
    register_sequence_detail::AccessCheckVisitor visitor{test};
    co_await model.for_each_register_async(visitor);
    co_return visitor.result();
}

template <typename Model>
coro::Task<RegisterSequenceResult> register_bit_bash(
    const TestContext& test, Model& model,
    RegisterBitBashOptions options = {}) {
    register_sequence_detail::BitBashVisitor visitor{test, options};
    co_await model.for_each_register_async(visitor);
    co_return visitor.result();
}

}  // namespace cpptb::vc
