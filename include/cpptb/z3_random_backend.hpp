// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <z3++.h>

#include "cpptb/randomized.hpp"

namespace cpptb {

namespace z3_random_detail {

inline z3::expr translate_node(
    const randomize_detail::ExprNode& node, z3::context& context,
    const std::vector<z3::expr>& variables) {
    using randomize_detail::ExprOp;
    if (node.op == ExprOp::Constant) {
        return context.bv_val(randomize_detail::normalize(node.raw, node.type),
                              node.type.width);
    }
    if (node.op == ExprOp::Variable) return variables[node.variable];
    if (node.op == ExprOp::LogicalNot) {
        return !translate_node(*node.left, context, variables);
    }
    if (node.op == ExprOp::Inside) {
        const auto value = translate_node(*node.left, context, variables);
        auto result = context.bool_val(false);
        if (!node.ranges) return result;
        for (const auto& range : *node.ranges) {
            const auto minimum = context.bv_val(
                randomize_detail::normalize(range.minimum_raw,
                                             node.left->type),
                node.left->type.width);
            const auto maximum = context.bv_val(
                randomize_detail::normalize(range.maximum_raw,
                                             node.left->type),
                node.left->type.width);
            const auto lower = node.left->type.is_signed
                                   ? value >= minimum
                                   : z3::uge(value, minimum);
            const auto upper = node.left->type.is_signed
                                   ? value <= maximum
                                   : z3::ule(value, maximum);
            result = result || (lower && upper);
        }
        return result;
    }

    const auto left = translate_node(*node.left, context, variables);
    const auto right = translate_node(*node.right, context, variables);
    switch (node.op) {
        case ExprOp::Add:
            return left + right;
        case ExprOp::Subtract:
            return left - right;
        case ExprOp::Multiply:
            return left * right;
        case ExprOp::Remainder:
            return node.type.is_signed ? z3::srem(left, right)
                                       : z3::urem(left, right);
        case ExprOp::Equal:
            return left == right;
        case ExprOp::NotEqual:
            return left != right;
        case ExprOp::Less:
            return node.left->type.is_signed ? left < right
                                             : z3::ult(left, right);
        case ExprOp::LessEqual:
            return node.left->type.is_signed ? left <= right
                                             : z3::ule(left, right);
        case ExprOp::Greater:
            return node.left->type.is_signed ? left > right
                                             : z3::ugt(left, right);
        case ExprOp::GreaterEqual:
            return node.left->type.is_signed ? left >= right
                                             : z3::uge(left, right);
        case ExprOp::LogicalAnd:
            return left && right;
        case ExprOp::LogicalOr:
            return left || right;
        default:
            throw std::logic_error("unsupported constrained-random AST node");
    }
}

inline z3::expr at_least(z3::expr variable, z3::expr bound,
                         bool is_signed) {
    return is_signed ? variable >= bound : z3::uge(variable, bound);
}

inline z3::expr at_most(z3::expr variable, z3::expr bound,
                        bool is_signed) {
    return is_signed ? variable <= bound : z3::ule(variable, bound);
}

}  // namespace z3_random_detail

class Z3RandomBackend final : public ConstraintBackend {
   public:
    // Cheap rejection sampling keeps common constraints on the fast path. Set
    // this to zero to force solver use.
    explicit Z3RandomBackend(std::size_t fast_search_attempts = 128)
        : fast_search_attempts_(fast_search_attempts),
          version_(z3::get_full_version()) {}

    std::string_view name() const noexcept override { return "z3"; }
    std::string_view version() const noexcept override { return version_; }

    std::size_t cache_builds() const noexcept { return cache_builds_; }
    std::size_t cache_hits() const noexcept { return cache_hits_; }

    void clear_cache() noexcept {
        model_cache_.clear();
        cache_builds_ = 0;
        cache_hits_ = 0;
    }

    RandomizeResult solve(const ConstraintProblem& problem,
                          Random& random) override {
        if (fast_search_attempts_ != 0) {
            auto fast = randomize_detail::random_search(
                problem, random, fast_search_attempts_);
            fast.engine = RandomizeEngine::Sampling;
            if (fast.status != RandomizeStatus::SearchExhausted) return fast;
        }

        try {
            auto result = solve_with_z3(problem, random);
            result.engine = RandomizeEngine::Solver;
            return result;
        } catch (const z3::exception& error) {
            return RandomizeResult{
                .status = RandomizeStatus::BackendError,
                .engine = RandomizeEngine::Solver,
                .message = "Z3 constraint backend failed: " +
                           std::string{error.msg()}};
        } catch (const std::exception& error) {
            return RandomizeResult{
                .status = RandomizeStatus::BackendError,
                .engine = RandomizeEngine::Solver,
                .message = "Z3 constraint translation failed: " +
                           std::string{error.what()}};
        }
    }

   private:
    struct SoftExpression {
        std::size_t constraint_index = 0;
        z3::expr expression;
        std::string label;
    };

    struct CachedModel {
        CachedModel(std::shared_ptr<const ConstraintModelIdentity> owner,
                    uint64_t owner_revision)
            : identity(std::move(owner)),
              revision(owner_revision),
              context(std::make_unique<z3::context>()),
              solver(std::make_unique<z3::solver>(*context)) {}

        std::weak_ptr<const ConstraintModelIdentity> identity;
        uint64_t revision = 0;
        std::size_t persistent_constraint_count = 0;
        std::unique_ptr<z3::context> context;
        std::unique_ptr<z3::solver> solver;
        std::vector<z3::expr> variables;
        std::vector<z3::expr> tracking;
        std::vector<std::string> tracking_labels;
        std::vector<std::size_t> hard_indices;
        std::vector<SoftExpression> soft_expressions;
    };

    class SolverFrame {
       public:
        explicit SolverFrame(z3::solver& solver) : solver_(&solver) {
            solver_->push();
        }
        SolverFrame(const SolverFrame&) = delete;
        SolverFrame& operator=(const SolverFrame&) = delete;
        ~SolverFrame() { solver_->pop(); }

       private:
        z3::solver* solver_;
    };

    static std::vector<std::size_t> completed_declared_cycles(
        const ConstraintProblem& problem) {
        std::vector<std::size_t> completed;
        for (std::size_t index = 0; index < problem.variables.size(); ++index) {
            const auto& variable = problem.variables[index];
            if (!variable.cyclic || variable.excluded_raw.empty()) continue;
            const uint64_t minimum = randomize_detail::ordered_raw(
                variable.minimum_raw, variable.type);
            const uint64_t maximum = randomize_detail::ordered_raw(
                variable.maximum_raw, variable.type);
            const uint128_t size =
                randomize_detail::inclusive_size(minimum, maximum);
            if (size > variable.excluded_raw.size()) continue;

            std::vector<uint64_t> unique;
            unique.reserve(variable.excluded_raw.size());
            for (const uint64_t raw : variable.excluded_raw) {
                const uint64_t ordered =
                    randomize_detail::ordered_raw(raw, variable.type);
                if (ordered < minimum || ordered > maximum ||
                    std::ranges::find(unique, ordered) != unique.end()) {
                    continue;
                }
                unique.push_back(ordered);
            }
            if (static_cast<uint128_t>(unique.size()) >= size) {
                completed.push_back(index);
            }
        }
        return completed;
    }

    std::unique_ptr<CachedModel> build_model(
        const ConstraintProblem& problem) {
        auto model = std::make_unique<CachedModel>(problem.model_identity,
                                                   problem.model_revision);
        auto& context = *model->context;
        auto& solver = *model->solver;

        model->variables.reserve(problem.variables.size());
        for (std::size_t index = 0; index < problem.variables.size(); ++index) {
            const auto& descriptor = problem.variables[index];
            const auto symbol = "cpptb_rand_" + std::to_string(index);
            model->variables.push_back(
                context.bv_const(symbol.c_str(), descriptor.type.width));
            const auto minimum = context.bv_val(
                randomize_detail::normalize(descriptor.minimum_raw,
                                             descriptor.type),
                descriptor.type.width);
            const auto maximum = context.bv_val(
                randomize_detail::normalize(descriptor.maximum_raw,
                                             descriptor.type),
                descriptor.type.width);
            solver.add(z3_random_detail::at_least(
                model->variables.back(), minimum, descriptor.type.is_signed));
            solver.add(z3_random_detail::at_most(
                model->variables.back(), maximum, descriptor.type.is_signed));
        }

        std::vector<bool> distributed(problem.variables.size(), false);
        for (std::size_t index = 0; index < problem.distributions.size();
             ++index) {
            const auto& distribution = problem.distributions[index];
            const std::size_t variable = distribution.expression.variable();
            if (variable >= model->variables.size()) {
                throw std::invalid_argument(
                    "Z3 distribution references an unknown variable");
            }
            if (distributed[variable]) {
                throw std::invalid_argument(
                    "multiple active distributions target '" +
                    std::string{problem.variables[variable].name} + "'");
            }
            distributed[variable] = true;

            auto supported = context.bool_val(false);
            for (const auto& entry : distribution.expression.entries()) {
                if (entry.weight == 0) continue;
                const auto minimum = context.bv_val(
                    randomize_detail::normalize(
                        entry.range.minimum_raw,
                        problem.variables[variable].type),
                    problem.variables[variable].type.width);
                const auto maximum = context.bv_val(
                    randomize_detail::normalize(
                        entry.range.maximum_raw,
                        problem.variables[variable].type),
                    problem.variables[variable].type.width);
                supported =
                    supported ||
                    (z3_random_detail::at_least(
                         model->variables[variable], minimum,
                         problem.variables[variable].type.is_signed) &&
                     z3_random_detail::at_most(
                         model->variables[variable], maximum,
                         problem.variables[variable].type.is_signed));
            }
            const auto marker_name =
                "cpptb_distribution_" + std::to_string(index);
            model->tracking.push_back(
                context.bool_const(marker_name.c_str()));
            model->tracking_labels.push_back(
                distribution.label.empty()
                    ? "distribution for " +
                          std::string{problem.variables[variable].name}
                    : distribution.label);
            solver.add(supported, model->tracking.back());
        }

        model->persistent_constraint_count =
            problem.model_identity
                ? std::min(problem.persistent_constraint_count,
                           problem.constraints.size())
                : problem.constraints.size();
        for (std::size_t index = 0;
             index < model->persistent_constraint_count; ++index) {
            const auto& constraint = problem.constraints[index];
            if (!constraint.expression.node()) {
                throw std::invalid_argument(
                    "Z3 received an empty constraint expression");
            }
            auto expression = z3_random_detail::translate_node(
                *constraint.expression.node(), context, model->variables);
            const std::string label =
                constraint.label.empty()
                    ? format_constraint(constraint.expression,
                                        problem.variables)
                    : constraint.label;
            if (constraint.soft) {
                model->soft_expressions.push_back(
                    SoftExpression{index, std::move(expression), label});
                continue;
            }
            model->hard_indices.push_back(index);
            const auto marker_name =
                "cpptb_constraint_" + std::to_string(index);
            model->tracking.push_back(
                context.bool_const(marker_name.c_str()));
            model->tracking_labels.push_back(label);
            solver.add(expression, model->tracking.back());
        }
        ++cache_builds_;
        return model;
    }

    CachedModel& cached_model(const ConstraintProblem& problem) {
        for (auto iterator = model_cache_.begin();
             iterator != model_cache_.end();) {
            auto owner = (*iterator)->identity.lock();
            if (!owner) {
                iterator = model_cache_.erase(iterator);
                continue;
            }
            if (owner == problem.model_identity) {
                if ((*iterator)->revision == problem.model_revision) {
                    ++cache_hits_;
                    return **iterator;
                }
                iterator = model_cache_.erase(iterator);
                continue;
            }
            ++iterator;
        }
        model_cache_.push_back(build_model(problem));
        return *model_cache_.back();
    }

    RandomizeResult solve_with_z3(const ConstraintProblem& problem,
                                  Random& random) {
        auto completed_cycles = completed_declared_cycles(problem);
        if (!completed_cycles.empty()) {
            return RandomizeResult{
                .status = RandomizeStatus::CycleExhausted,
                .cycle_variables = std::move(completed_cycles),
                .message = "one or more declared RandC domains are complete"};
        }

        std::unique_ptr<CachedModel> transient;
        CachedModel* model = nullptr;
        if (problem.model_identity) {
            model = &cached_model(problem);
        } else {
            transient = build_model(problem);
            model = transient.get();
        }
        auto& context = *model->context;
        auto& solver = *model->solver;
        z3::params parameters{context};
        parameters.set("random_seed",
                       static_cast<unsigned>(random.next_u64()));
        solver.set(parameters);
        SolverFrame frame{solver};

        std::vector<z3::expr> cycle_tracking;
        std::vector<std::size_t> cycle_tracking_variables;

        for (std::size_t index = 0; index < problem.variables.size(); ++index) {
            const auto& descriptor = problem.variables[index];
            auto exclusions = context.bool_val(true);
            for (const uint64_t raw : descriptor.excluded_raw) {
                exclusions =
                    exclusions &&
                    model->variables[index] != context.bv_val(
                                                   randomize_detail::normalize(
                                                       raw, descriptor.type),
                                                   descriptor.type.width);
            }
            if (!descriptor.excluded_raw.empty() && descriptor.cyclic) {
                const auto marker_name =
                    "cpptb_cycle_" + std::to_string(index);
                cycle_tracking.push_back(
                    context.bool_const(marker_name.c_str()));
                cycle_tracking_variables.push_back(index);
                solver.add(exclusions, cycle_tracking.back());
            } else if (!descriptor.excluded_raw.empty()) {
                solver.add(exclusions);
            }
        }

        std::vector<const NamedDistribution*> distributions(
            problem.variables.size(), nullptr);
        for (const auto& distribution : problem.distributions) {
            const std::size_t variable = distribution.expression.variable();
            distributions[variable] = &distribution;
        }

        std::vector<z3::expr> dynamic_tracking;
        std::vector<std::string> dynamic_tracking_labels;
        std::vector<std::size_t> dynamic_hard_indices;
        std::vector<SoftExpression> dynamic_soft;
        for (std::size_t index = model->persistent_constraint_count;
             index < problem.constraints.size(); ++index) {
            const auto& constraint = problem.constraints[index];
            if (!constraint.expression.node()) {
                return RandomizeResult{
                    .status = RandomizeStatus::BackendError,
                    .message = "Z3 received an empty constraint expression"};
            }
            auto expression = z3_random_detail::translate_node(
                *constraint.expression.node(), context, model->variables);
            const std::string label =
                constraint.label.empty()
                    ? format_constraint(constraint.expression,
                                        problem.variables)
                    : constraint.label;
            if (constraint.soft) {
                dynamic_soft.push_back(
                    SoftExpression{index, std::move(expression), label});
                continue;
            }
            dynamic_hard_indices.push_back(index);
            const auto marker_name =
                "cpptb_constraint_" + std::to_string(index);
            dynamic_tracking.push_back(
                context.bool_const(marker_name.c_str()));
            dynamic_tracking_labels.push_back(label);
            solver.add(expression, dynamic_tracking.back());
        }

        const auto status = solver.check();
        if (status == z3::unsat) {
            const auto core = solver.unsat_core();
            std::vector<std::size_t> exhausted_cycles;
            for (unsigned core_index = 0; core_index < core.size();
                 ++core_index) {
                for (std::size_t index = 0; index < cycle_tracking.size();
                     ++index) {
                    if (Z3_is_eq_ast(context, core[core_index],
                                     cycle_tracking[index])) {
                        exhausted_cycles.push_back(
                            cycle_tracking_variables[index]);
                    }
                }
            }
            if (!exhausted_cycles.empty()) {
                return RandomizeResult{
                    .status = RandomizeStatus::CycleExhausted,
                    .cycle_variables = std::move(exhausted_cycles),
                    .message = "Z3 identified exhausted RandC exclusions"};
            }

            std::string message = "constraints are unsatisfiable";
            bool first = true;
            for (unsigned core_index = 0; core_index < core.size();
                 ++core_index) {
                for (std::size_t index = 0; index < model->tracking.size();
                     ++index) {
                    if (!Z3_is_eq_ast(context, core[core_index],
                                      model->tracking[index])) {
                        continue;
                    }
                    message += first ? ": " : ", ";
                    first = false;
                    message += model->tracking_labels[index];
                    break;
                }
                for (std::size_t index = 0;
                     index < dynamic_tracking.size(); ++index) {
                    if (!Z3_is_eq_ast(context, core[core_index],
                                      dynamic_tracking[index])) {
                        continue;
                    }
                    message += first ? ": " : ", ";
                    first = false;
                    message += dynamic_tracking_labels[index];
                    break;
                }
            }
            return RandomizeResult{.status = RandomizeStatus::Unsatisfiable,
                                   .message = std::move(message)};
        }
        if (status == z3::unknown) {
            return RandomizeResult{
                .status = RandomizeStatus::BackendError,
                .message = "Z3 returned unknown: " + solver.reason_unknown()};
        }

        std::vector<std::size_t> accepted_soft;
        const auto apply_soft = [&](const SoftExpression& soft) {
            solver.push();
            solver.add(soft.expression);
            const auto soft_status = solver.check();
            solver.pop();
            if (soft_status == z3::sat) {
                solver.add(soft.expression);
                accepted_soft.push_back(soft.constraint_index);
            } else if (soft_status == z3::unknown) {
                throw std::runtime_error(
                    "Z3 returned unknown while applying soft '" + soft.label +
                    "': " + solver.reason_unknown());
            }
        };
        for (const auto& soft : model->soft_expressions) apply_soft(soft);
        for (const auto& soft : dynamic_soft) apply_soft(soft);

        std::vector<randomize_detail::CandidateDomain> sampling_domains(
            problem.variables.size());
        for (std::size_t index = 0; index < problem.variables.size(); ++index) {
            sampling_domains[index].low_ordered =
                randomize_detail::ordered_raw(
                    problem.variables[index].minimum_raw,
                    problem.variables[index].type);
            sampling_domains[index].high_ordered =
                randomize_detail::ordered_raw(
                    problem.variables[index].maximum_raw,
                    problem.variables[index].type);
        }
        for (const std::size_t index : model->hard_indices) {
            randomize_detail::tighten_domain(
                *problem.constraints[index].expression.node(),
                sampling_domains);
        }
        for (const std::size_t index : dynamic_hard_indices) {
            randomize_detail::tighten_domain(
                *problem.constraints[index].expression.node(),
                sampling_domains);
        }
        for (const std::size_t index : accepted_soft) {
            randomize_detail::tighten_domain(
                *problem.constraints[index].expression.node(),
                sampling_domains);
        }
        for (auto& domain : sampling_domains) {
            randomize_detail::finalize_domain(domain);
        }

        for (std::size_t index = 0; index < model->variables.size(); ++index) {
            if (sampling_domains[index].ranges.empty()) continue;
            for (int candidate_index = 0; candidate_index < 4;
                 ++candidate_index) {
                const uint64_t ordered = randomize_detail::sample_variable(
                    sampling_domains[index], distributions[index],
                    problem.variables[index].type, random);
                const uint64_t raw = randomize_detail::from_ordered_raw(
                    ordered, problem.variables[index].type);
                if (std::ranges::find(
                        problem.variables[index].excluded_raw, raw) !=
                    problem.variables[index].excluded_raw.end()) {
                    continue;
                }
                const auto candidate = context.bv_val(
                    randomize_detail::normalize(
                        raw, problem.variables[index].type),
                    problem.variables[index].type.width);
                solver.push();
                solver.add(model->variables[index] == candidate);
                const auto candidate_status = solver.check();
                solver.pop();
                if (candidate_status == z3::sat) {
                    solver.add(model->variables[index] == candidate);
                    break;
                }
            }
        }

        const auto final_status = solver.check();
        if (final_status != z3::sat) {
            return RandomizeResult{
                .status = RandomizeStatus::BackendError,
                .message = "Z3 randomized model selection did not remain "
                           "satisfiable"};
        }

        const auto solution = solver.get_model();
        RandomizeValues values;
        values.reserve(model->variables.size());
        for (const auto& variable : model->variables) {
            const auto value = solution.eval(variable, true);
            uint64_t raw = 0;
            if (!value.is_numeral_u64(raw)) {
                return RandomizeResult{
                    .status = RandomizeStatus::BackendError,
                    .message = "Z3 returned a non-numeral assignment"};
            }
            values.push_back(raw);
        }
        return RandomizeResult{.status = RandomizeStatus::Solved,
                               .values = std::move(values)};
    }

    std::size_t fast_search_attempts_;
    std::string version_;
    std::vector<std::unique_ptr<CachedModel>> model_cache_;
    std::size_t cache_builds_ = 0;
    std::size_t cache_hits_ = 0;
};

}  // namespace cpptb
