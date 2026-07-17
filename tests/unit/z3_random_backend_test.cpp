#include <cstdint>
#include <iostream>
#include <set>
#include <string_view>

#include "cpptb/z3_random_backend.hpp"

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

class Coupled final : public cpptb::Randomized {
   public:
    cpptb::Rand<uint16_t> left{*this, "left", 0, 1000};
    cpptb::Rand<uint16_t> right{*this, "right", 0, 1000};

    Coupled() { constraint("sum is 1000", left + right == 1000); }
};

class Contradiction final : public cpptb::Randomized {
   public:
    cpptb::Rand<uint8_t> value{*this, "value"};

    Contradiction() {
        constraint("must be zero", value == 0);
        constraint("must be one", value == 1);
    }
};

class CycleValue final : public cpptb::Randomized {
   public:
    cpptb::RandC<uint8_t> value{*this, "value", 0, 3};
};

class SolverFeatures final : public cpptb::Randomized {
   public:
    cpptb::Rand<uint16_t> left{*this, "left", 0, 1000};
    cpptb::Rand<uint16_t> right{*this, "right", 0, 1000};

    SolverFeatures() {
        constraint("selected left",
                   cpptb::inside(left, {uint16_t{100}, uint16_t{200},
                                       uint16_t{300}}));
        distribution(
            "right mix",
            cpptb::dist(
                right, cpptb::weighted(uint16_t{200}, 1),
                cpptb::weighted(
                    cpptb::range(uint16_t{300}, uint16_t{400}), 3)));
        constraint("coupled total", left + right == uint16_t{500});
    }
};

class SoftValue final : public cpptb::Randomized {
   public:
    cpptb::Rand<uint8_t> value{*this, "value"};

    SoftValue() { soft_constraint("default", value == uint8_t{3}); }
};

class SparseCycle final : public cpptb::Randomized {
   public:
    cpptb::RandC<uint8_t> value{*this, "value", 0, 7};

    SparseCycle() {
        constraint("sparse cycle", cpptb::inside(value, {1, 3}));
    }
};

class ControlledCoupled final : public cpptb::Randomized {
   public:
    cpptb::Rand<uint16_t> left{*this, "left", 0, 1000};
    cpptb::Rand<uint16_t> right{*this, "right", 0, 1000};
    cpptb::ConstraintHandle total;

    ControlledCoupled() {
        total = constraint("sum is 1000", left + right == 1000);
    }
};

}  // namespace

int main() {
    cpptb::Random random{42};
    cpptb::Z3RandomBackend solver_only{0};

    Coupled coupled;
    const auto solved = coupled.randomize(random, solver_only);
    check(static_cast<bool>(solved), "Z3 solves a coupled constraint");
    check(static_cast<uint32_t>(coupled.left.get()) + coupled.right.get() ==
              1000,
          "Z3 assignment satisfies the coupled constraint");

    Contradiction contradiction;
    const auto unsat = contradiction.randomize(random, solver_only);
    check(unsat.status == cpptb::RandomizeStatus::Unsatisfiable,
          "Z3 reports an unsatisfiable problem");
    check(unsat.message.find("must be zero") != std::string::npos &&
              unsat.message.find("must be one") != std::string::npos,
          "Z3 diagnostics name the conflicting constraints");

    CycleValue cycle;
    std::set<uint8_t> values;
    for (int index = 0; index < 4; ++index) {
        check(static_cast<bool>(cycle.randomize(random, solver_only)),
              "Z3 solves each RandC assignment");
        values.insert(cycle.value.get());
    }
    check(values.size() == 4, "Z3 honors RandC exclusions");
    check(static_cast<bool>(cycle.randomize(random, solver_only)),
          "Z3 begins a new RandC cycle after exhaustion");

    SolverFeatures features;
    for (int index = 0; index < 20; ++index) {
        check(static_cast<bool>(features.randomize(random, solver_only)),
              "Z3 solves inside, distribution, and coupled constraints");
        check(features.left.get() + features.right.get() == 500,
              "Z3 randomized model satisfies the coupled total");
        check(features.left.get() == 100 || features.left.get() == 200 ||
                  features.left.get() == 300,
              "Z3 honors inside membership");
        check(features.right.get() >= 200 && features.right.get() <= 400,
              "Z3 honors distribution support");
    }

    SoftValue soft;
    check(static_cast<bool>(soft.randomize(random, solver_only)) &&
              soft.value.get() == 3,
          "Z3 prefers a satisfiable soft constraint");
    check(static_cast<bool>(soft.randomize_with(
              random, soft.value == uint8_t{9}, solver_only)) &&
              soft.value.get() == 9,
          "Z3 drops a soft default overridden by an inline constraint");

    cpptb::Random replay_a{0x5555};
    cpptb::Random replay_b{0x5555};
    Coupled coupled_a;
    Coupled coupled_b;
    std::set<uint16_t> diverse_values;
    for (int index = 0; index < 12; ++index) {
        check(static_cast<bool>(coupled_a.randomize(replay_a, solver_only)) &&
                  static_cast<bool>(
                      coupled_b.randomize(replay_b, solver_only)),
              "Z3 replay objects randomize");
        check(coupled_a.left.get() == coupled_b.left.get() &&
                  coupled_a.right.get() == coupled_b.right.get(),
              "Z3 model randomization replays from the same seed");
        diverse_values.insert(coupled_a.left.get());
    }
    check(diverse_values.size() > 1,
          "Z3 model selection varies satisfying assignments");

    SparseCycle sparse;
    std::set<uint8_t> sparse_values;
    for (int index = 0; index < 2; ++index) {
        check(static_cast<bool>(sparse.randomize(random, solver_only)),
              "Z3 solves an inside-constrained RandC field");
        sparse_values.insert(sparse.value.get());
    }
    check(sparse_values == std::set<uint8_t>({1, 3}),
          "Z3 completes the effective sparse RandC domain");
    check(static_cast<bool>(sparse.randomize(random, solver_only)),
          "Z3 starts the sparse RandC cycle again");

    cpptb::Z3RandomBackend cached_solver{0};
    ControlledCoupled cached;
    const auto first_cached = cached.randomize(random, cached_solver);
    const auto second_cached = cached.randomize_with(
        random, cached.left < uint16_t{900}, cached_solver);
    check(static_cast<bool>(first_cached) && static_cast<bool>(second_cached),
          "cached Z3 model handles ordinary and inline solves");
    check(first_cached.engine == cpptb::RandomizeEngine::Solver &&
              second_cached.engine == cpptb::RandomizeEngine::Solver,
          "Z3 reports solver execution");
    check(cached_solver.cache_builds() == 1 &&
              cached_solver.cache_hits() == 1,
          "Z3 reuses its compiled model for inline constraints");

    cached.total.disable();
    check(static_cast<bool>(cached.randomize(random, cached_solver)),
          "Z3 solves after a persistent constraint mode change");
    check(cached_solver.cache_builds() == 2,
          "Z3 rebuilds its model after a persistent constraint change");

    ControlledCoupled independent;
    check(static_cast<bool>(independent.randomize(random, cached_solver)),
          "Z3 solves an independent randomized object");
    check(cached_solver.cache_builds() == 3,
          "Z3 does not share compiled models between randomized objects");

    cached_solver.clear_cache();
    check(cached_solver.cache_builds() == 0 &&
              cached_solver.cache_hits() == 0,
          "Z3 cache reset clears models and counters");

    if (failures != 0) {
        std::cerr << failures << " Z3 randomized test(s) failed\n";
        return 1;
    }
    std::cout << "Z3 randomized tests passed\n";
    return 0;
}
