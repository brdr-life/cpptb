#pragma once

#include <algorithm>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace cpptb {

template <typename Value>
concept CoverageScalar =
    std::integral<std::remove_cv_t<Value>> ||
    std::is_enum_v<std::remove_cv_t<Value>>;

enum class CoverageBinKind : uint8_t {
    Ordinary,
    Ignore,
    Illegal,
    Transition,
};

inline constexpr std::string_view coverage_bin_kind_name(
    CoverageBinKind kind) noexcept {
    switch (kind) {
        case CoverageBinKind::Ordinary:
            return "ordinary";
        case CoverageBinKind::Ignore:
            return "ignore";
        case CoverageBinKind::Illegal:
            return "illegal";
        case CoverageBinKind::Transition:
            return "transition";
    }
    return "ordinary";
}

struct CoverageSampleResult {
    uint32_t illegal_hits = 0;

    bool legal() const noexcept { return illegal_hits == 0; }
    explicit operator bool() const noexcept { return illegal_hits == 0; }
};

struct CoverageBinSnapshot {
    std::string name;
    CoverageBinKind kind = CoverageBinKind::Ordinary;
    uint64_t hits = 0;
};

struct CoveragePointSnapshot {
    std::string name;
    uint64_t samples = 0;
    uint64_t illegal_hits = 0;
    std::vector<CoverageBinSnapshot> bins;
};

struct CoverageCrossBinSnapshot {
    std::string left_bin;
    std::string right_bin;
    uint64_t hits = 0;
};

struct CoverageCrossSnapshot {
    std::string name;
    std::string left;
    std::string right;
    std::vector<CoverageCrossBinSnapshot> bins;
};

struct CoverageSnapshot {
    std::string name;
    std::string source_file;
    uint32_t source_line = 0;
    uint64_t samples = 0;
    uint64_t illegal_hits = 0;
    std::vector<CoveragePointSnapshot> points;
    std::vector<CoverageCrossSnapshot> crosses;

    uint64_t coverable_bins() const noexcept {
        uint64_t count = 0;
        for (const auto& point : points) {
            for (const auto& bin : point.bins) {
                if (bin.kind == CoverageBinKind::Ordinary ||
                    bin.kind == CoverageBinKind::Transition) {
                    ++count;
                }
            }
        }
        for (const auto& cross : crosses) count += cross.bins.size();
        return count;
    }

    uint64_t covered_bins() const noexcept {
        uint64_t count = 0;
        for (const auto& point : points) {
            for (const auto& bin : point.bins) {
                if ((bin.kind == CoverageBinKind::Ordinary ||
                     bin.kind == CoverageBinKind::Transition) &&
                    bin.hits != 0) {
                    ++count;
                }
            }
        }
        for (const auto& cross : crosses) {
            count += static_cast<uint64_t>(std::count_if(
                cross.bins.begin(), cross.bins.end(),
                [](const auto& bin) { return bin.hits != 0; }));
        }
        return count;
    }

    double coverage_percent() const noexcept {
        const uint64_t total = coverable_bins();
        if (total == 0) return 100.0;
        return 100.0 * static_cast<double>(covered_bins()) /
               static_cast<double>(total);
    }

    void merge(const CoverageSnapshot& other) {
        const auto mismatch = [] {
            throw std::invalid_argument(
                "cpptb coverage snapshots have different models");
        };
        if (name != other.name || points.size() != other.points.size() ||
            crosses.size() != other.crosses.size()) {
            mismatch();
        }

        // Validate the complete model before changing any hit counts.
        for (std::size_t point_index = 0; point_index < points.size();
             ++point_index) {
            const auto& point = points[point_index];
            const auto& incoming = other.points[point_index];
            if (point.name != incoming.name ||
                point.bins.size() != incoming.bins.size()) {
                mismatch();
            }
            for (std::size_t bin_index = 0; bin_index < point.bins.size();
                 ++bin_index) {
                const auto& bin = point.bins[bin_index];
                const auto& incoming_bin = incoming.bins[bin_index];
                if (bin.name != incoming_bin.name ||
                    bin.kind != incoming_bin.kind) {
                    mismatch();
                }
            }
        }
        for (std::size_t cross_index = 0; cross_index < crosses.size();
             ++cross_index) {
            const auto& cross = crosses[cross_index];
            const auto& incoming = other.crosses[cross_index];
            if (cross.name != incoming.name || cross.left != incoming.left ||
                cross.right != incoming.right ||
                cross.bins.size() != incoming.bins.size()) {
                mismatch();
            }
            for (std::size_t bin_index = 0; bin_index < cross.bins.size();
                 ++bin_index) {
                const auto& bin = cross.bins[bin_index];
                const auto& incoming_bin = incoming.bins[bin_index];
                if (bin.left_bin != incoming_bin.left_bin ||
                    bin.right_bin != incoming_bin.right_bin) {
                    mismatch();
                }
            }
        }

        samples += other.samples;
        illegal_hits += other.illegal_hits;
        for (std::size_t point_index = 0; point_index < points.size();
             ++point_index) {
            auto& point = points[point_index];
            const auto& incoming = other.points[point_index];
            point.samples += incoming.samples;
            point.illegal_hits += incoming.illegal_hits;
            for (std::size_t bin_index = 0; bin_index < point.bins.size();
                 ++bin_index) {
                point.bins[bin_index].hits += incoming.bins[bin_index].hits;
            }
        }
        for (std::size_t cross_index = 0; cross_index < crosses.size();
             ++cross_index) {
            auto& cross = crosses[cross_index];
            const auto& incoming = other.crosses[cross_index];
            for (std::size_t bin_index = 0; bin_index < cross.bins.size();
                 ++bin_index) {
                auto& bin = cross.bins[bin_index];
                const auto& incoming_bin = incoming.bins[bin_index];
                bin.hits += incoming_bin.hits;
            }
        }
    }
};

namespace coverage_detail {

inline void write_json_string(FILE* stream, std::string_view value) {
    std::fputc('"', stream);
    for (const unsigned char character : value) {
        switch (character) {
            case '"':
                std::fputs("\\\"", stream);
                break;
            case '\\':
                std::fputs("\\\\", stream);
                break;
            case '\n':
                std::fputs("\\n", stream);
                break;
            case '\r':
                std::fputs("\\r", stream);
                break;
            case '\t':
                std::fputs("\\t", stream);
                break;
            default:
                if (character < 0x20) {
                    std::fprintf(stream, "\\u%04x", character);
                } else {
                    std::fputc(character, stream);
                }
                break;
        }
    }
    std::fputc('"', stream);
}

class CoverpointBase {
   public:
    explicit CoverpointBase(std::string name) : name_(std::move(name)) {
        if (name_.empty()) {
            throw std::invalid_argument("cpptb coverpoint name is empty");
        }
    }
    virtual ~CoverpointBase() = default;

    const std::string& name() const noexcept { return name_; }
    const std::vector<std::size_t>& last_ordinary_hits() const noexcept {
        return last_ordinary_hits_;
    }
    virtual std::size_t ordinary_bin_count() const noexcept = 0;
    virtual std::string_view ordinary_bin_name(std::size_t index) const = 0;
    virtual CoveragePointSnapshot snapshot() const = 0;
    virtual uint64_t illegal_hits() const noexcept = 0;
    virtual void seal() noexcept = 0;

   protected:
    std::string name_;
    std::vector<std::size_t> last_ordinary_hits_;
};

template <CoverageScalar Value>
constexpr auto ordered(Value value) noexcept {
    if constexpr (std::is_enum_v<Value>) {
        return static_cast<std::underlying_type_t<Value>>(value);
    } else {
        return value;
    }
}

}  // namespace coverage_detail

template <CoverageScalar Value>
class Coverpoint final : public coverage_detail::CoverpointBase {
   public:
    explicit Coverpoint(std::string name)
        : coverage_detail::CoverpointBase(std::move(name)) {}

    Coverpoint& bin(std::string name, Value value) {
        return add_bin(CoverageBinKind::Ordinary, std::move(name), value,
                       value, value, value);
    }

    Coverpoint& bin(std::string name, Value minimum, Value maximum) {
        return add_bin(CoverageBinKind::Ordinary, std::move(name), minimum,
                       maximum, minimum, maximum);
    }

    Coverpoint& ignore_bin(std::string name, Value value) {
        return add_bin(CoverageBinKind::Ignore, std::move(name), value, value,
                       value, value);
    }

    Coverpoint& ignore_bin(std::string name, Value minimum, Value maximum) {
        return add_bin(CoverageBinKind::Ignore, std::move(name), minimum,
                       maximum, minimum, maximum);
    }

    Coverpoint& illegal_bin(std::string name, Value value) {
        return add_bin(CoverageBinKind::Illegal, std::move(name), value, value,
                       value, value);
    }

    Coverpoint& illegal_bin(std::string name, Value minimum, Value maximum) {
        return add_bin(CoverageBinKind::Illegal, std::move(name), minimum,
                       maximum, minimum, maximum);
    }

    Coverpoint& transition_bin(std::string name, Value from, Value to) {
        return add_bin(CoverageBinKind::Transition, std::move(name), from,
                       from, to, to);
    }

    CoverageSampleResult sample(Value value) {
        sampled_ = true;
        ++samples_;
        this->last_ordinary_hits_.clear();

        bool ignored = false;
        uint32_t illegal = 0;
        for (auto& bin : bins_) {
            if (bin.kind != CoverageBinKind::Ignore &&
                bin.kind != CoverageBinKind::Illegal) {
                continue;
            }
            if (!contains(value, bin.minimum, bin.maximum)) continue;
            ++bin.hits;
            if (bin.kind == CoverageBinKind::Illegal) {
                ++illegal;
                ++illegal_hits_;
            } else {
                ignored = true;
            }
        }

        if (!ignored && illegal == 0) {
            std::size_t ordinary_index = 0;
            for (auto& bin : bins_) {
                if (bin.kind == CoverageBinKind::Ordinary) {
                    if (contains(value, bin.minimum, bin.maximum)) {
                        ++bin.hits;
                        this->last_ordinary_hits_.push_back(ordinary_index);
                    }
                    ++ordinary_index;
                } else if (bin.kind == CoverageBinKind::Transition &&
                           previous_ &&
                           contains(*previous_, bin.minimum, bin.maximum) &&
                           contains(value, bin.to_minimum, bin.to_maximum)) {
                    ++bin.hits;
                }
            }
        }
        previous_ = value;
        return {.illegal_hits = illegal};
    }

    std::size_t ordinary_bin_count() const noexcept override {
        return ordinary_bin_names_.size();
    }

    std::string_view ordinary_bin_name(std::size_t index) const override {
        return ordinary_bin_names_.at(index);
    }

    CoveragePointSnapshot snapshot() const override {
        CoveragePointSnapshot result{
            .name = this->name_,
            .samples = samples_,
            .illegal_hits = illegal_hits_,
        };
        result.bins.reserve(bins_.size());
        for (const auto& bin : bins_) {
            result.bins.push_back({bin.name, bin.kind, bin.hits});
        }
        return result;
    }

    uint64_t illegal_hits() const noexcept override { return illegal_hits_; }

    void seal() noexcept override { sealed_ = true; }

   private:
    struct Bin {
        std::string name;
        CoverageBinKind kind = CoverageBinKind::Ordinary;
        Value minimum{};
        Value maximum{};
        Value to_minimum{};
        Value to_maximum{};
        uint64_t hits = 0;
    };

    static bool contains(Value value, Value minimum, Value maximum) noexcept {
        return coverage_detail::ordered(value) >=
                   coverage_detail::ordered(minimum) &&
               coverage_detail::ordered(value) <=
                   coverage_detail::ordered(maximum);
    }

    Coverpoint& add_bin(CoverageBinKind kind, std::string name, Value minimum,
                        Value maximum, Value to_minimum, Value to_maximum) {
        if (sampled_) {
            throw std::logic_error(
                "cpptb coverage bins cannot be added after sampling");
        }
        if (sealed_) {
            throw std::logic_error(
                "cpptb coverage bins cannot be added after the coverpoint "
                "is used in a cross");
        }
        if (name.empty()) {
            throw std::invalid_argument("cpptb coverage bin name is empty");
        }
        if (coverage_detail::ordered(maximum) <
                coverage_detail::ordered(minimum) ||
            coverage_detail::ordered(to_maximum) <
                coverage_detail::ordered(to_minimum)) {
            throw std::invalid_argument(
                "cpptb coverage bin range is reversed");
        }
        if (std::ranges::any_of(bins_, [&](const Bin& bin) {
                return bin.name == name;
            })) {
            throw std::invalid_argument("duplicate cpptb coverage bin '" +
                                        name + "'");
        }
        if (kind == CoverageBinKind::Ordinary) {
            ordinary_bin_names_.push_back(name);
            this->last_ordinary_hits_.reserve(ordinary_bin_names_.size());
        }
        bins_.push_back(Bin{std::move(name), kind, minimum, maximum,
                            to_minimum, to_maximum, 0});
        return *this;
    }

    std::vector<Bin> bins_;
    std::vector<std::string> ordinary_bin_names_;
    uint64_t samples_ = 0;
    uint64_t illegal_hits_ = 0;
    std::optional<Value> previous_;
    bool sampled_ = false;
    bool sealed_ = false;
};

template <typename Sample>
class Covergroup {
   public:
    explicit Covergroup(
        std::string name,
        std::source_location location = std::source_location::current())
        : name_(std::move(name)),
          source_file_(location.file_name()),
          source_line_(location.line()) {
        if (name_.empty()) {
            throw std::invalid_argument("cpptb covergroup name is empty");
        }
    }

    template <typename Extractor>
        requires CoverageScalar<std::remove_cvref_t<std::invoke_result_t<
            Extractor, const Sample&>>>
    auto& coverpoint(std::string name, Extractor extractor) {
        using Value = std::remove_cvref_t<
            std::invoke_result_t<Extractor, const Sample&>>;
        if (sampled_) {
            throw std::logic_error(
                "cpptb coverpoints cannot be added after sampling");
        }
        if (find_point(name)) {
            throw std::invalid_argument("duplicate cpptb coverpoint '" + name +
                                        "'");
        }
        auto point = std::make_unique<BoundPoint<Value, Extractor>>(
            std::move(name), std::move(extractor));
        auto& result = point->point;
        points_.push_back(std::move(point));
        return result;
    }

    template <CoverageScalar Left, CoverageScalar Right>
    void cross(std::string name, Coverpoint<Left>& left,
               Coverpoint<Right>& right) {
        if (sampled_) {
            throw std::logic_error(
                "cpptb crosses cannot be added after sampling");
        }
        auto* left_point = find_point(left);
        auto* right_point = find_point(right);
        if (!left_point || !right_point) {
            throw std::invalid_argument(
                "cpptb cross coverpoints must belong to the covergroup");
        }
        if (name.empty()) {
            throw std::invalid_argument("cpptb coverage cross name is empty");
        }
        if (std::ranges::any_of(crosses_, [&](const Cross& cross) {
                return cross.name == name;
            })) {
            throw std::invalid_argument("duplicate cpptb coverage cross '" +
                                        name + "'");
        }
        left_point->seal();
        right_point->seal();
        crosses_.emplace_back(std::move(name), left_point, right_point);
    }

    CoverageSampleResult sample(const Sample& sample_value) {
        sampled_ = true;
        ++samples_;
        uint32_t illegal = 0;
        for (auto& point : points_) {
            illegal += point->sample(sample_value).illegal_hits;
        }
        for (auto& cross : crosses_) cross.sample();
        illegal_hits_ += illegal;
        return {.illegal_hits = illegal};
    }

    CoverageSnapshot snapshot() const {
        CoverageSnapshot result{
            .name = name_,
            .source_file = source_file_,
            .source_line = source_line_,
            .samples = samples_,
            .illegal_hits = illegal_hits_,
        };
        result.points.reserve(points_.size());
        for (const auto& point : points_) {
            result.points.push_back(point->base().snapshot());
        }
        result.crosses.reserve(crosses_.size());
        for (const auto& cross : crosses_) {
            result.crosses.push_back(cross.snapshot());
        }
        return result;
    }

   private:
    class BoundPointBase {
       public:
        virtual ~BoundPointBase() = default;
        virtual CoverageSampleResult sample(const Sample&) = 0;
        virtual coverage_detail::CoverpointBase& base() noexcept = 0;
        virtual const coverage_detail::CoverpointBase& base() const noexcept = 0;
    };

    template <CoverageScalar Value, typename Extractor>
    class BoundPoint final : public BoundPointBase {
       public:
        BoundPoint(std::string name, Extractor extractor)
            : point(std::move(name)), extractor_(std::move(extractor)) {}

        CoverageSampleResult sample(const Sample& value) override {
            return point.sample(std::invoke(extractor_, value));
        }
        coverage_detail::CoverpointBase& base() noexcept override {
            return point;
        }
        const coverage_detail::CoverpointBase& base() const noexcept override {
            return point;
        }

        Coverpoint<Value> point;

       private:
        Extractor extractor_;
    };

    struct Cross {
        Cross(std::string cross_name,
              coverage_detail::CoverpointBase* left_point,
              coverage_detail::CoverpointBase* right_point)
            : name(std::move(cross_name)),
              left(left_point),
              right(right_point),
              hits(left->ordinary_bin_count() * right->ordinary_bin_count()) {}

        std::string name;
        coverage_detail::CoverpointBase* left = nullptr;
        coverage_detail::CoverpointBase* right = nullptr;
        std::vector<uint64_t> hits;

        void sample() {
            const std::size_t right_count = right->ordinary_bin_count();
            for (const std::size_t left_bin : left->last_ordinary_hits()) {
                for (const std::size_t right_bin :
                     right->last_ordinary_hits()) {
                    ++hits[left_bin * right_count + right_bin];
                }
            }
        }

        CoverageCrossSnapshot snapshot() const {
            CoverageCrossSnapshot result{
                .name = name,
                .left = left->name(),
                .right = right->name(),
            };
            const std::size_t left_count = left->ordinary_bin_count();
            const std::size_t right_count = right->ordinary_bin_count();
            result.bins.reserve(left_count * right_count);
            for (std::size_t left_index = 0; left_index < left_count;
                 ++left_index) {
                for (std::size_t right_index = 0; right_index < right_count;
                     ++right_index) {
                    const std::size_t index =
                        left_index * right_count + right_index;
                    result.bins.push_back(CoverageCrossBinSnapshot{
                        .left_bin = std::string{left->ordinary_bin_name(
                            left_index)},
                        .right_bin = std::string{right->ordinary_bin_name(
                            right_index)},
                        .hits = index < hits.size() ? hits[index] : 0,
                    });
                }
            }
            return result;
        }
    };

    BoundPointBase* find_point(std::string_view name) noexcept {
        const auto found = std::ranges::find_if(points_, [&](const auto& point) {
            return point->base().name() == name;
        });
        return found == points_.end() ? nullptr : found->get();
    }

    template <CoverageScalar Value>
    coverage_detail::CoverpointBase* find_point(
        Coverpoint<Value>& candidate) noexcept {
        const auto found = std::ranges::find_if(points_, [&](const auto& point) {
            return &point->base() == &candidate;
        });
        return found == points_.end() ? nullptr : &(*found)->base();
    }

    std::string name_;
    std::string source_file_;
    uint32_t source_line_ = 0;
    std::vector<std::unique_ptr<BoundPointBase>> points_;
    std::vector<Cross> crosses_;
    uint64_t samples_ = 0;
    uint64_t illegal_hits_ = 0;
    bool sampled_ = false;
};

inline bool write_coverage_json(const char* path,
                                const CoverageSnapshot& coverage) {
    if (!path || path[0] == '\0') return true;
    FILE* stream = std::fopen(path, "w");
    if (!stream) return false;
    const auto string_field = [&](std::string_view name,
                                  std::string_view value) {
        coverage_detail::write_json_string(stream, name);
        std::fputc(':', stream);
        coverage_detail::write_json_string(stream, value);
    };

    std::fputs("{\n  \"schema_version\":1,\n  ", stream);
    string_field("name", coverage.name);
    std::fputs(",\n  ", stream);
    string_field("source_file", coverage.source_file);
    std::fprintf(stream,
                 ",\n  \"source_line\":%u,\n  \"samples\":%llu,\n"
                 "  \"illegal_hits\":%llu,\n  \"covered_bins\":%llu,\n"
                 "  \"coverable_bins\":%llu,\n  \"points\":[",
                 coverage.source_line,
                 static_cast<unsigned long long>(coverage.samples),
                 static_cast<unsigned long long>(coverage.illegal_hits),
                 static_cast<unsigned long long>(coverage.covered_bins()),
                 static_cast<unsigned long long>(coverage.coverable_bins()));
    for (std::size_t point_index = 0; point_index < coverage.points.size();
         ++point_index) {
        const auto& point = coverage.points[point_index];
        std::fputs(point_index == 0 ? "\n    {" : ",\n    {", stream);
        string_field("name", point.name);
        std::fprintf(stream,
                     ",\"samples\":%llu,\"illegal_hits\":%llu,\"bins\":[",
                     static_cast<unsigned long long>(point.samples),
                     static_cast<unsigned long long>(point.illegal_hits));
        for (std::size_t bin_index = 0; bin_index < point.bins.size();
             ++bin_index) {
            const auto& bin = point.bins[bin_index];
            std::fputs(bin_index == 0 ? "{" : ",{", stream);
            string_field("name", bin.name);
            std::fputs(",", stream);
            string_field("kind", coverage_bin_kind_name(bin.kind));
            std::fprintf(stream, ",\"hits\":%llu}",
                         static_cast<unsigned long long>(bin.hits));
        }
        std::fputs("]}", stream);
    }
    if (!coverage.points.empty()) std::fputc('\n', stream);
    std::fputs("  ],\n  \"crosses\":[", stream);
    for (std::size_t cross_index = 0; cross_index < coverage.crosses.size();
         ++cross_index) {
        const auto& cross = coverage.crosses[cross_index];
        std::fputs(cross_index == 0 ? "\n    {" : ",\n    {", stream);
        string_field("name", cross.name);
        std::fputs(",", stream);
        string_field("left", cross.left);
        std::fputs(",", stream);
        string_field("right", cross.right);
        std::fputs(",\"bins\":[", stream);
        for (std::size_t bin_index = 0; bin_index < cross.bins.size();
             ++bin_index) {
            const auto& bin = cross.bins[bin_index];
            std::fputs(bin_index == 0 ? "{" : ",{", stream);
            string_field("left_bin", bin.left_bin);
            std::fputs(",", stream);
            string_field("right_bin", bin.right_bin);
            std::fprintf(stream, ",\"hits\":%llu}",
                         static_cast<unsigned long long>(bin.hits));
        }
        std::fputs("]}", stream);
    }
    if (!coverage.crosses.empty()) std::fputc('\n', stream);
    std::fputs("  ]\n}\n", stream);
    return std::fclose(stream) == 0;
}

}  // namespace cpptb
