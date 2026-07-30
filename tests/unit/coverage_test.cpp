#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include "cpptb/coverage.hpp"

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

enum class Opcode : uint8_t { Read, Write, Reserved };

struct Transaction {
    Opcode opcode;
    uint16_t length;
};

cpptb::Covergroup<Transaction> make_coverage() {
    cpptb::Covergroup<Transaction> coverage{"transactions"};
    auto& opcode = coverage.coverpoint("opcode", &Transaction::opcode);
    opcode.bin("read", Opcode::Read)
        .bin("write", Opcode::Write)
        .illegal_bin("reserved", Opcode::Reserved);

    auto& length = coverage.coverpoint("length", &Transaction::length);
    length.ignore_bin("empty", uint16_t{0})
        .bin("small", uint16_t{1}, uint16_t{63})
        .bin("large", uint16_t{64}, uint16_t{1500})
        .illegal_bin("oversize", uint16_t{1501}, uint16_t{0xffff})
        .transition_bin("four_to_eight", uint16_t{4}, uint16_t{8});
    coverage.cross("opcode_x_length", opcode, length);
    return coverage;
}

const cpptb::CoverageBinSnapshot& find_bin(
    const cpptb::CoveragePointSnapshot& point, std::string_view name) {
    for (const auto& bin : point.bins) {
        if (bin.name == name) return bin;
    }
    throw std::runtime_error("missing coverage bin");
}

}  // namespace

int main() {
    auto coverage = make_coverage();
    check(static_cast<bool>(coverage.sample({Opcode::Read, uint16_t{4}})),
          "ordinary sample is legal");
    coverage.sample({Opcode::Write, uint16_t{8}});
    coverage.sample({Opcode::Read, uint16_t{100}});
    const auto illegal = coverage.sample({Opcode::Reserved, uint16_t{200}});
    check(!static_cast<bool>(illegal) && illegal.illegal_hits == 1,
          "illegal bin hit is returned to the caller");
    coverage.sample({Opcode::Read, uint16_t{0}});

    const auto snapshot = coverage.snapshot();
    check(snapshot.name == "transactions" && snapshot.samples == 5,
          "snapshot retains model name and sample count");
    check(snapshot.illegal_hits == 1 && snapshot.points.size() == 2 &&
              snapshot.crosses.size() == 1,
          "snapshot records points, crosses, and illegal hits");

    const auto& opcode = snapshot.points[0];
    const auto& length = snapshot.points[1];
    check(find_bin(opcode, "read").hits == 3 &&
              find_bin(opcode, "write").hits == 1 &&
              find_bin(opcode, "reserved").hits == 1,
          "ordinary and illegal opcode bins count samples");
    check(find_bin(length, "empty").hits == 1 &&
              find_bin(length, "small").hits == 2 &&
              find_bin(length, "large").hits == 2,
          "range and ignore bins count samples");
    check(find_bin(length, "four_to_eight").hits == 1,
          "transition bin observes consecutive values");

    const auto& cross = snapshot.crosses[0];
    check(cross.bins.size() == 4 && cross.bins[0].hits == 1 &&
              cross.bins[1].hits == 1 && cross.bins[2].hits == 1 &&
              cross.bins[3].hits == 0,
          "cross counts each ordinary bin combination");
    check(snapshot.coverable_bins() == 9 && snapshot.covered_bins() == 8 &&
              std::abs(snapshot.coverage_percent() - 88.888888) < 0.001,
          "coverage percentage includes points, transitions, and crosses");

    auto merged = snapshot;
    merged.merge(snapshot);
    check(merged.samples == 10 && merged.illegal_hits == 2 &&
              find_bin(merged.points[0], "read").hits == 6 &&
              merged.crosses[0].bins[0].hits == 2,
          "matching snapshots merge hit counts");

    auto different = make_coverage().snapshot();
    different.name = "different";
    bool rejected_merge = false;
    try {
        merged.merge(different);
    } catch (const std::invalid_argument&) {
        rejected_merge = true;
    }
    check(rejected_merge, "different coverage models cannot be merged");

    auto late_mismatch = snapshot;
    late_mismatch.crosses[0].bins.back().bins.back() = "different";
    const uint64_t samples_before_failed_merge = merged.samples;
    const uint64_t hits_before_failed_merge =
        find_bin(merged.points[0], "read").hits;
    try {
        merged.merge(late_mismatch);
    } catch (const std::invalid_argument&) {
    }
    check(merged.samples == samples_before_failed_merge &&
              find_bin(merged.points[0], "read").hits ==
                  hits_before_failed_merge,
          "failed coverage merge does not partially update counters");

    bool rejected_mutation = false;
    try {
        cpptb::Coverpoint<uint16_t> late{"late"};
        late.bin("first", uint16_t{1});
        late.sample(1);
        late.bin("second", uint16_t{2});
    } catch (const std::logic_error&) {
        rejected_mutation = true;
    }
    check(rejected_mutation, "coverage model freezes after sampling");

    bool rejected_cross_mutation = false;
    try {
        cpptb::Covergroup<Transaction> crossed{"crossed"};
        auto& left = crossed.coverpoint("opcode", &Transaction::opcode);
        left.bin("read", Opcode::Read);
        auto& right = crossed.coverpoint("length", &Transaction::length);
        right.bin("small", uint16_t{1}, uint16_t{8});
        crossed.cross("opcode_x_length", left, right);
        right.bin("large", uint16_t{9}, uint16_t{16});
    } catch (const std::logic_error&) {
        rejected_cross_mutation = true;
    }
    check(rejected_cross_mutation,
          "crossed coverpoints reject bins that would resize the cross");

    const char* path = "coverage_test.json";
    check(cpptb::write_coverage_json(path, snapshot),
          "coverage JSON is written");
    std::ifstream input{path};
    const std::string json{std::istreambuf_iterator<char>{input},
                           std::istreambuf_iterator<char>{}};
    check(json.find("\"schema_version\":1") != std::string::npos &&
              json.find("\"name\":\"transactions\"") !=
                  std::string::npos &&
              json.find("\"bins\":[\"read\"") != std::string::npos,
          "coverage JSON contains stable model and cross data");
    std::remove(path);

    if (failures != 0) {
        std::cerr << failures << " coverage test(s) failed\n";
        return 1;
    }
    std::cout << "coverage tests passed\n";
    return 0;
}
