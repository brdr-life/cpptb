// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <ranges>
#include <source_location>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "cpptb/random.hpp"

namespace cpptb {

template <typename Value>
concept RandomScalar =
    (std::integral<Value> || std::is_enum_v<Value>) && sizeof(Value) <= 8;

namespace randomize_detail {

template <typename Value, bool = std::is_enum_v<Value>>
struct scalar_base {
    using type = Value;
};

template <typename Value>
struct scalar_base<Value, true> {
    using type = std::underlying_type_t<Value>;
};

template <typename Value>
using scalar_base_t = typename scalar_base<Value>::type;

struct ScalarType {
    uint8_t width = 1;
    bool is_signed = false;
};

constexpr uint64_t width_mask(uint8_t width) noexcept {
    return width == 64 ? std::numeric_limits<uint64_t>::max()
                       : (uint64_t{1} << width) - 1;
}

constexpr uint64_t normalize(uint64_t raw, ScalarType type) noexcept {
    return raw & width_mask(type.width);
}

constexpr uint64_t ordered_raw(uint64_t raw, ScalarType type) noexcept {
    raw = normalize(raw, type);
    if (!type.is_signed) return raw;
    return raw ^ (uint64_t{1} << (type.width - 1));
}

constexpr uint64_t from_ordered_raw(uint64_t ordered,
                                    ScalarType type) noexcept {
    if (type.is_signed) {
        ordered ^= uint64_t{1} << (type.width - 1);
    }
    return normalize(ordered, type);
}

constexpr int64_t signed_raw(uint64_t raw, ScalarType type) noexcept {
    raw = normalize(raw, type);
    if (!type.is_signed || type.width == 64) {
        return std::bit_cast<int64_t>(raw);
    }
    const uint64_t sign = uint64_t{1} << (type.width - 1);
    if ((raw & sign) != 0) raw |= ~width_mask(type.width);
    return std::bit_cast<int64_t>(raw);
}

template <RandomScalar Value>
constexpr ScalarType scalar_type() noexcept {
    using Base = scalar_base_t<Value>;
    return ScalarType{static_cast<uint8_t>(sizeof(Base) * 8),
                      std::is_signed_v<Base>};
}

template <RandomScalar Value>
constexpr uint64_t to_raw(Value value) noexcept {
    using Base = scalar_base_t<Value>;
    const Base base = static_cast<Base>(value);
    if constexpr (std::same_as<Base, bool>) {
        return base ? 1 : 0;
    } else if constexpr (std::is_signed_v<Base>) {
        using Unsigned = std::make_unsigned_t<Base>;
        return static_cast<uint64_t>(std::bit_cast<Unsigned>(base));
    } else {
        return static_cast<uint64_t>(base);
    }
}

template <RandomScalar Value>
constexpr Value from_raw(uint64_t raw) noexcept {
    using Base = scalar_base_t<Value>;
    if constexpr (std::same_as<Base, bool>) {
        return static_cast<Value>((raw & 1) != 0);
    } else {
        using Unsigned = std::make_unsigned_t<Base>;
        const auto bits = static_cast<Unsigned>(raw);
        const Base base = [&] {
            if constexpr (std::is_signed_v<Base>) {
                return std::bit_cast<Base>(bits);
            } else {
                return static_cast<Base>(bits);
            }
        }();
        return static_cast<Value>(base);
    }
}

enum class ExprOp : uint8_t {
    Constant,
    Variable,
    Add,
    Subtract,
    Multiply,
    Remainder,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    LogicalAnd,
    LogicalOr,
    LogicalNot,
    Inside,
};

struct RawRange {
    uint64_t minimum_raw = 0;
    uint64_t maximum_raw = 0;
};

struct ExprNode {
    ExprOp op = ExprOp::Constant;
    ScalarType type{};
    uint64_t raw = 0;
    std::size_t variable = 0;
    std::shared_ptr<const ExprNode> left;
    std::shared_ptr<const ExprNode> right;
    std::shared_ptr<const std::vector<RawRange>> ranges;
};

inline auto make_value_node(ExprOp op, ScalarType type,
                            std::shared_ptr<const ExprNode> left = {},
                            std::shared_ptr<const ExprNode> right = {}) {
    return std::make_shared<ExprNode>(
        ExprNode{.op = op,
                 .type = type,
                 .left = std::move(left),
                 .right = std::move(right)});
}

inline auto make_boolean_node(ExprOp op,
                              std::shared_ptr<const ExprNode> left,
                              std::shared_ptr<const ExprNode> right = {}) {
    return make_value_node(op, ScalarType{1, false}, std::move(left),
                           std::move(right));
}

}  // namespace randomize_detail

template <RandomScalar Value>
class ValueExpr {
   public:
    using value_type = Value;

    ValueExpr expression() const { return *this; }

    // Public for backend adapters; users construct expressions through Rand.
    explicit ValueExpr(std::shared_ptr<const randomize_detail::ExprNode> node)
        : node_(std::move(node)) {}

    static ValueExpr constant(Value value) {
        auto node = std::make_shared<randomize_detail::ExprNode>();
        node->op = randomize_detail::ExprOp::Constant;
        node->type = randomize_detail::scalar_type<Value>();
        node->raw = randomize_detail::to_raw(value);
        return ValueExpr{std::move(node)};
    }

    const std::shared_ptr<const randomize_detail::ExprNode>& node() const {
        return node_;
    }

   private:
    std::shared_ptr<const randomize_detail::ExprNode> node_;

    template <RandomScalar>
    friend class Rand;
    template <typename Left, typename Right>
    friend auto randomize_binary_value(randomize_detail::ExprOp, const Left&,
                                       const Right&);
    template <typename Left, typename Right>
    friend auto randomize_compare(randomize_detail::ExprOp, const Left&,
                                  const Right&);
};

class Constraint {
   public:
    Constraint() = default;

    // Public for backend adapters; users construct constraints with operators.
    explicit Constraint(std::shared_ptr<const randomize_detail::ExprNode> node)
        : node_(std::move(node)) {}

    const std::shared_ptr<const randomize_detail::ExprNode>& node() const {
        return node_;
    }

   private:
    std::shared_ptr<const randomize_detail::ExprNode> node_;

    friend class Randomized;
    friend class RandomSearchBackend;
    template <typename Left, typename Right>
    friend auto randomize_compare(randomize_detail::ExprOp, const Left&,
                                  const Right&);
    friend Constraint operator&&(const Constraint&, const Constraint&);
    friend Constraint operator||(const Constraint&, const Constraint&);
    friend Constraint operator!(const Constraint&);
};

template <typename Operand>
concept RandomValueOperand = requires(const std::remove_cvref_t<Operand>& v) {
    typename std::remove_cvref_t<Operand>::value_type;
    v.expression();
};

template <typename Left, typename Right, bool = RandomValueOperand<Left>>
struct randomize_value;

template <typename Left, typename Right>
struct randomize_value<Left, Right, true> {
    using type = typename std::remove_cvref_t<Left>::value_type;
};

template <typename Left, typename Right>
struct randomize_value<Left, Right, false> {
    using type = typename std::remove_cvref_t<Right>::value_type;
};

template <typename Left, typename Right>
    requires(RandomValueOperand<Left> || RandomValueOperand<Right>)
using randomize_value_t = typename randomize_value<Left, Right>::type;

template <RandomScalar Value, typename Operand>
ValueExpr<Value> randomize_value_expression(const Operand& operand) {
    if constexpr (RandomValueOperand<Operand>) {
        static_assert(std::same_as<
                      Value,
                      typename std::remove_cvref_t<Operand>::value_type>,
                      "constraint operands must have the same value type");
        return operand.expression();
    } else {
        static_assert(std::convertible_to<Operand, Value>,
                      "constraint scalar is not convertible to the field type");
        return ValueExpr<Value>::constant(static_cast<Value>(operand));
    }
}

template <typename Left, typename Right>
    requires(RandomValueOperand<Left> || RandomValueOperand<Right>)
auto randomize_binary_value(randomize_detail::ExprOp op, const Left& left,
                            const Right& right) {
    using Value = randomize_value_t<Left, Right>;
    const auto lhs = randomize_value_expression<Value>(left);
    const auto rhs = randomize_value_expression<Value>(right);
    return ValueExpr<Value>{randomize_detail::make_value_node(
        op, randomize_detail::scalar_type<Value>(), lhs.node(), rhs.node())};
}

template <typename Left, typename Right>
    requires(RandomValueOperand<Left> || RandomValueOperand<Right>)
auto randomize_compare(randomize_detail::ExprOp op, const Left& left,
                       const Right& right) {
    using Value = randomize_value_t<Left, Right>;
    const auto lhs = randomize_value_expression<Value>(left);
    const auto rhs = randomize_value_expression<Value>(right);
    return Constraint{randomize_detail::make_boolean_node(
        op, lhs.node(), rhs.node())};
}

#define CPPTB_RANDOM_VALUE_OPERATOR(symbol, operation)                         \
    template <typename Left, typename Right>                                   \
        requires(RandomValueOperand<Left> || RandomValueOperand<Right>)        \
    auto operator symbol(const Left& left, const Right& right) {               \
        return randomize_binary_value(randomize_detail::ExprOp::operation,     \
                                      left, right);                            \
    }

CPPTB_RANDOM_VALUE_OPERATOR(+, Add)
CPPTB_RANDOM_VALUE_OPERATOR(-, Subtract)
CPPTB_RANDOM_VALUE_OPERATOR(*, Multiply)
CPPTB_RANDOM_VALUE_OPERATOR(%, Remainder)

#undef CPPTB_RANDOM_VALUE_OPERATOR

#define CPPTB_RANDOM_COMPARE_OPERATOR(symbol, operation)                       \
    template <typename Left, typename Right>                                   \
        requires(RandomValueOperand<Left> || RandomValueOperand<Right>)        \
    auto operator symbol(const Left& left, const Right& right) {               \
        return randomize_compare(randomize_detail::ExprOp::operation, left,    \
                                 right);                                       \
    }

CPPTB_RANDOM_COMPARE_OPERATOR(==, Equal)
CPPTB_RANDOM_COMPARE_OPERATOR(!=, NotEqual)
CPPTB_RANDOM_COMPARE_OPERATOR(<, Less)
CPPTB_RANDOM_COMPARE_OPERATOR(<=, LessEqual)
CPPTB_RANDOM_COMPARE_OPERATOR(>, Greater)
CPPTB_RANDOM_COMPARE_OPERATOR(>=, GreaterEqual)

#undef CPPTB_RANDOM_COMPARE_OPERATOR

inline Constraint operator&&(const Constraint& left, const Constraint& right) {
    return Constraint{randomize_detail::make_boolean_node(
        randomize_detail::ExprOp::LogicalAnd, left.node(), right.node())};
}

inline Constraint operator||(const Constraint& left, const Constraint& right) {
    return Constraint{randomize_detail::make_boolean_node(
        randomize_detail::ExprOp::LogicalOr, left.node(), right.node())};
}

inline Constraint operator!(const Constraint& value) {
    return Constraint{randomize_detail::make_boolean_node(
        randomize_detail::ExprOp::LogicalNot, value.node())};
}

template <RandomScalar Value>
struct RandomRange {
    using value_type = Value;

    Value minimum;
    Value maximum;
};

template <RandomScalar Value>
[[nodiscard]] constexpr RandomRange<Value> range(Value minimum,
                                                  Value maximum) {
    const auto type = randomize_detail::scalar_type<Value>();
    if (randomize_detail::ordered_raw(randomize_detail::to_raw(minimum), type) >
        randomize_detail::ordered_raw(randomize_detail::to_raw(maximum), type)) {
        throw std::invalid_argument(
            "cpptb::range minimum exceeds maximum");
    }
    return RandomRange<Value>{minimum, maximum};
}

namespace randomize_detail {

template <typename Entry>
struct is_random_range : std::false_type {};

template <RandomScalar Value>
struct is_random_range<RandomRange<Value>> : std::true_type {};

template <typename Entry>
inline constexpr bool is_random_range_v =
    is_random_range<std::remove_cvref_t<Entry>>::value;

template <RandomScalar Value, typename Entry>
void append_raw_range(std::vector<RawRange>& ranges, const Entry& entry) {
    const auto type = scalar_type<Value>();
    if constexpr (is_random_range_v<Entry>) {
        static_assert(
            std::same_as<Value,
                         typename std::remove_cvref_t<Entry>::value_type>,
            "range type must match the constrained field type");
        const uint64_t minimum = to_raw(entry.minimum);
        const uint64_t maximum = to_raw(entry.maximum);
        if (ordered_raw(minimum, type) > ordered_raw(maximum, type)) {
            throw std::invalid_argument(
                "cpptb constrained-random range minimum exceeds maximum");
        }
        ranges.push_back(RawRange{minimum, maximum});
    } else {
        static_assert(std::convertible_to<Entry, Value>,
                      "inside value is not convertible to the field type");
        const uint64_t raw = to_raw(static_cast<Value>(entry));
        ranges.push_back(RawRange{raw, raw});
    }
}

template <RandomScalar Value>
Constraint make_inside_constraint(
    const ValueExpr<Value>& expression,
    std::vector<RawRange> ranges) {
    if (ranges.empty()) {
        throw std::invalid_argument(
            "cpptb::inside requires at least one value or range");
    }
    auto node = make_boolean_node(ExprOp::Inside, expression.node());
    node->ranges =
        std::make_shared<const std::vector<RawRange>>(std::move(ranges));
    return Constraint{std::move(node)};
}

}  // namespace randomize_detail

template <RandomValueOperand Operand, typename... Entries>
    requires(sizeof...(Entries) > 0)
[[nodiscard]] Constraint inside(const Operand& operand,
                                const Entries&... entries) {
    using Value = typename std::remove_cvref_t<Operand>::value_type;
    std::vector<randomize_detail::RawRange> ranges;
    ranges.reserve(sizeof...(Entries));
    (randomize_detail::append_raw_range<Value>(ranges, entries), ...);
    return randomize_detail::make_inside_constraint(
        randomize_value_expression<Value>(operand), std::move(ranges));
}

template <RandomValueOperand Operand>
[[nodiscard]] Constraint inside(
    const Operand& operand,
    std::initializer_list<
        typename std::remove_cvref_t<Operand>::value_type> values) {
    using Value = typename std::remove_cvref_t<Operand>::value_type;
    std::vector<randomize_detail::RawRange> ranges;
    ranges.reserve(values.size());
    for (const Value value : values) {
        randomize_detail::append_raw_range<Value>(ranges, value);
    }
    return randomize_detail::make_inside_constraint(
        randomize_value_expression<Value>(operand), std::move(ranges));
}

struct WeightedRange {
    randomize_detail::RawRange range;
    uint64_t weight = 0;
};

class Distribution {
   public:
    Distribution() = default;

    // Public for backend adapters; users construct distributions with dist().
    Distribution(std::size_t variable, randomize_detail::ScalarType type,
                 std::vector<WeightedRange> entries)
        : variable_(variable), type_(type), entries_(std::move(entries)) {}

    std::size_t variable() const noexcept { return variable_; }
    randomize_detail::ScalarType type() const noexcept { return type_; }
    std::span<const WeightedRange> entries() const noexcept { return entries_; }

   private:
    std::size_t variable_ = 0;
    randomize_detail::ScalarType type_{};
    std::vector<WeightedRange> entries_;

};

namespace randomize_detail {

template <typename Entry>
struct is_weighted : std::false_type {};

template <typename Value>
struct is_weighted<Weighted<Value>> : std::true_type {};

template <typename Entry>
inline constexpr bool is_weighted_v =
    is_weighted<std::remove_cvref_t<Entry>>::value;

template <RandomScalar Value, typename Entry>
void append_distribution_entry(std::vector<WeightedRange>& entries,
                               const Entry& entry) {
    static_assert(is_weighted_v<Entry>,
                  "dist entries must use cpptb::weighted(value, weight)");
    using Item = typename std::remove_cvref_t<Entry>::value_type;
    std::vector<RawRange> ranges;
    append_raw_range<Value>(ranges, static_cast<const Item&>(entry.value));
    entries.push_back(WeightedRange{ranges.front(), entry.weight});
}

}  // namespace randomize_detail

template <RandomValueOperand Operand, typename... Entries>
    requires(sizeof...(Entries) > 0)
[[nodiscard]] Distribution dist(const Operand& operand,
                                const Entries&... entries) {
    using Value = typename std::remove_cvref_t<Operand>::value_type;
    static_assert((randomize_detail::is_weighted_v<Entries> && ...),
                  "dist entries must use cpptb::weighted(value, weight)");
    const auto expression = randomize_value_expression<Value>(operand);
    if (!expression.node() ||
        expression.node()->op != randomize_detail::ExprOp::Variable) {
        throw std::invalid_argument(
            "cpptb::dist requires a randomized field, not an expression");
    }

    std::vector<WeightedRange> result;
    result.reserve(sizeof...(Entries));
    (randomize_detail::append_distribution_entry<Value>(result, entries), ...);
    uint64_t total_weight = 0;
    for (const auto& entry : result) {
        if (entry.weight >
            std::numeric_limits<uint64_t>::max() - total_weight) {
            throw std::invalid_argument(
                "cpptb::dist total weight overflows uint64_t");
        }
        total_weight += entry.weight;
    }
    if (total_weight == 0) {
        throw std::invalid_argument(
            "cpptb::dist requires at least one positive weight");
    }
    return Distribution{expression.node()->variable,
                        randomize_detail::scalar_type<Value>(),
                        std::move(result)};
}

template <RandomValueOperand Operand, typename... Entries>
    requires(sizeof...(Entries) > 0)
[[nodiscard]] Distribution distribution(const Operand& operand,
                                        const Entries&... entries) {
    return dist(operand, entries...);
}

struct VariableDescriptor {
    std::size_t id = 0;
    std::string_view name;
    randomize_detail::ScalarType type{};
    uint64_t minimum_raw = 0;
    uint64_t maximum_raw = 0;
    std::vector<uint64_t> excluded_raw;
    bool cyclic = false;
};

struct NamedConstraint {
    std::string label;
    Constraint expression;
    bool soft = false;
    bool enabled = true;
};

struct NamedDistribution {
    std::string label;
    Distribution expression;
    bool enabled = true;
};

struct ConstraintModelIdentity final {};

struct ConstraintProblem {
    std::vector<VariableDescriptor> variables;
    std::vector<NamedConstraint> constraints;
    std::vector<NamedDistribution> distributions;
    std::shared_ptr<const ConstraintModelIdentity> model_identity;
    uint64_t model_revision = 0;
    std::size_t persistent_constraint_count = 0;
};

class RandomizeValues {
   public:
    static constexpr std::size_t inline_capacity = 8;

    RandomizeValues() = default;
    explicit RandomizeValues(std::size_t size) { resize(size); }

    void reserve(std::size_t capacity) {
        if (capacity > inline_capacity) overflow_.reserve(capacity);
    }

    void resize(std::size_t size) {
        const std::size_t old_size = size_;
        if (size > inline_capacity) {
            if (overflow_.empty()) {
                overflow_.assign(inline_.begin(),
                                 inline_.begin() +
                                     std::min(old_size, inline_capacity));
            }
            overflow_.resize(size);
        } else {
            if (!overflow_.empty()) {
                std::copy_n(overflow_.begin(), std::min(old_size, size),
                            inline_.begin());
            }
            if (size > old_size) {
                std::fill(inline_.begin() + old_size,
                          inline_.begin() + size, 0);
            }
            overflow_.clear();
        }
        size_ = size;
    }

    void push_back(uint64_t value) {
        if (size_ < inline_capacity && overflow_.empty()) {
            inline_[size_++] = value;
            return;
        }
        if (overflow_.empty()) {
            overflow_.assign(inline_.begin(), inline_.end());
        }
        overflow_.push_back(value);
        ++size_;
    }

    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    uint64_t& operator[](std::size_t index) noexcept {
        return data()[index];
    }
    const uint64_t& operator[](std::size_t index) const noexcept {
        return data()[index];
    }

    uint64_t* data() noexcept {
        return size_ > inline_capacity ? overflow_.data() : inline_.data();
    }
    const uint64_t* data() const noexcept {
        return size_ > inline_capacity ? overflow_.data() : inline_.data();
    }

    auto begin() noexcept { return data(); }
    auto end() noexcept { return data() + size_; }
    auto begin() const noexcept { return data(); }
    auto end() const noexcept { return data() + size_; }

    std::span<const uint64_t> span() const noexcept {
        return {data(), size_};
    }

   private:
    std::array<uint64_t, inline_capacity> inline_{};
    std::vector<uint64_t> overflow_;
    std::size_t size_ = 0;
};

enum class RandomizeStatus : uint8_t {
    Solved,
    Unsatisfiable,
    CycleExhausted,
    SearchExhausted,
    BackendError,
};

enum class RandomizeEngine : uint8_t {
    None,
    Sampling,
    Solver,
};

struct RandomizeResult {
    RandomizeStatus status = RandomizeStatus::BackendError;
    RandomizeEngine engine = RandomizeEngine::None;
    RandomizeValues values;
    std::vector<std::size_t> cycle_variables;
    std::string message;

    explicit operator bool() const noexcept {
        return status == RandomizeStatus::Solved;
    }
};

class ConstraintBackend {
   public:
    virtual ~ConstraintBackend() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual std::string_view version() const noexcept { return {}; }
    virtual RandomizeResult solve(const ConstraintProblem& problem,
                                  Random& random) = 0;
};

namespace randomize_detail {

struct EvaluatedValue {
    uint64_t raw = 0;
    bool boolean = false;
};

inline EvaluatedValue evaluate_node(const ExprNode& node,
                                    std::span<const uint64_t> values) {
    const auto arithmetic = [&](auto operation) {
        const auto lhs = evaluate_node(*node.left, values).raw;
        const auto rhs = evaluate_node(*node.right, values).raw;
        return EvaluatedValue{normalize(operation(lhs, rhs), node.type), false};
    };
    const auto compare = [&](auto unsigned_operation, auto signed_operation) {
        const auto lhs = evaluate_node(*node.left, values).raw;
        const auto rhs = evaluate_node(*node.right, values).raw;
        const bool result = node.left->type.is_signed
                                ? signed_operation(
                                      signed_raw(lhs, node.left->type),
                                      signed_raw(rhs, node.right->type))
                                : unsigned_operation(lhs, rhs);
        return EvaluatedValue{result ? 1u : 0u, result};
    };

    switch (node.op) {
        case ExprOp::Constant:
            return EvaluatedValue{normalize(node.raw, node.type), false};
        case ExprOp::Variable:
            return EvaluatedValue{normalize(values[node.variable], node.type),
                                  false};
        case ExprOp::Add:
            return arithmetic([](uint64_t lhs, uint64_t rhs) {
                return lhs + rhs;
            });
        case ExprOp::Subtract:
            return arithmetic([](uint64_t lhs, uint64_t rhs) {
                return lhs - rhs;
            });
        case ExprOp::Multiply:
            return arithmetic([](uint64_t lhs, uint64_t rhs) {
                return lhs * rhs;
            });
        case ExprOp::Remainder: {
            const auto lhs = evaluate_node(*node.left, values).raw;
            const auto rhs = evaluate_node(*node.right, values).raw;
            if (rhs == 0) return EvaluatedValue{};
            if (node.type.is_signed) {
                const auto signed_lhs = signed_raw(lhs, node.type);
                const auto signed_rhs = signed_raw(rhs, node.type);
                const auto result =
                    (signed_lhs == std::numeric_limits<int64_t>::min() &&
                     signed_rhs == -1)
                        ? 0
                        : signed_lhs % signed_rhs;
                return EvaluatedValue{normalize(std::bit_cast<uint64_t>(result),
                                                node.type),
                                      false};
            }
            return EvaluatedValue{normalize(lhs % rhs, node.type), false};
        }
        case ExprOp::Equal:
            return compare(std::equal_to{}, std::equal_to{});
        case ExprOp::NotEqual:
            return compare(std::not_equal_to{}, std::not_equal_to{});
        case ExprOp::Less:
            return compare(std::less{}, std::less{});
        case ExprOp::LessEqual:
            return compare(std::less_equal{}, std::less_equal{});
        case ExprOp::Greater:
            return compare(std::greater{}, std::greater{});
        case ExprOp::GreaterEqual:
            return compare(std::greater_equal{}, std::greater_equal{});
        case ExprOp::LogicalAnd: {
            const bool lhs = evaluate_node(*node.left, values).boolean;
            const bool rhs = evaluate_node(*node.right, values).boolean;
            return EvaluatedValue{lhs && rhs ? 1u : 0u, lhs && rhs};
        }
        case ExprOp::LogicalOr: {
            const bool lhs = evaluate_node(*node.left, values).boolean;
            const bool rhs = evaluate_node(*node.right, values).boolean;
            return EvaluatedValue{lhs || rhs ? 1u : 0u, lhs || rhs};
        }
        case ExprOp::LogicalNot: {
            const bool result = !evaluate_node(*node.left, values).boolean;
            return EvaluatedValue{result ? 1u : 0u, result};
        }
        case ExprOp::Inside: {
            const auto value = evaluate_node(*node.left, values).raw;
            const auto type = node.left->type;
            const auto ordered = ordered_raw(value, type);
            const bool result = node.ranges && std::ranges::any_of(
                                                    *node.ranges,
                                                    [&](const RawRange& range) {
                                                        return ordered >=
                                                                   ordered_raw(
                                                                       range.minimum_raw,
                                                                       type) &&
                                                               ordered <=
                                                                   ordered_raw(
                                                                       range.maximum_raw,
                                                                       type);
                                                    });
            return EvaluatedValue{result ? 1u : 0u, result};
        }
    }
    return EvaluatedValue{};
}

inline bool evaluate_constraint(const Constraint& constraint,
                                std::span<const uint64_t> values) {
    return constraint.node() &&
           evaluate_node(*constraint.node(), values).boolean;
}

inline std::string format_node(const ExprNode& node,
                               std::span<const VariableDescriptor> variables) {
    switch (node.op) {
        case ExprOp::Constant:
            if (node.type.is_signed) {
                return std::to_string(signed_raw(node.raw, node.type));
            }
            return std::to_string(normalize(node.raw, node.type));
        case ExprOp::Variable:
            return std::string{variables[node.variable].name};
        case ExprOp::LogicalNot:
            return "!(" + format_node(*node.left, variables) + ")";
        case ExprOp::Inside: {
            std::string result = format_node(*node.left, variables) +
                                 " inside {";
            if (node.ranges) {
                for (std::size_t index = 0; index < node.ranges->size();
                     ++index) {
                    if (index != 0) result += ", ";
                    const auto& range = (*node.ranges)[index];
                    const auto format_value = [&](uint64_t raw) {
                        return node.left->type.is_signed
                                   ? std::to_string(
                                         signed_raw(raw, node.left->type))
                                   : std::to_string(
                                         normalize(raw, node.left->type));
                    };
                    result += format_value(range.minimum_raw);
                    if (range.minimum_raw != range.maximum_raw) {
                        result += ":" + format_value(range.maximum_raw);
                    }
                }
            }
            return result + "}";
        }
        default:
            break;
    }
    const auto symbol = [&]() -> std::string_view {
        switch (node.op) {
            case ExprOp::Add:
                return "+";
            case ExprOp::Subtract:
                return "-";
            case ExprOp::Multiply:
                return "*";
            case ExprOp::Remainder:
                return "%";
            case ExprOp::Equal:
                return "==";
            case ExprOp::NotEqual:
                return "!=";
            case ExprOp::Less:
                return "<";
            case ExprOp::LessEqual:
                return "<=";
            case ExprOp::Greater:
                return ">";
            case ExprOp::GreaterEqual:
                return ">=";
            case ExprOp::LogicalAnd:
                return "&&";
            case ExprOp::LogicalOr:
                return "||";
            default:
                return "?";
        }
    }();
    return "(" + format_node(*node.left, variables) + " " +
           std::string{symbol} + " " + format_node(*node.right, variables) +
           ")";
}

struct CandidateDomain {
    uint64_t low_ordered = 0;
    uint64_t high_ordered = 0;
    bool restricted = false;
    std::vector<RawRange> ranges;
};

inline ExprOp reverse_comparison(ExprOp op) {
    switch (op) {
        case ExprOp::Less:
            return ExprOp::Greater;
        case ExprOp::LessEqual:
            return ExprOp::GreaterEqual;
        case ExprOp::Greater:
            return ExprOp::Less;
        case ExprOp::GreaterEqual:
            return ExprOp::LessEqual;
        default:
            return op;
    }
}

inline bool tighten_domain(const ExprNode& node,
                           std::vector<CandidateDomain>& domains) {
    if (node.op == ExprOp::LogicalAnd) {
        const bool left = tighten_domain(*node.left, domains);
        const bool right = tighten_domain(*node.right, domains);
        return left && right;
    }
    if (node.op == ExprOp::Inside && node.left && node.ranges &&
        node.left->op == ExprOp::Variable) {
        auto& domain = domains.at(node.left->variable);
        std::vector<RawRange> incoming;
        incoming.reserve(node.ranges->size());
        for (const auto& range : *node.ranges) {
            incoming.push_back(RawRange{
                ordered_raw(range.minimum_raw, node.left->type),
                ordered_raw(range.maximum_raw, node.left->type)});
        }
        if (!domain.restricted) {
            domain.ranges = std::move(incoming);
            domain.restricted = true;
            return true;
        }

        std::vector<RawRange> intersection;
        intersection.reserve(domain.ranges.size() * incoming.size());
        for (const auto& left : domain.ranges) {
            for (const auto& right : incoming) {
                const uint64_t minimum =
                    std::max(left.minimum_raw, right.minimum_raw);
                const uint64_t maximum =
                    std::min(left.maximum_raw, right.maximum_raw);
                if (minimum <= maximum) {
                    intersection.push_back(RawRange{minimum, maximum});
                }
            }
        }
        domain.ranges = std::move(intersection);
        return true;
    }
    if (node.op < ExprOp::Equal || node.op > ExprOp::GreaterEqual) return false;

    const ExprNode* variable = node.left.get();
    const ExprNode* constant = node.right.get();
    ExprOp operation = node.op;
    if (variable->op != ExprOp::Variable ||
        constant->op != ExprOp::Constant) {
        variable = node.right.get();
        constant = node.left.get();
        operation = reverse_comparison(operation);
    }
    if (variable->op != ExprOp::Variable ||
        constant->op != ExprOp::Constant ||
        operation == ExprOp::NotEqual) {
        return false;
    }

    auto& domain = domains.at(variable->variable);
    const uint64_t value = ordered_raw(constant->raw, variable->type);
    switch (operation) {
        case ExprOp::Equal:
            domain.low_ordered = std::max(domain.low_ordered, value);
            domain.high_ordered = std::min(domain.high_ordered, value);
            break;
        case ExprOp::Less:
            if (value == 0) {
                domain.low_ordered = 1;
                domain.high_ordered = 0;
            } else {
                domain.high_ordered =
                    std::min(domain.high_ordered, value - 1);
            }
            break;
        case ExprOp::LessEqual:
            domain.high_ordered = std::min(domain.high_ordered, value);
            break;
        case ExprOp::Greater:
            if (value == std::numeric_limits<uint64_t>::max()) {
                domain.low_ordered = 1;
                domain.high_ordered = 0;
            } else {
                domain.low_ordered = std::max(domain.low_ordered, value + 1);
            }
            break;
        case ExprOp::GreaterEqual:
            domain.low_ordered = std::max(domain.low_ordered, value);
            break;
        default:
            break;
    }
    return true;
}

inline void finalize_domain(CandidateDomain& domain) {
    if (!domain.restricted) {
        if (domain.low_ordered <= domain.high_ordered) {
            domain.ranges = {
                RawRange{domain.low_ordered, domain.high_ordered}};
        } else {
            domain.ranges.clear();
        }
        return;
    }

    std::vector<RawRange> clamped;
    clamped.reserve(domain.ranges.size());
    for (const auto& range : domain.ranges) {
        const uint64_t minimum =
            std::max(range.minimum_raw, domain.low_ordered);
        const uint64_t maximum =
            std::min(range.maximum_raw, domain.high_ordered);
        if (minimum <= maximum) {
            clamped.push_back(RawRange{minimum, maximum});
        }
    }
    std::ranges::sort(clamped, {}, &RawRange::minimum_raw);
    domain.ranges.clear();
    for (const auto& range : clamped) {
        if (domain.ranges.empty() ||
            (domain.ranges.back().maximum_raw !=
                 std::numeric_limits<uint64_t>::max() &&
             range.minimum_raw > domain.ranges.back().maximum_raw + 1)) {
            domain.ranges.push_back(range);
        } else {
            domain.ranges.back().maximum_raw =
                std::max(domain.ranges.back().maximum_raw, range.maximum_raw);
        }
    }
}

inline bool domain_contains(const CandidateDomain& domain,
                            uint64_t ordered) {
    return std::ranges::any_of(domain.ranges, [&](const RawRange& range) {
        return ordered >= range.minimum_raw && ordered <= range.maximum_raw;
    });
}

inline uint128_t inclusive_size(uint64_t minimum, uint64_t maximum) {
    return static_cast<uint128_t>(maximum) - minimum + 1;
}

inline uint128_t domain_size(const CandidateDomain& domain) {
    uint128_t result = 0;
    for (const auto& range : domain.ranges) {
        result += inclusive_size(range.minimum_raw, range.maximum_raw);
    }
    return result;
}

inline uint64_t random_offset(Random& random, uint128_t size) {
    constexpr uint128_t full_u64 = uint128_t{1} << 64;
    if (size == full_u64) return random.next_u64();
    return random.randint<uint64_t>(0, static_cast<uint64_t>(size - 1));
}

inline uint64_t sample_domain(const CandidateDomain& domain, Random& random) {
    uint64_t offset = random_offset(random, domain_size(domain));
    for (const auto& range : domain.ranges) {
        const auto size = inclusive_size(range.minimum_raw, range.maximum_raw);
        if (static_cast<uint128_t>(offset) < size) {
            return range.minimum_raw + offset;
        }
        offset -= static_cast<uint64_t>(size);
    }
    throw std::logic_error("cpptb random domain sampling overflow");
}

inline uint128_t distribution_entry_size(
    const WeightedRange& entry, const CandidateDomain& domain,
    ScalarType type) {
    const uint64_t entry_minimum =
        ordered_raw(entry.range.minimum_raw, type);
    const uint64_t entry_maximum =
        ordered_raw(entry.range.maximum_raw, type);
    uint128_t result = 0;
    for (const auto& range : domain.ranges) {
        const uint64_t minimum = std::max(entry_minimum, range.minimum_raw);
        const uint64_t maximum = std::min(entry_maximum, range.maximum_raw);
        if (minimum <= maximum) result += inclusive_size(minimum, maximum);
    }
    return result;
}

inline uint64_t sample_distribution_entry(
    const WeightedRange& entry, const CandidateDomain& domain,
    ScalarType type, Random& random) {
    uint64_t offset =
        random_offset(random, distribution_entry_size(entry, domain, type));
    const uint64_t entry_minimum =
        ordered_raw(entry.range.minimum_raw, type);
    const uint64_t entry_maximum =
        ordered_raw(entry.range.maximum_raw, type);
    for (const auto& range : domain.ranges) {
        const uint64_t minimum = std::max(entry_minimum, range.minimum_raw);
        const uint64_t maximum = std::min(entry_maximum, range.maximum_raw);
        if (minimum > maximum) continue;
        const auto size = inclusive_size(minimum, maximum);
        if (static_cast<uint128_t>(offset) < size) return minimum + offset;
        offset -= static_cast<uint64_t>(size);
    }
    throw std::logic_error("cpptb distribution sampling overflow");
}

inline bool distribution_contains(const NamedDistribution* distribution,
                                  const CandidateDomain& domain,
                                  ScalarType type, uint64_t ordered) {
    if (!distribution) return domain_contains(domain, ordered);
    if (!domain_contains(domain, ordered)) return false;
    return std::ranges::any_of(
        distribution->expression.entries(), [&](const WeightedRange& entry) {
            return entry.weight != 0 &&
                   ordered >= ordered_raw(entry.range.minimum_raw, type) &&
                   ordered <= ordered_raw(entry.range.maximum_raw, type);
        });
}

inline uint64_t sample_variable(
    const CandidateDomain& domain,
    const NamedDistribution* distribution, ScalarType type, Random& random) {
    if (!distribution) return sample_domain(domain, random);

    uint64_t total_weight = 0;
    for (const auto& entry : distribution->expression.entries()) {
        if (entry.weight != 0 &&
            distribution_entry_size(entry, domain, type) != 0) {
            total_weight += entry.weight;
        }
    }
    uint64_t selected = random.randint<uint64_t>(0, total_weight - 1);
    for (const auto& entry : distribution->expression.entries()) {
        if (entry.weight == 0 ||
            distribution_entry_size(entry, domain, type) == 0) {
            continue;
        }
        if (selected < entry.weight) {
            return sample_distribution_entry(entry, domain, type, random);
        }
        selected -= entry.weight;
    }
    throw std::logic_error("cpptb distribution selection overflow");
}

inline std::string constraint_name(const NamedConstraint& constraint,
                                   const ConstraintProblem& problem) {
    return constraint.label.empty()
               ? format_node(*constraint.expression.node(),
                             std::span<const VariableDescriptor>{
                                 problem.variables})
               : constraint.label;
}

inline RandomizeResult random_search(const ConstraintProblem& problem,
                                     Random& random, std::size_t attempts) {
    static thread_local std::vector<CandidateDomain> domains;
    static thread_local std::vector<std::size_t> required_constraints;
    static thread_local std::vector<std::size_t> soft_constraints;
    static thread_local std::vector<const NamedDistribution*> distributions;
    static thread_local std::vector<uint64_t> rejection_counts;
    static thread_local std::vector<uint8_t> best_soft;
    static thread_local std::vector<uint8_t> candidate_soft;

    const bool has_soft = std::ranges::any_of(
        problem.constraints,
        [](const NamedConstraint& constraint) { return constraint.soft; });

    auto configure = [&](bool require_soft) -> RandomizeResult {
        domains.resize(problem.variables.size());
        for (std::size_t index = 0; index < problem.variables.size(); ++index) {
            const auto& variable = problem.variables[index];
            auto& domain = domains[index];
            domain.low_ordered = ordered_raw(variable.minimum_raw, variable.type);
            domain.high_ordered =
                ordered_raw(variable.maximum_raw, variable.type);
            domain.restricted = false;
            domain.ranges.clear();
        }
        required_constraints.clear();
        soft_constraints.clear();
        for (std::size_t index = 0; index < problem.constraints.size(); ++index) {
            const auto& constraint = problem.constraints[index];
            if (!constraint.expression.node()) {
                return RandomizeResult{
                    .status = RandomizeStatus::BackendError,
                    .message = "deterministic search received an empty "
                               "constraint expression"};
            }
            if (constraint.soft) soft_constraints.push_back(index);
            if (constraint.soft && !require_soft) continue;
            if (!tighten_domain(*constraint.expression.node(), domains)) {
                required_constraints.push_back(index);
            }
        }
        for (auto& domain : domains) finalize_domain(domain);

        distributions.assign(problem.variables.size(), nullptr);
        for (const auto& distribution : problem.distributions) {
            const std::size_t variable = distribution.expression.variable();
            if (variable >= distributions.size()) {
                return RandomizeResult{
                    .status = RandomizeStatus::BackendError,
                    .message = "distribution '" + distribution.label +
                               "' references an unknown variable"};
            }
            if (distributions[variable]) {
                return RandomizeResult{
                    .status = RandomizeStatus::BackendError,
                    .message = "multiple active distributions target '" +
                               std::string{problem.variables[variable].name} +
                               "'"};
            }
            distributions[variable] = &distribution;
        }

        for (std::size_t index = 0; index < domains.size(); ++index) {
            if (domains[index].ranges.empty()) {
                return RandomizeResult{
                    .status = RandomizeStatus::Unsatisfiable,
                    .message = "constraint domain for '" +
                               std::string{problem.variables[index].name} +
                               "' is empty"};
            }
            if (distributions[index]) {
                const bool supported = std::ranges::any_of(
                    distributions[index]->expression.entries(),
                    [&](const WeightedRange& entry) {
                        return entry.weight != 0 &&
                               distribution_entry_size(
                                   entry, domains[index],
                                   problem.variables[index].type) != 0;
                    });
                if (!supported) {
                    return RandomizeResult{
                        .status = RandomizeStatus::Unsatisfiable,
                        .message = "distribution for '" +
                                   std::string{problem.variables[index].name} +
                                   "' has no value in the active domain"};
                }
            }
        }

        std::vector<std::size_t> completed_cycles;
        for (std::size_t index = 0; index < domains.size(); ++index) {
            const auto& variable = problem.variables[index];
            if (!variable.cyclic || variable.excluded_raw.empty()) continue;

            uint128_t support_size = 0;
            if (!distributions[index]) {
                support_size = domain_size(domains[index]);
            } else {
                std::vector<RawRange> support;
                for (const auto& entry :
                     distributions[index]->expression.entries()) {
                    if (entry.weight == 0) continue;
                    const uint64_t entry_minimum =
                        ordered_raw(entry.range.minimum_raw, variable.type);
                    const uint64_t entry_maximum =
                        ordered_raw(entry.range.maximum_raw, variable.type);
                    for (const auto& domain_range : domains[index].ranges) {
                        const uint64_t minimum =
                            std::max(entry_minimum, domain_range.minimum_raw);
                        const uint64_t maximum =
                            std::min(entry_maximum, domain_range.maximum_raw);
                        if (minimum <= maximum) {
                            support.push_back(RawRange{minimum, maximum});
                        }
                    }
                }
                std::ranges::sort(support, {}, &RawRange::minimum_raw);
                uint64_t current_minimum = 0;
                uint64_t current_maximum = 0;
                bool have_range = false;
                for (const auto& range : support) {
                    if (!have_range) {
                        current_minimum = range.minimum_raw;
                        current_maximum = range.maximum_raw;
                        have_range = true;
                    } else if (current_maximum ==
                                   std::numeric_limits<uint64_t>::max() ||
                               range.minimum_raw <= current_maximum + 1) {
                        current_maximum =
                            std::max(current_maximum, range.maximum_raw);
                    } else {
                        support_size +=
                            inclusive_size(current_minimum, current_maximum);
                        current_minimum = range.minimum_raw;
                        current_maximum = range.maximum_raw;
                    }
                }
                if (have_range) {
                    support_size +=
                        inclusive_size(current_minimum, current_maximum);
                }
            }
            if (support_size > variable.excluded_raw.size()) continue;

            std::vector<uint64_t> unique;
            unique.reserve(variable.excluded_raw.size());
            for (const uint64_t raw : variable.excluded_raw) {
                const uint64_t ordered = ordered_raw(raw, variable.type);
                if (!distribution_contains(distributions[index], domains[index],
                                           variable.type, ordered) ||
                    std::ranges::find(unique, ordered) != unique.end()) {
                    continue;
                }
                unique.push_back(ordered);
            }
            if (static_cast<uint128_t>(unique.size()) >= support_size) {
                completed_cycles.push_back(index);
            }
        }
        if (!completed_cycles.empty()) {
            std::string message = "random cycle complete for ";
            for (std::size_t index = 0; index < completed_cycles.size(); ++index) {
                if (index != 0) message += ", ";
                message += "'" + std::string{
                                     problem.variables[completed_cycles[index]].name} +
                           "'";
            }
            return RandomizeResult{
                .status = RandomizeStatus::CycleExhausted,
                .cycle_variables = std::move(completed_cycles),
                .message = std::move(message)};
        }
        return RandomizeResult{.status = RandomizeStatus::Solved};
    };

    auto run_phase = [&](bool score_soft) -> RandomizeResult {
        RandomizeValues values(problem.variables.size());
        RandomizeValues best_values;
        bool have_best = false;
        uint64_t excluded_rejections = 0;
        rejection_counts.assign(required_constraints.size(), 0);
        best_soft.assign(soft_constraints.size(), 0);
        candidate_soft.resize(soft_constraints.size());

        for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
            bool excluded = false;
            for (std::size_t index = 0; index < problem.variables.size();
                 ++index) {
                const auto& variable = problem.variables[index];
                const uint64_t ordered = sample_variable(
                    domains[index], distributions[index], variable.type, random);
                values[index] = from_ordered_raw(ordered, variable.type);
                if (std::ranges::find(variable.excluded_raw, values[index]) !=
                    variable.excluded_raw.end()) {
                    excluded = true;
                    ++excluded_rejections;
                    break;
                }
            }
            if (excluded) continue;

            bool valid = true;
            for (std::size_t position = 0;
                 position < required_constraints.size(); ++position) {
                const std::size_t index = required_constraints[position];
                if (!evaluate_constraint(problem.constraints[index].expression,
                                         values.span())) {
                    ++rejection_counts[position];
                    valid = false;
                    break;
                }
            }
            if (!valid) continue;
            if (!score_soft || soft_constraints.empty()) {
                return RandomizeResult{.status = RandomizeStatus::Solved,
                                       .values = std::move(values)};
            }

            bool all_soft = true;
            for (std::size_t index = 0; index < soft_constraints.size(); ++index) {
                candidate_soft[index] = static_cast<uint8_t>(evaluate_constraint(
                    problem.constraints[soft_constraints[index]].expression,
                    values.span()));
                all_soft = all_soft && candidate_soft[index] != 0;
            }
            if (all_soft) {
                return RandomizeResult{.status = RandomizeStatus::Solved,
                                       .values = std::move(values)};
            }

            bool better = !have_best;
            if (have_best) {
                for (std::size_t index = 0; index < candidate_soft.size();
                     ++index) {
                    if (candidate_soft[index] == best_soft[index]) continue;
                    better = candidate_soft[index] > best_soft[index];
                    break;
                }
            }
            if (better) {
                best_values = values;
                best_soft = candidate_soft;
                have_best = true;
            }
        }
        if (have_best) {
            return RandomizeResult{.status = RandomizeStatus::Solved,
                                   .values = std::move(best_values)};
        }

        std::string message = "deterministic candidate search exhausted after " +
                              std::to_string(attempts) + " attempts";
        if (excluded_rejections != 0) {
            message += "; RandC exclusions rejected " +
                       std::to_string(excluded_rejections);
        }
        for (std::size_t position = 0;
             position < required_constraints.size(); ++position) {
            if (rejection_counts[position] == 0) continue;
            message += "; '" +
                       constraint_name(
                           problem.constraints[required_constraints[position]],
                           problem) +
                       "' rejected " + std::to_string(rejection_counts[position]);
        }
        message += "; try Z3RandomBackend for tightly coupled constraints";
        return RandomizeResult{.status = RandomizeStatus::SearchExhausted,
                               .message = std::move(message)};
    };

    if (has_soft) {
        auto configured = configure(true);
        if (configured.status == RandomizeStatus::Solved) {
            auto preferred = run_phase(false);
            if (preferred.status == RandomizeStatus::Solved) return preferred;
        } else if (configured.status == RandomizeStatus::BackendError) {
            return configured;
        }
    }

    auto configured = configure(false);
    if (configured.status != RandomizeStatus::Solved) return configured;
    return run_phase(has_soft);
}

}  // namespace randomize_detail

class RandomSearchBackend final : public ConstraintBackend {
   public:
    explicit RandomSearchBackend(std::size_t attempts = 4096)
        : attempts_(attempts) {
        if (attempts == 0) {
            throw std::invalid_argument(
                "cpptb::RandomSearchBackend attempts must be positive");
        }
    }

    std::string_view name() const noexcept override {
        return "deterministic-search";
    }

    RandomizeResult solve(const ConstraintProblem& problem,
                          Random& random) override {
        auto result =
            randomize_detail::random_search(problem, random, attempts_);
        result.engine = RandomizeEngine::Sampling;
        return result;
    }

   private:
    std::size_t attempts_;
};

class AdaptiveConstraintBackend final : public ConstraintBackend {
   public:
    explicit AdaptiveConstraintBackend(std::size_t sampling_attempts = 4096)
        : sampler_(sampling_attempts) {}

    AdaptiveConstraintBackend(ConstraintBackend& fallback,
                              std::size_t sampling_attempts = 4096)
        : sampler_(sampling_attempts), fallback_(&fallback) {}

    std::string_view name() const noexcept override { return "adaptive"; }

    std::string_view version() const noexcept override {
        return fallback_ ? fallback_->version() : std::string_view{};
    }

    void set_fallback(ConstraintBackend& fallback) noexcept {
        fallback_ = &fallback;
    }

    void clear_fallback() noexcept { fallback_ = nullptr; }
    bool has_fallback() const noexcept { return fallback_ != nullptr; }

    RandomizeResult solve(const ConstraintProblem& problem,
                          Random& random) override {
        auto sampled = sampler_.solve(problem, random);
        if (sampled.status != RandomizeStatus::SearchExhausted) {
            return sampled;
        }
        if (!fallback_) {
            sampled.message +=
                "; adaptive backend has no solver fallback configured";
            return sampled;
        }
        return fallback_->solve(problem, random);
    }

   private:
    RandomSearchBackend sampler_;
    ConstraintBackend* fallback_ = nullptr;
};

inline AdaptiveConstraintBackend& default_adaptive_constraint_backend() {
    static AdaptiveConstraintBackend backend;
    return backend;
}

inline ConstraintBackend& default_constraint_backend() {
    return default_adaptive_constraint_backend();
}

class RandomVariable {
   public:
    virtual ~RandomVariable() = default;

   protected:
    RandomVariable() = default;

   private:
    virtual VariableDescriptor descriptor() const = 0;
    virtual void set_raw(uint64_t raw) = 0;
    virtual void accept_cycle_value(uint64_t) {}
    virtual bool has_cycle_values() const { return false; }
    virtual void reset_cycle() {}

    friend class Randomized;
};

class Randomized;

enum class ConstraintControlKind : uint8_t {
    Expression,
    Distribution,
};

class ConstraintHandle {
   public:
    ConstraintHandle() = default;

    void enable() const;
    void disable() const;
    void set_enabled(bool enabled) const;
    bool enabled() const;
    bool soft() const;

   private:
    ConstraintHandle(Randomized& owner, ConstraintControlKind kind,
                     std::size_t index)
        : owner_(&owner), kind_(kind), index_(index) {}

    Randomized* owner_ = nullptr;
    ConstraintControlKind kind_ = ConstraintControlKind::Expression;
    std::size_t index_ = 0;

    friend class Randomized;
};

class Randomized {
   public:
    Randomized()
        : model_identity_(std::make_shared<ConstraintModelIdentity>()),
          root_(this) {
        problem_.model_identity = model_identity_;
    }
    Randomized(const Randomized&) = delete;
    Randomized& operator=(const Randomized&) = delete;
    Randomized(Randomized&&) = delete;
    Randomized& operator=(Randomized&&) = delete;
    virtual ~Randomized() {
        if (root_ && root_ != this) {
            auto& nested = root_->nested_objects_;
            const auto position = std::ranges::find(nested, this);
            if (position != nested.end()) nested.erase(position);
        }
    }

    ConstraintHandle constraint(Constraint expression,
                                std::string_view label = {}) {
        return add_constraint(std::move(expression), label, false);
    }

    ConstraintHandle constraint(std::string_view label,
                                Constraint expression) {
        return constraint(std::move(expression), label);
    }

    ConstraintHandle soft_constraint(Constraint expression,
                                     std::string_view label = {}) {
        return add_constraint(std::move(expression), label, true);
    }

    ConstraintHandle soft_constraint(std::string_view label,
                                     Constraint expression) {
        return soft_constraint(std::move(expression), label);
    }

    ConstraintHandle constraint(Distribution expression,
                                std::string_view label = {}) {
        auto& root = *root_;
        if (expression.entries().empty()) {
            throw std::invalid_argument(
                "cpptb::Randomized distribution is empty");
        }
        const std::size_t index = root.distributions_.size();
        root.distributions_.push_back(NamedDistribution{
            qualify_label(label), std::move(expression), true});
        root.invalidate_structure();
        return ConstraintHandle{root, ConstraintControlKind::Distribution,
                                index};
    }

    ConstraintHandle constraint(std::string_view label,
                                Distribution expression) {
        return constraint(std::move(expression), label);
    }

    ConstraintHandle distribution(Distribution expression,
                                  std::string_view label = {}) {
        return constraint(std::move(expression), label);
    }

    ConstraintHandle distribution(std::string_view label,
                                  Distribution expression) {
        return constraint(std::move(expression), label);
    }

    virtual void pre_randomize() {}
    virtual void post_randomize() {}

    RandomizeResult randomize(Random& random,
                              ConstraintBackend& backend =
                                  default_constraint_backend()) {
        if (root_ != this) {
            return RandomizeResult{
                .status = RandomizeStatus::BackendError,
                .message = "nested randomized object '" + prefix_ +
                           "' must be randomized through its owning object"};
        }
        return randomize_impl(random, backend, {});
    }

    RandomizeResult randomize_with(
        Random& random, Constraint expression,
        ConstraintBackend& backend = default_constraint_backend()) {
        if (!expression.node()) {
            throw std::invalid_argument(
                "cpptb::Randomized inline constraint expression is empty");
        }
        if (root_ != this) {
            return RandomizeResult{
                .status = RandomizeStatus::BackendError,
                .message = "nested randomized object '" + prefix_ +
                           "' must be randomized through its owning object"};
        }
        const NamedConstraint inline_constraint{
            "inline constraint", std::move(expression), false, true};
        return randomize_impl(random, backend,
                              std::span{&inline_constraint, 1});
    }

   protected:
    explicit Randomized(Randomized& parent, std::string_view name)
        : root_(parent.root_), prefix_(parent.qualify_name(name)) {
        root_->nested_objects_.push_back(this);
    }

   private:
    ConstraintHandle add_constraint(Constraint expression,
                                    std::string_view label, bool soft) {
        if (!expression.node()) {
            throw std::invalid_argument(
                "cpptb::Randomized constraint expression is empty");
        }
        auto& root = *root_;
        const std::size_t index = root.constraints_.size();
        root.constraints_.push_back(NamedConstraint{
            qualify_label(label), std::move(expression), soft, true});
        root.invalidate_structure();
        return ConstraintHandle{root, ConstraintControlKind::Expression,
                                index};
    }

    std::pair<std::size_t, std::string> register_variable(
        RandomVariable& variable, std::string_view name) {
        auto& root = *root_;
        const std::size_t id = root.variables_.size();
        std::string local_name = name.empty()
                                     ? "rand_" + std::to_string(id)
                                     : std::string{name};
        std::string qualified = qualify_name(local_name);
        root.variables_.push_back(&variable);
        root.invalidate_structure();
        return {id, std::move(qualified)};
    }

    std::string qualify_name(std::string_view name) const {
        if (prefix_.empty()) return std::string{name};
        if (name.empty()) return prefix_;
        return prefix_ + "." + std::string{name};
    }

    std::string qualify_label(std::string_view label) const {
        if (label.empty() || prefix_.empty()) return std::string{label};
        return prefix_ + "." + std::string{label};
    }

    void invalidate_structure() {
        auto& root = *root_;
        root.problem_initialized_ = false;
        ++root.control_revision_;
    }

    void set_constraint_enabled(ConstraintControlKind kind, std::size_t index,
                                bool enabled) {
        auto& root = *root_;
        bool* current = nullptr;
        if (kind == ConstraintControlKind::Expression) {
            current = &root.constraints_.at(index).enabled;
        } else {
            current = &root.distributions_.at(index).enabled;
        }
        if (*current == enabled) return;
        *current = enabled;
        ++root.control_revision_;
    }

    bool constraint_enabled(ConstraintControlKind kind,
                            std::size_t index) const {
        const auto& root = *root_;
        return kind == ConstraintControlKind::Expression
                   ? root.constraints_.at(index).enabled
                   : root.distributions_.at(index).enabled;
    }

    bool constraint_soft(ConstraintControlKind kind, std::size_t index) const {
        const auto& root = *root_;
        return kind == ConstraintControlKind::Expression &&
               root.constraints_.at(index).soft;
    }

    ConstraintProblem& prepare_problem(
        std::span<const NamedConstraint> inline_constraints) {
        if (!problem_initialized_) {
            problem_.variables.clear();
            problem_.variables.reserve(variables_.size());
            for (const auto* variable : variables_) {
                problem_.variables.push_back(variable->descriptor());
            }
            problem_initialized_ = true;
        } else {
            // RandC exclusions are the only descriptor state that changes after
            // construction. Reassignment reuses vector capacity in the common
            // case and leaves ordinary Rand descriptors allocation-free.
            for (std::size_t index = 0; index < variables_.size(); ++index) {
                problem_.variables[index] = variables_[index]->descriptor();
            }
        }
        if (problem_control_revision_ != control_revision_) {
            problem_.constraints.clear();
            problem_.constraints.reserve(constraints_.size() + 1);
            for (const auto& constraint : constraints_) {
                if (constraint.enabled) problem_.constraints.push_back(constraint);
            }
            base_constraint_count_ = problem_.constraints.size();

            problem_.distributions.clear();
            problem_.distributions.reserve(distributions_.size());
            for (const auto& distribution : distributions_) {
                if (distribution.enabled) {
                    problem_.distributions.push_back(distribution);
                }
            }
            problem_control_revision_ = control_revision_;
            problem_.model_revision = control_revision_;
        }
        problem_.constraints.resize(base_constraint_count_);
        problem_.constraints.insert(problem_.constraints.end(),
                                    inline_constraints.begin(),
                                    inline_constraints.end());
        problem_.persistent_constraint_count = base_constraint_count_;
        return problem_;
    }

    RandomizeResult randomize_impl(
        Random& random, ConstraintBackend& backend,
        std::span<const NamedConstraint> inline_constraints) {
        pre_randomize();
        for (auto* nested : nested_objects_) nested->pre_randomize();
        auto& problem = prepare_problem(inline_constraints);
        auto result = backend.solve(problem, random);

        if (result.status == RandomizeStatus::CycleExhausted) {
            for (const std::size_t index : result.cycle_variables) {
                if (index >= variables_.size() ||
                    !variables_[index]->has_cycle_values()) {
                    return RandomizeResult{
                        .status = RandomizeStatus::BackendError,
                        .message = "constraint backend '" +
                                   std::string{backend.name()} +
                                   "' reported an invalid RandC cycle"};
                }
                variables_[index]->reset_cycle();
            }
            prepare_problem(inline_constraints);
            result = backend.solve(problem, random);
        }

        if (!result) {
            if (result.message.empty()) {
                result.message = "constraint backend '" +
                                 std::string{backend.name()} + "' failed";
            }
            return result;
        }
        if (result.values.size() != variables_.size()) {
            return RandomizeResult{
                .status = RandomizeStatus::BackendError,
                .message = "constraint backend '" +
                           std::string{backend.name()} +
                           "' returned the wrong assignment size"};
        }
        for (std::size_t index = 0; index < variables_.size(); ++index) {
            variables_[index]->set_raw(result.values[index]);
            variables_[index]->accept_cycle_value(result.values[index]);
        }
        for (auto* nested : nested_objects_) nested->post_randomize();
        post_randomize();
        return result;
    }

    std::vector<RandomVariable*> variables_;
    std::vector<NamedConstraint> constraints_;
    std::vector<NamedDistribution> distributions_;
    std::vector<Randomized*> nested_objects_;
    std::shared_ptr<const ConstraintModelIdentity> model_identity_;
    ConstraintProblem problem_;
    Randomized* root_ = nullptr;
    std::string prefix_;
    std::size_t base_constraint_count_ = 0;
    uint64_t control_revision_ = 1;
    uint64_t problem_control_revision_ = 0;
    bool problem_initialized_ = false;

    template <RandomScalar>
    friend class Rand;
    friend class ConstraintHandle;
};

inline void ConstraintHandle::enable() const { set_enabled(true); }
inline void ConstraintHandle::disable() const { set_enabled(false); }

inline void ConstraintHandle::set_enabled(bool enabled) const {
    if (!owner_) {
        throw std::logic_error("cpptb constraint handle is empty");
    }
    owner_->set_constraint_enabled(kind_, index_, enabled);
}

inline bool ConstraintHandle::enabled() const {
    if (!owner_) return false;
    return owner_->constraint_enabled(kind_, index_);
}

inline bool ConstraintHandle::soft() const {
    if (!owner_) return false;
    return owner_->constraint_soft(kind_, index_);
}

template <RandomScalar Value>
class Rand : public RandomVariable {
   public:
    using value_type = Value;

    explicit Rand(
        Randomized& owner, std::string_view name = {},
        Value minimum = static_cast<Value>(
            std::numeric_limits<randomize_detail::scalar_base_t<Value>>::lowest()),
        Value maximum = static_cast<Value>(
            std::numeric_limits<randomize_detail::scalar_base_t<Value>>::max()),
        std::source_location location = std::source_location::current())
        : minimum_(minimum), maximum_(maximum), name_(name) {
        const auto type = randomize_detail::scalar_type<Value>();
        if (randomize_detail::ordered_raw(randomize_detail::to_raw(minimum),
                                          type) >
            randomize_detail::ordered_raw(randomize_detail::to_raw(maximum),
                                          type)) {
            throw std::invalid_argument(
                "cpptb::Rand minimum exceeds maximum");
        }
        auto registration = owner.register_variable(*this, name_);
        id_ = registration.first;
        name_ = std::move(registration.second);
        source_file_ = location.file_name();
        source_line_ = location.line();
    }

    Rand(const Rand&) = delete;
    Rand& operator=(const Rand&) = delete;
    Rand(Rand&&) = delete;
    Rand& operator=(Rand&&) = delete;

    Value get() const noexcept { return value_; }
    operator Value() const noexcept { return value_; }

    Rand& operator=(Value value) noexcept {
        value_ = value;
        return *this;
    }

    ValueExpr<Value> expression() const {
        auto node = std::make_shared<randomize_detail::ExprNode>();
        node->op = randomize_detail::ExprOp::Variable;
        node->type = randomize_detail::scalar_type<Value>();
        node->variable = id_;
        return ValueExpr<Value>{std::move(node)};
    }

   protected:
    VariableDescriptor descriptor() const override {
        return VariableDescriptor{
            .id = id_,
            .name = name_,
            .type = randomize_detail::scalar_type<Value>(),
            .minimum_raw = randomize_detail::to_raw(minimum_),
            .maximum_raw = randomize_detail::to_raw(maximum_)};
    }

    void set_raw(uint64_t raw) override {
        value_ = randomize_detail::from_raw<Value>(raw);
    }

    std::size_t id_ = 0;
    Value value_{};
    Value minimum_;
    Value maximum_;
    std::string name_;
    std::string source_file_;
    uint32_t source_line_ = 0;
};

template <RandomScalar Value>
class RandC final : public Rand<Value> {
   public:
    using Rand<Value>::Rand;

   private:
    VariableDescriptor descriptor() const override {
        auto result = Rand<Value>::descriptor();
        result.excluded_raw = seen_;
        result.cyclic = true;
        return result;
    }

    void accept_cycle_value(uint64_t raw) override {
        seen_.push_back(randomize_detail::normalize(
            raw, randomize_detail::scalar_type<Value>()));
    }

    bool has_cycle_values() const override { return !seen_.empty(); }
    void reset_cycle() override { seen_.clear(); }

    std::vector<uint64_t> seen_;
};

template <RandomScalar Value, std::size_t Size>
class RandArray {
    static_assert(Size > 0, "cpptb::RandArray size must be greater than zero");

   public:
    using value_type = Value;
    using array_type = std::array<Value, Size>;

    explicit RandArray(
        Randomized& owner, std::string_view name = {},
        Value minimum = static_cast<Value>(
            std::numeric_limits<randomize_detail::scalar_base_t<Value>>::lowest()),
        Value maximum = static_cast<Value>(
            std::numeric_limits<randomize_detail::scalar_base_t<Value>>::max())) {
        for (std::size_t index = 0; index < Size; ++index) {
            std::string element_name = name.empty() ? "array" : std::string{name};
            element_name += "[" + std::to_string(index) + "]";
            elements_.emplace_back(owner, element_name, minimum, maximum);
        }
    }

    static constexpr std::size_t size() noexcept { return Size; }

    Rand<Value>& operator[](std::size_t index) { return elements_.at(index); }
    const Rand<Value>& operator[](std::size_t index) const {
        return elements_.at(index);
    }

    array_type get() const {
        array_type result{};
        for (std::size_t index = 0; index < Size; ++index) {
            result[index] = elements_[index].get();
        }
        return result;
    }

    void set(const array_type& values) noexcept {
        for (std::size_t index = 0; index < Size; ++index) {
            elements_[index] = values[index];
        }
    }

   private:
    std::deque<Rand<Value>> elements_;
};

template <std::size_t Width>
class RandBits {
    static_assert(Width > 0, "cpptb::RandBits width must be greater than zero");

   public:
    using value_type = Bits<Width>;
    static constexpr std::size_t word_count = Bits<Width>::word_count;

    explicit RandBits(Randomized& owner, std::string_view name = {}) {
        for (std::size_t index = 0; index < word_count; ++index) {
            const std::size_t remaining = Width - index * 32;
            const uint32_t maximum =
                remaining >= 32
                    ? std::numeric_limits<uint32_t>::max()
                    : (uint32_t{1} << remaining) - 1;
            std::string word_name = name.empty() ? "bits" : std::string{name};
            word_name += ".word[" + std::to_string(index) + "]";
            words_.emplace_back(owner, word_name, uint32_t{0}, maximum);
        }
    }

    Rand<uint32_t>& word(std::size_t index) { return words_.at(index); }
    const Rand<uint32_t>& word(std::size_t index) const {
        return words_.at(index);
    }

    Bits<Width> get() const {
        typename Bits<Width>::word_array result{};
        for (std::size_t index = 0; index < word_count; ++index) {
            result[index] = words_[index].get();
        }
        return Bits<Width>::from_words(result);
    }

    void set(const Bits<Width>& value) noexcept {
        for (std::size_t index = 0; index < word_count; ++index) {
            words_[index] = value.word(index);
        }
    }

   private:
    std::deque<Rand<uint32_t>> words_;
};

inline std::string format_constraint(
    const Constraint& constraint,
    std::span<const VariableDescriptor> variables) {
    return constraint.node()
               ? randomize_detail::format_node(*constraint.node(), variables)
               : "<empty constraint>";
}

}  // namespace cpptb
