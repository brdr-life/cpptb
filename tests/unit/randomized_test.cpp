#include <array>
#include <cstdint>
#include <iostream>
#include <set>
#include <string_view>

#include "cpptb/randomized.hpp"

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

enum class Kind : uint8_t { Read, Write, Atomic, Reserved };

class Packet final : public cpptb::Randomized {
   public:
    cpptb::Rand<Kind> kind{*this, "kind"};
    cpptb::Rand<uint16_t> length{*this, "length"};
    cpptb::Rand<uint16_t> address{*this, "address", 0x1000, 0x1fff};

    Packet() {
        constraint("supported kind", kind < Kind::Reserved);
        constraint("legal length", length >= 64 && length <= 1500);
        constraint("word alignment", length % 4 == 0);
        constraint("small atomic packet",
                   kind != Kind::Atomic || length <= 256);
    }

    void pre_randomize() override { ++pre_calls; }
    void post_randomize() override { ++post_calls; }

    int pre_calls = 0;
    int post_calls = 0;
};

class CycleValue final : public cpptb::Randomized {
   public:
    cpptb::RandC<uint8_t> value{*this, "value", 0, 3};
};

class Impossible final : public cpptb::Randomized {
   public:
    cpptb::Rand<uint8_t> value{*this, "value"};

    Impossible() {
        constraint("too small", value < 4);
        constraint("too large", value > 7);
    }
};

class SignedValue final : public cpptb::Randomized {
   public:
    cpptb::Rand<int8_t> value{*this, "value"};

    SignedValue() {
        constraint(value >= int8_t{-20} && value <= int8_t{-1});
        constraint(value % int8_t{2} == int8_t{0});
    }
};

class ManyFields final : public cpptb::Randomized {
   public:
    cpptb::Rand<uint8_t> value0{*this, "value0"};
    cpptb::Rand<uint8_t> value1{*this, "value1"};
    cpptb::Rand<uint8_t> value2{*this, "value2"};
    cpptb::Rand<uint8_t> value3{*this, "value3"};
    cpptb::Rand<uint8_t> value4{*this, "value4"};
    cpptb::Rand<uint8_t> value5{*this, "value5"};
    cpptb::Rand<uint8_t> value6{*this, "value6"};
    cpptb::Rand<uint8_t> value7{*this, "value7"};
    cpptb::Rand<uint8_t> value8{*this, "value8"};
};

class DomainItem final : public cpptb::Randomized {
   public:
    cpptb::Rand<uint8_t> opcode{*this, "opcode"};
    cpptb::Rand<uint16_t> length{*this, "length"};

    DomainItem() {
        constraint("supported opcodes", cpptb::inside(opcode, {1, 3, 5}));
        distribution(
            "packet length mix",
            cpptb::dist(
                length, cpptb::weighted(uint16_t{64}, 1),
                cpptb::weighted(
                    cpptb::range(uint16_t{128}, uint16_t{131}), 3),
                cpptb::weighted(uint16_t{999}, 0)));
    }
};

class ControlledItem final : public cpptb::Randomized {
   public:
    cpptb::Rand<uint8_t> value{*this, "value"};
    cpptb::ConstraintHandle legal;
    cpptb::ConstraintHandle preferred;

    ControlledItem() {
        legal = constraint("small value", value < uint8_t{4});
        preferred = soft_constraint("default value", value == uint8_t{3});
    }
};

class NestedHeader final : public cpptb::Randomized {
   public:
    cpptb::Rand<uint8_t> kind{*this, "kind"};
    cpptb::Rand<uint8_t> route{*this, "route"};
    int pre_calls = 0;
    int post_calls = 0;

    explicit NestedHeader(cpptb::Randomized& parent)
        : Randomized(parent, "header") {
        constraint("legal kind", cpptb::inside(kind, {1, 2, 4}));
        soft_constraint("default route", route == uint8_t{2});
    }

    void pre_randomize() override { ++pre_calls; }
    void post_randomize() override { ++post_calls; }
};

class CompositeItem final : public cpptb::Randomized {
   public:
    NestedHeader header{*this};
    cpptb::RandArray<uint8_t, 4> bytes{*this, "bytes"};
    cpptb::RandBits<65> payload{*this, "payload"};

    CompositeItem() {
        constraint("distinct prefix", bytes[0] != bytes[1]);
        constraint("high payload bit", payload.word(2) == uint32_t{1});
    }
};

class DualCycle final : public cpptb::Randomized {
   public:
    cpptb::RandC<uint8_t> short_cycle{*this, "short_cycle", 0, 1};
    cpptb::RandC<uint8_t> long_cycle{*this, "long_cycle", 0, 3};
};

class SparseCycle final : public cpptb::Randomized {
   public:
    cpptb::RandC<uint8_t> value{*this, "value", 0, 7};

    SparseCycle() {
        constraint("sparse cycle", cpptb::inside(value, {1, 3}));
    }
};

class SearchDiagnostic final : public cpptb::Randomized {
   public:
    cpptb::Rand<uint8_t> left{*this, "left", 0, 10};
    cpptb::Rand<uint8_t> right{*this, "right", 0, 10};

    SearchDiagnostic() {
        constraint("impossible product",
                   left * right == uint8_t{251});
    }
};

class DuplicateDistribution final : public cpptb::Randomized {
   public:
    cpptb::Rand<uint8_t> value{*this, "value"};
    cpptb::ConstraintHandle second;

    DuplicateDistribution() {
        distribution("first", cpptb::dist(
                                  value, cpptb::weighted(uint8_t{1}, 1)));
        second = distribution(
            "second",
            cpptb::dist(value, cpptb::weighted(uint8_t{2}, 1)));
    }
};

class RecordingFallback final : public cpptb::ConstraintBackend {
   public:
    std::string_view name() const noexcept override { return "recording"; }
    std::string_view version() const noexcept override { return "test-1"; }

    cpptb::RandomizeResult solve(const cpptb::ConstraintProblem& problem,
                                 cpptb::Random&) override {
        ++calls;
        cpptb::RandomizeValues values(problem.variables.size());
        return {.status = cpptb::RandomizeStatus::Solved,
                .engine = cpptb::RandomizeEngine::Solver,
                .values = std::move(values)};
    }

    int calls = 0;
};

void test_constraints_and_replay() {
    cpptb::Random first_random{0x1234};
    cpptb::Random second_random{0x1234};
    Packet first;
    Packet second;

    for (int index = 0; index < 100; ++index) {
        const auto first_result = first.randomize(first_random);
        const auto second_result = second.randomize(second_random);
        check(static_cast<bool>(first_result), "packet randomization succeeds");
        check(static_cast<bool>(second_result), "replay randomization succeeds");
        check(first.kind.get() == second.kind.get(), "kind replays");
        check(first.length.get() == second.length.get(), "length replays");
        check(first.address.get() == second.address.get(), "address replays");
        check(first.kind.get() < Kind::Reserved, "kind constraint holds");
        check(first.length.get() >= 64 && first.length.get() <= 1500,
              "length range holds");
        check(first.length.get() % 4 == 0, "alignment constraint holds");
        check(first.kind.get() != Kind::Atomic || first.length.get() <= 256,
              "relational constraint holds");
    }
    check(first.pre_calls == 100 && first.post_calls == 100,
          "randomize hooks run once per successful call");
}

void test_inline_constraint() {
    cpptb::Random random{99};
    Packet packet;
    const auto result = packet.randomize_with(random, packet.length == 256);
    check(static_cast<bool>(result), "inline randomization succeeds");
    check(packet.length.get() == 256, "inline constraint is applied");

    bool rejected_empty = false;
    try {
        static_cast<void>(
            packet.randomize_with(random, cpptb::Constraint{}));
    } catch (const std::invalid_argument&) {
        rejected_empty = true;
    }
    check(rejected_empty, "empty inline constraints are rejected");
}

void test_assignment_storage_resize() {
    cpptb::RandomizeValues values;
    for (uint64_t value = 1;
         value <= cpptb::RandomizeValues::inline_capacity; ++value) {
        values.push_back(value);
    }

    values.resize(cpptb::RandomizeValues::inline_capacity + 1);
    check(values[0] == 1 &&
              values[cpptb::RandomizeValues::inline_capacity - 1] ==
                  cpptb::RandomizeValues::inline_capacity,
          "growing into overflow storage preserves inline assignments");

    values.resize(4);
    check(values[0] == 1 && values[3] == 4,
          "shrinking into inline storage preserves assignments");
    values.resize(6);
    check(values[4] == 0 && values[5] == 0,
          "new inline assignments are value initialized");
}

void test_unsatisfiable_domain() {
    cpptb::Random random{1};
    Impossible value;
    const auto result = value.randomize(random);
    check(result.status == cpptb::RandomizeStatus::Unsatisfiable,
          "contradictory bounds report unsatisfiable");
    check(result.message.find("empty") != std::string::npos,
          "unsatisfiable result explains the empty domain");
}

void test_randc_cycle() {
    cpptb::Random random{7};
    CycleValue item;
    std::set<uint8_t> first_cycle;
    for (int index = 0; index < 4; ++index) {
        const auto result = item.randomize(random);
        check(static_cast<bool>(result), "RandC value randomizes");
        first_cycle.insert(item.value.get());
    }
    check(first_cycle.size() == 4, "RandC visits every value once");
    check(static_cast<bool>(item.randomize(random)),
          "RandC automatically begins its next cycle");
}

void test_diagnostics() {
    Packet packet;
    cpptb::ConstraintProblem problem;
    problem.variables = {
        {.id = 0, .name = "kind"},
        {.id = 1, .name = "length"},
        {.id = 2, .name = "address"},
    };
    const auto text = cpptb::format_constraint(packet.length % 4 == 0,
                                                problem.variables);
    check(text == "((length % 4) == 0)",
          "constraint formatting retains names and operations");
}

void test_signed_and_large_assignments() {
    cpptb::Random random{0xface};
    SignedValue signed_value;
    for (int index = 0; index < 20; ++index) {
        check(static_cast<bool>(signed_value.randomize(random)),
              "signed value randomizes");
        check(signed_value.value.get() >= -20 &&
                  signed_value.value.get() <= -1 &&
                  signed_value.value.get() % 2 == 0,
              "signed constraints hold");
    }

    ManyFields many;
    check(static_cast<bool>(many.randomize(random)),
          "more than eight fields use the overflow assignment path");
}

void test_inside_and_distribution() {
    cpptb::Random random{0x1234};
    DomainItem item;
    std::set<uint8_t> opcodes;
    int short_packets = 0;
    int ranged_packets = 0;
    for (int index = 0; index < 1000; ++index) {
        check(static_cast<bool>(item.randomize(random)),
              "inside and distribution randomization succeeds");
        opcodes.insert(item.opcode.get());
        if (item.length.get() == 64) {
            ++short_packets;
        } else if (item.length.get() >= 128 && item.length.get() <= 131) {
            ++ranged_packets;
        } else {
            check(false, "distribution never returns unsupported values");
        }
    }
    check(opcodes == std::set<uint8_t>({1, 3, 5}),
          "inside samples each listed value");
    check(short_packets > 150 && short_packets < 350 &&
              ranged_packets == 1000 - short_packets,
          "distribution applies total entry weights");
}

void test_constraint_controls_and_soft_defaults() {
    cpptb::Random random{33};
    ControlledItem item;
    check(item.legal.enabled() && item.preferred.enabled() &&
              item.preferred.soft(),
          "constraint handles expose mode and soft metadata");
    check(static_cast<bool>(item.randomize(random)),
          "soft default randomizes");
    check(item.value.get() == 3, "soft default is preferred");

    const auto conflict = item.randomize_with(random, item.value == uint8_t{9});
    check(conflict.status == cpptb::RandomizeStatus::Unsatisfiable,
          "enabled hard constraint rejects an inline conflict");

    item.legal.disable();
    check(!item.legal.enabled(), "constraint handle disables its constraint");
    check(static_cast<bool>(
              item.randomize_with(random, item.value == uint8_t{9})),
          "inline hard constraint overrides a conflicting soft default");
    check(item.value.get() == 9, "soft override assigns the inline value");
    item.legal.enable();
}

void test_composite_fields_and_nested_objects() {
    cpptb::Random random{0xbeef};
    CompositeItem item;
    check(static_cast<bool>(item.randomize(random)),
          "nested, array, and wide fields randomize together");
    check(item.header.kind.get() == 1 || item.header.kind.get() == 2 ||
              item.header.kind.get() == 4,
          "nested object constraint holds");
    check(item.header.route.get() == 2, "nested soft default holds");
    check(item.bytes.get()[0] != item.bytes.get()[1],
          "array element constraint holds");
    check(item.payload.get().bit(64), "wide randomized payload sets bit 64");
    check(item.header.pre_calls == 1 && item.header.post_calls == 1,
          "owning randomization invokes nested hooks");

    const auto nested_result = item.header.randomize(random);
    check(nested_result.status == cpptb::RandomizeStatus::BackendError &&
              nested_result.message.find("owning object") != std::string::npos,
          "nested objects reject independent randomization clearly");
}

void test_independent_and_sparse_randc_cycles() {
    cpptb::Random random{71};
    DualCycle cycles;
    std::set<uint8_t> long_values;
    for (int index = 0; index < 4; ++index) {
        check(static_cast<bool>(cycles.randomize(random)),
              "independent RandC fields randomize");
        long_values.insert(cycles.long_cycle.get());
    }
    check(long_values.size() == 4,
          "short RandC rollover does not reset the longer cycle");

    SparseCycle sparse;
    std::set<uint8_t> first_cycle;
    for (int index = 0; index < 2; ++index) {
        check(static_cast<bool>(sparse.randomize(random)),
              "inside-constrained RandC field randomizes");
        first_cycle.insert(sparse.value.get());
    }
    check(first_cycle == std::set<uint8_t>({1, 3}),
          "RandC cycle uses the effective inside domain");
    check(static_cast<bool>(sparse.randomize(random)),
          "sparse RandC automatically begins another cycle");
}

void test_search_diagnostics_and_distribution_modes() {
    cpptb::Random random{9};
    cpptb::RandomSearchBackend short_search{16};
    SearchDiagnostic impossible;
    const auto result = impossible.randomize(random, short_search);
    check(result.status == cpptb::RandomizeStatus::SearchExhausted,
          "search distinguishes exhaustion from proven unsatisfiable");
    check(result.message.find("impossible product") != std::string::npos &&
              result.message.find("rejected 16") != std::string::npos &&
              result.message.find("Z3RandomBackend") != std::string::npos,
          "search exhaustion names rejection counts and the solver remedy");

    DuplicateDistribution duplicate;
    const auto duplicate_result = duplicate.randomize(random);
    check(duplicate_result.status == cpptb::RandomizeStatus::BackendError &&
              duplicate_result.message.find("multiple active distributions") !=
                  std::string::npos,
          "duplicate distributions produce an actionable error");
    duplicate.second.disable();
    check(static_cast<bool>(duplicate.randomize(random)) &&
              duplicate.value.get() == 1,
          "distribution handles support mode changes");
}

void test_adaptive_backend_policy() {
    cpptb::Random random{17};
    RecordingFallback fallback;
    cpptb::AdaptiveConstraintBackend adaptive{fallback, 16};

    Packet packet;
    const auto sampled = packet.randomize(random, adaptive);
    check(static_cast<bool>(sampled) &&
              sampled.engine == cpptb::RandomizeEngine::Sampling,
          "adaptive backend reports its sampling fast path");
    check(fallback.calls == 0,
          "adaptive backend does not invoke fallback for an easy problem");
    check(adaptive.name() == "adaptive" && adaptive.version() == "test-1",
          "adaptive backend reports its policy and fallback version");

    SearchDiagnostic sparse;
    const auto solved = sparse.randomize(random, adaptive);
    check(static_cast<bool>(solved) &&
              solved.engine == cpptb::RandomizeEngine::Solver,
          "adaptive backend reports its solver fallback path");
    check(fallback.calls == 1,
          "adaptive backend invokes fallback after sampling exhaustion");

    adaptive.clear_fallback();
    const auto exhausted = sparse.randomize(random, adaptive);
    check(exhausted.status == cpptb::RandomizeStatus::SearchExhausted &&
              exhausted.message.find("no solver fallback configured") !=
                  std::string::npos,
          "adaptive backend explains how sparse constraints failed");
    check(!adaptive.has_fallback(),
          "adaptive backend exposes whether fallback is configured");
}

}  // namespace

int main() {
    test_constraints_and_replay();
    test_inline_constraint();
    test_assignment_storage_resize();
    test_unsatisfiable_domain();
    test_randc_cycle();
    test_diagnostics();
    test_signed_and_large_assignments();
    test_inside_and_distribution();
    test_constraint_controls_and_soft_defaults();
    test_composite_fields_and_nested_objects();
    test_independent_and_sparse_randc_cycles();
    test_search_diagnostics_and_distribution_modes();
    test_adaptive_backend_policy();
    if (failures != 0) {
        std::cerr << failures << " randomized test(s) failed\n";
        return 1;
    }
    std::cout << "randomized tests passed\n";
    return 0;
}
