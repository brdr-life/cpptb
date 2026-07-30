// Checks the covergroup constructs cpptb grew so that Ibex's functional
// coverage could be expressed: wildcard bins, value-list bins, three-way
// crosses, cross bin selection with binsof/intersect and its && and ||,
// sampling guards, and `illegal_bins = default sequence`.
//
// Every one of these is a construct Verilator 5.050 either ignores with a
// COVERIGN warning or hard-errors on, so there is no reference simulator here
// to compare against. The expectations below come from IEEE 1800-2023 clause
// 19, and each test names the rule it is checking.

#include <cstdint>
#include <iostream>
#include <string>

#include "cpptb/coverage.hpp"

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << "\n";
        ++failures;
    }
}

struct Sample {
    uint32_t irq = 0;
    uint32_t priv = 0;
    uint32_t category = 0;
    bool enabled = true;
};

const cpptb::CoverageBinSnapshot& find_bin(
    const cpptb::CoveragePointSnapshot& point, const std::string& name) {
    for (const auto& bin : point.bins) {
        if (bin.name == name) return bin;
    }
    throw std::runtime_error("no bin " + name);
}

uint64_t cross_hits(const cpptb::CoverageCrossSnapshot& cross,
                    const std::vector<std::string>& bins) {
    for (const auto& bin : cross.bins) {
        if (bin.bins == bins) return bin.hits;
    }
    throw std::runtime_error("no such cross bin");
}

bool has_cross_bin(const cpptb::CoverageCrossSnapshot& cross,
                   const std::vector<std::string>& bins) {
    for (const auto& bin : cross.bins) {
        if (bin.bins == bins) return true;
    }
    return false;
}

// 19.5.1: a wildcard bin's `?` positions do not take part in the comparison.
void wildcard_bins() {
    cpptb::Covergroup<Sample> group("wildcard");
    auto& irq = group.coverpoint("cp_irq", [](const Sample& s) { return s.irq; });
    irq.wildcard_bin("nmi_external", "1?????");
    irq.wildcard_bin("nmi_internal", "01????");
    irq.wildcard_bin("irq_timer", "000001");

    group.sample({.irq = 0b100000});  // nmi_external
    group.sample({.irq = 0b111111});  // nmi_external again
    group.sample({.irq = 0b010101});  // nmi_internal
    group.sample({.irq = 0b000001});  // irq_timer

    const auto snapshot = group.snapshot();
    check(find_bin(snapshot.points[0], "nmi_external").hits == 2,
          "a wildcard bin ignores its don't-care positions");
    check(find_bin(snapshot.points[0], "nmi_internal").hits == 1,
          "wildcard bins discriminate on their care bits");
    check(find_bin(snapshot.points[0], "irq_timer").hits == 1,
          "a fully specified wildcard bin matches exactly one value");
}

// 19.5: a bin declared over a value list counts once per sample, however many
// of its values match -- it is one bin, not several.
void value_list_bins() {
    cpptb::Covergroup<Sample> group("lists");
    using Point = cpptb::Coverpoint<uint32_t>;
    auto& category =
        group.coverpoint("cp_cat", [](const Sample& s) { return s.category; });
    category.bin("jumps", {Point::Range{2}, Point::Range{5, 7}});
    category.bin("other", {Point::Range{0}, Point::Range{1}});

    group.sample({.category = 2});
    group.sample({.category = 6});
    group.sample({.category = 1});

    const auto snapshot = group.snapshot();
    check(find_bin(snapshot.points[0], "jumps").hits == 2,
          "a value-list bin accepts every value in its list");
    check(find_bin(snapshot.points[0], "other").hits == 1,
          "value-list bins stay distinct");
}

// 19.6: a cross of three coverpoints has one bin per combination.
void three_way_cross() {
    cpptb::Covergroup<Sample> group("three");
    auto& priv =
        group.coverpoint("cp_priv", [](const Sample& s) { return s.priv; });
    priv.bin("m", 3u).bin("u", 0u);
    auto& irq =
        group.coverpoint("cp_irq", [](const Sample& s) { return s.irq; });
    irq.bin("timer", 1u).bin("external", 2u);
    auto& category =
        group.coverpoint("cp_cat", [](const Sample& s) { return s.category; });
    category.bin("load", 0u).bin("store", 1u);

    group.cross("priv_irq_cat", priv, irq, category);
    group.sample({.irq = 1, .priv = 3, .category = 0});
    group.sample({.irq = 2, .priv = 0, .category = 1});

    const auto snapshot = group.snapshot();
    const auto& cross = snapshot.crosses[0];
    check(cross.points.size() == 3, "a three-way cross names three points");
    check(cross.bins.size() == 8, "a three-way cross has 2*2*2 bins");
    check(cross_hits(cross, {"m", "timer", "load"}) == 1 &&
              cross_hits(cross, {"u", "external", "store"}) == 1,
          "each sample lands in exactly one three-way cross bin");
}

// 19.6.1: `ignore_bins x = binsof(cp) intersect {v}` removes those cross bins
// from the bin set entirely -- they are neither counted nor coverable.
void cross_bin_selection() {
    cpptb::Covergroup<Sample> group("select");
    auto& priv =
        group.coverpoint("cp_priv", [](const Sample& s) { return s.priv; });
    priv.bin("m", 3u).bin("u", 0u);
    auto& irq =
        group.coverpoint("cp_irq", [](const Sample& s) { return s.irq; });
    irq.bin("timer", 1u).bin("external", 2u);

    group.cross("priv_irq", priv, irq)
        .ignore("no_user_timer",
                cpptb::binsof(priv, {0u}) && cpptb::binsof(irq, {1u}));

    group.sample({.irq = 1, .priv = 0});  // the ignored combination
    group.sample({.irq = 1, .priv = 3});

    const auto snapshot = group.snapshot();
    const auto& cross = snapshot.crosses[0];
    check(cross.bins.size() == 3,
          "an ignored cross bin leaves the bin set");
    check(!has_cross_bin(cross, {"u", "timer"}),
          "the ignored combination is the one removed");
    check(cross_hits(cross, {"m", "timer"}) == 1,
          "the remaining combinations still count");
    check(cross.ignored.size() == 1 &&
              cross.ignored[0].find("binsof(cp_priv)") != std::string::npos,
          "the select expression is reported, not silently dropped");
}

// The same, for illegal_bins: reaching one is an error, and it is reported.
void cross_illegal_bins() {
    cpptb::Covergroup<Sample> group("illegal");
    auto& priv =
        group.coverpoint("cp_priv", [](const Sample& s) { return s.priv; });
    priv.bin("m", 3u).bin("u", 0u);
    auto& irq =
        group.coverpoint("cp_irq", [](const Sample& s) { return s.irq; });
    irq.bin("timer", 1u).bin("external", 2u);

    group.cross("priv_irq", priv, irq)
        .illegal("user_external",
                 cpptb::binsof(priv, {0u}) && cpptb::binsof(irq, {2u}));

    const auto legal = group.sample({.irq = 1, .priv = 0});
    check(legal.legal(), "a combination no filter names is legal");
    const auto bad = group.sample({.irq = 2, .priv = 0});
    check(!bad.legal(), "reaching an illegal cross bin is reported");

    const auto snapshot = group.snapshot();
    check(snapshot.crosses[0].illegal_hits == 1,
          "the illegal cross bin counts its hits");
    check(!has_cross_bin(snapshot.crosses[0], {"u", "external"}),
          "an illegal cross bin is not a coverable bin");
}

// `||` in a select expression, which Verilator ignores 8 times over in Ibex.
void cross_select_disjunction() {
    cpptb::Covergroup<Sample> group("disjunction");
    auto& priv =
        group.coverpoint("cp_priv", [](const Sample& s) { return s.priv; });
    priv.bin("m", 3u).bin("u", 0u);
    auto& irq =
        group.coverpoint("cp_irq", [](const Sample& s) { return s.irq; });
    irq.bin("timer", 1u).bin("external", 2u);

    group.cross("priv_irq", priv, irq)
        .ignore("either", cpptb::binsof(priv, {0u}) || cpptb::binsof(irq, {2u}));

    const auto snapshot = group.snapshot();
    // Removes u/timer, u/external and m/external, leaving only m/timer.
    check(snapshot.crosses[0].bins.size() == 1 &&
              has_cross_bin(snapshot.crosses[0], {"m", "timer"}),
          "a disjunction removes every combination either side names");
}

// 19.5: `coverpoint x iff (cond)` -- a sample the guard rejects is not taken.
void sampling_guard() {
    cpptb::Covergroup<Sample> group("guard");
    bool enabled = true;
    auto& priv =
        group.coverpoint("cp_priv", [](const Sample& s) { return s.priv; });
    priv.bin("m", 3u).iff([&] { return enabled; });

    group.sample({.priv = 3});
    enabled = false;
    group.sample({.priv = 3});
    enabled = true;
    group.sample({.priv = 3});

    const auto snapshot = group.snapshot();
    check(find_bin(snapshot.points[0], "m").hits == 2,
          "a guarded coverpoint ignores samples its guard rejects");
    check(snapshot.points[0].samples == 2,
          "a rejected sample is not counted as a sample");
}

// A guard on the cross rather than the coverpoint.
void cross_guard() {
    cpptb::Covergroup<Sample> group("cross_guard");
    bool enabled = false;
    auto& priv =
        group.coverpoint("cp_priv", [](const Sample& s) { return s.priv; });
    priv.bin("m", 3u);
    auto& irq =
        group.coverpoint("cp_irq", [](const Sample& s) { return s.irq; });
    irq.bin("timer", 1u);
    group.cross("priv_irq", priv, irq).iff([&] { return enabled; });

    group.sample({.irq = 1, .priv = 3});
    enabled = true;
    group.sample({.irq = 1, .priv = 3});

    const auto snapshot = group.snapshot();
    check(cross_hits(snapshot.crosses[0], {"m", "timer"}) == 1,
          "a guarded cross ignores samples its guard rejects");
    check(find_bin(snapshot.points[0], "m").hits == 2,
          "the cross guard does not suppress its coverpoints");
}

// 19.5.2: `illegal_bins x = default sequence;` catches every transition no
// listed transition bin accepts.
void default_sequence() {
    cpptb::Covergroup<Sample> group("fsm");
    auto& state =
        group.coverpoint("cp_fsm", [](const Sample& s) { return s.priv; });
    state.transition_bin("zero_to_one", 0u, 1u);
    state.transition_bin("one_to_two", 1u, 2u);
    state.illegal_default_sequence("illegal_transitions");

    check(group.sample({.priv = 0}).legal(),
          "the first sample has no previous value and no transition");
    check(group.sample({.priv = 1}).legal(), "0 -> 1 is listed");
    check(group.sample({.priv = 2}).legal(), "1 -> 2 is listed");
    const auto bad = group.sample({.priv = 0});
    check(!bad.legal(), "2 -> 0 is not listed and is caught");

    const auto snapshot = group.snapshot();
    check(find_bin(snapshot.points[0], "zero_to_one").hits == 1 &&
              find_bin(snapshot.points[0], "one_to_two").hits == 1,
          "listed transitions still count");
    check(find_bin(snapshot.points[0], "illegal_transitions").hits == 1,
          "the default sequence bin counts the transition it caught");
}

// A guarded transition coverpoint sees consecutive *sampled* values, not
// consecutive clock cycles. Getting this wrong turns every guarded FSM
// coverpoint into a source of phantom transitions.
void guard_does_not_break_transitions() {
    cpptb::Covergroup<Sample> group("guarded_fsm");
    bool enabled = true;
    auto& state =
        group.coverpoint("cp_fsm", [](const Sample& s) { return s.priv; });
    state.transition_bin("zero_to_one", 0u, 1u).iff([&] { return enabled; });

    group.sample({.priv = 0});
    enabled = false;
    group.sample({.priv = 7});  // not sampled, must not become the previous
    enabled = true;
    group.sample({.priv = 1});

    const auto snapshot = group.snapshot();
    check(find_bin(snapshot.points[0], "zero_to_one").hits == 1,
          "a rejected sample does not become the transition's previous value");
}

// 19.5: `bins name[] = {[0:31]};` is 32 bins, not one bin of 32 values.
void bin_arrays() {
    cpptb::Covergroup<Sample> group("arrays");
    auto& addr =
        group.coverpoint("cp_addr", [](const Sample& s) { return s.category; });
    addr.bin_array("napot_addr", 0u, 31u);

    group.sample({.category = 0});
    group.sample({.category = 31});
    group.sample({.category = 31});

    const auto snapshot = group.snapshot();
    check(snapshot.points[0].bins.size() == 32,
          "an array bin declares one bin per value");
    check(find_bin(snapshot.points[0], "napot_addr[0]").hits == 1 &&
              find_bin(snapshot.points[0], "napot_addr[31]").hits == 2,
          "array bins count independently");
}

// `with (expr)` re-expressed at bin granularity, which is exact whenever the
// expression is constant across each bin.
void where_selects() {
    cpptb::Covergroup<Sample> group("where");
    auto& priv =
        group.coverpoint("cp_priv", [](const Sample& s) { return s.priv; });
    priv.bin("m", 3u).bin("u", 0u);
    auto& irq =
        group.coverpoint("cp_irq", [](const Sample& s) { return s.irq; });
    irq.wildcard_bin("nmi", "1?????").wildcard_bin("timer", "000001");

    // Stands for `with (cp_irq >> 4 == 0)`: true for timer, false for nmi.
    group.cross("priv_irq", priv, irq)
        .ignore("upper_bits_clear",
                cpptb::binsof(priv, {3u}) &&
                    cpptb::where("cp_irq >> 4 == 0",
                                 [&](const cpptb::coverage_detail::CrossBinView&
                                         bin) {
                                     return bin.bin_name_of(irq) == "timer";
                                 }));

    const auto snapshot = group.snapshot();
    const auto& cross = snapshot.crosses[0];
    check(cross.bins.size() == 3 && !has_cross_bin(cross, {"m", "timer"}),
          "a where predicate removes the combinations it names");
    check(cross.ignored[0].find("where(cp_irq >> 4 == 0)") !=
              std::string::npos,
          "the SystemVerilog a where stands for is reported");
}

}  // namespace

int main() {
    bin_arrays();
    where_selects();
    wildcard_bins();
    value_list_bins();
    three_way_cross();
    cross_bin_selection();
    cross_illegal_bins();
    cross_select_disjunction();
    sampling_guard();
    cross_guard();
    default_sequence();
    guard_does_not_break_transitions();

    if (failures != 0) {
        std::cerr << failures << " coverage semantics test(s) failed\n";
        return 1;
    }
    std::cout << "coverage SV semantics tests passed\n";
    return 0;
}
