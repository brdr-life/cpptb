// SPDX-License-Identifier: Apache-2.0
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
    // One bin name per crossed coverpoint, in the order they were crossed.
    // SystemVerilog crosses two or more coverpoints; Ibex's fcov crosses three
    // in several places, so this is a list rather than a left/right pair.
    std::vector<std::string> bins;
    uint64_t hits = 0;
};

struct CoverageCrossSnapshot {
    std::string name;
    std::vector<std::string> points;
    // The select expressions that removed bins from this cross, as written.
    // Reported so that a cross with filters can be told apart from one whose
    // filters were silently dropped -- which is what Verilator does.
    std::vector<std::string> ignored;
    std::vector<std::string> illegal;
    uint64_t illegal_hits = 0;
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
            if (cross.name != incoming.name ||
                cross.points != incoming.points ||
                cross.ignored != incoming.ignored ||
                cross.illegal != incoming.illegal ||
                cross.bins.size() != incoming.bins.size()) {
                mismatch();
            }
            for (std::size_t bin_index = 0; bin_index < cross.bins.size();
                 ++bin_index) {
                if (cross.bins[bin_index].bins !=
                    incoming.bins[bin_index].bins) {
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
    // True when the bin would accept this value. A cross filter written as
    // binsof(point) intersect {v} selects the cross bins whose `point` bin
    // accepts v, and asking the coverpoint keeps the cross free of the
    // coverpoint's value type.
    virtual bool ordinary_bin_accepts(std::size_t index,
                                      int64_t value) const = 0;
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
    // One entry of a bin's value list: `{3}` is Range{3, 3} and `{[0:7]}` is
    // Range{0, 7}, so a SystemVerilog bin body transcribes term for term.
    struct Range {
        Value minimum{};
        Value maximum{};

        Range(Value only) : minimum(only), maximum(only) {}
        Range(Value low, Value high) : minimum(low), maximum(high) {}
    };

    explicit Coverpoint(std::string name)
        : coverage_detail::CoverpointBase(std::move(name)) {}

    Coverpoint& bin(std::string name, Value value) {
        return add_bin(CoverageBinKind::Ordinary, std::move(name),
                       {Range{value, value}}, {});
    }

    Coverpoint& bin(std::string name, Value minimum, Value maximum) {
        return add_bin(CoverageBinKind::Ordinary, std::move(name),
                       {Range{minimum, maximum}}, {});
    }

    // `bins b = {a, b, c};` and `bins b = {[0:3], 7};`. A bin in SystemVerilog
    // holds a value list, not a single range, and a bin that holds several
    // values still counts once however many of them a sample matches.
    Coverpoint& bin(std::string name, std::initializer_list<Range> ranges) {
        return add_bin(CoverageBinKind::Ordinary, std::move(name),
                       std::vector<Range>(ranges), {});
    }

    Coverpoint& ignore_bin(std::string name, Value value) {
        return add_bin(CoverageBinKind::Ignore, std::move(name),
                       {Range{value, value}}, {});
    }

    Coverpoint& ignore_bin(std::string name, Value minimum, Value maximum) {
        return add_bin(CoverageBinKind::Ignore, std::move(name),
                       {Range{minimum, maximum}}, {});
    }

    Coverpoint& ignore_bin(std::string name,
                           std::initializer_list<Range> ranges) {
        return add_bin(CoverageBinKind::Ignore, std::move(name),
                       std::vector<Range>(ranges), {});
    }

    Coverpoint& illegal_bin(std::string name, Value value) {
        return add_bin(CoverageBinKind::Illegal, std::move(name),
                       {Range{value, value}}, {});
    }

    Coverpoint& illegal_bin(std::string name, Value minimum, Value maximum) {
        return add_bin(CoverageBinKind::Illegal, std::move(name),
                       {Range{minimum, maximum}}, {});
    }

    Coverpoint& illegal_bin(std::string name,
                            std::initializer_list<Range> ranges) {
        return add_bin(CoverageBinKind::Illegal, std::move(name),
                       std::vector<Range>(ranges), {});
    }

    Coverpoint& transition_bin(std::string name, Value from, Value to) {
        return add_bin(CoverageBinKind::Transition, std::move(name),
                       {Range{from, from}}, {Range{to, to}});
    }

    // `wildcard bins b = {6'b1?????};` -- the pattern is read the way the
    // source writes it, most significant bit first, with `?` or `x` for a
    // don't-care. Taking the text rather than a value/mask pair keeps the
    // transcription from the SystemVerilog checkable by eye.
    Coverpoint& wildcard_bin(std::string name, std::string_view pattern) {
        return add_wildcard(CoverageBinKind::Ordinary, std::move(name),
                            pattern);
    }

    Coverpoint& wildcard_ignore_bin(std::string name,
                                    std::string_view pattern) {
        return add_wildcard(CoverageBinKind::Ignore, std::move(name), pattern);
    }

    Coverpoint& wildcard_illegal_bin(std::string name,
                                     std::string_view pattern) {
        return add_wildcard(CoverageBinKind::Illegal, std::move(name),
                            pattern);
    }

    // `bins napot_addr[] = { [0:31] };` -- an array bin is N bins, one per
    // value, not one bin spanning them. The names follow the source's
    // `name[index]` so a report can be read against the SystemVerilog.
    Coverpoint& bin_array(const std::string& name, Value minimum,
                          Value maximum) {
        const auto low = coverage_detail::ordered(minimum);
        const auto high = coverage_detail::ordered(maximum);
        if (high < low) {
            throw std::invalid_argument("cpptb coverage bin array '" + name +
                                        "' has a reversed range");
        }
        for (auto value = low; value <= high; ++value) {
            const auto element = static_cast<Value>(value);
            bin(name + "[" + std::to_string(value) + "]", element);
            if (value == high) break;  // guard the wrap at the type's maximum
        }
        return *this;
    }

    // `illegal_bins x = default sequence;` -- every transition that no
    // transition bin above accepts. SystemVerilog gives no way to enumerate
    // those, so this is a flag rather than a bin with ranges.
    Coverpoint& illegal_default_sequence(std::string name) {
        return add_bin(CoverageBinKind::Illegal, std::move(name), {}, {},
                       /*wildcard=*/false, /*default_sequence=*/true);
    }

    // `coverpoint x iff (cond)`. A sample the guard rejects is not a sample:
    // it advances nothing, not even the transition history, which is what
    // makes an iff-guarded transition coverpoint see consecutive *sampled*
    // values rather than consecutive clock cycles.
    Coverpoint& iff(std::function<bool()> guard) {
        guard_ = std::move(guard);
        return *this;
    }

    CoverageSampleResult sample(Value value) {
        sampled_ = true;
        this->last_ordinary_hits_.clear();
        // An iff-guarded coverpoint that rejects this sample contributes
        // nothing, and must not advance `previous_`: a cross reads
        // last_ordinary_hits(), and leaving it empty is what keeps the guard
        // from letting a stale hit through.
        if (guard_ && !guard_()) return {};
        ++samples_;

        bool ignored = false;
        uint32_t illegal = 0;
        for (auto& bin : bins_) {
            if (bin.kind != CoverageBinKind::Ignore &&
                bin.kind != CoverageBinKind::Illegal) {
                continue;
            }
            if (bin.default_sequence) continue;  // handled with transitions
            if (!bin.matches_from(value)) continue;
            ++bin.hits;
            if (bin.kind == CoverageBinKind::Illegal) {
                ++illegal;
                ++illegal_hits_;
            } else {
                ignored = true;
            }
        }

        if (!ignored && illegal == 0) {
            bool any_transition = false;
            bool have_transitions = false;
            std::size_t ordinary_index = 0;
            for (auto& bin : bins_) {
                if (bin.kind == CoverageBinKind::Ordinary) {
                    if (bin.matches_from(value)) {
                        ++bin.hits;
                        this->last_ordinary_hits_.push_back(ordinary_index);
                    }
                    ++ordinary_index;
                } else if (bin.kind == CoverageBinKind::Transition) {
                    have_transitions = true;
                    if (previous_ && bin.matches_from(*previous_) &&
                        bin.matches_to(value)) {
                        ++bin.hits;
                        any_transition = true;
                    }
                }
            }
            // `default sequence` catches the transitions no listed transition
            // bin took. It only means anything once there is a previous value
            // and at least one transition bin to have missed.
            if (have_transitions && previous_ && !any_transition) {
                for (auto& bin : bins_) {
                    if (!bin.default_sequence) continue;
                    ++bin.hits;
                    ++illegal;
                    ++illegal_hits_;
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

    // Which values a coverpoint's bins accept, so that a cross filter written
    // as binsof(point) intersect {...} can be answered without the cross
    // knowing the coverpoint's value type.
    bool ordinary_bin_accepts(std::size_t index,
                              int64_t value) const override {
        std::size_t ordinary_index = 0;
        for (const auto& bin : bins_) {
            if (bin.kind != CoverageBinKind::Ordinary) continue;
            if (ordinary_index == index) {
                return bin.matches_from(static_cast<Value>(value));
            }
            ++ordinary_index;
        }
        return false;
    }

   private:
    struct Bin {
        std::string name;
        CoverageBinKind kind = CoverageBinKind::Ordinary;
        std::vector<Range> from;
        std::vector<Range> to;
        bool wildcard = false;
        // Care mask and wanted bits, both already in the ordered domain.
        uint64_t care = 0;
        uint64_t want = 0;
        bool default_sequence = false;
        uint64_t hits = 0;

        bool matches_from(Value value) const noexcept {
            if (wildcard) {
                const auto raw =
                    static_cast<uint64_t>(coverage_detail::ordered(value));
                return (raw & care) == want;
            }
            return in_any(value, from);
        }

        bool matches_to(Value value) const noexcept {
            return in_any(value, to);
        }

        static bool in_any(Value value,
                           const std::vector<Range>& ranges) noexcept {
            return std::ranges::any_of(ranges, [&](const Range& range) {
                return contains(value, range.minimum, range.maximum);
            });
        }
    };

    static bool contains(Value value, Value minimum, Value maximum) noexcept {
        return coverage_detail::ordered(value) >=
                   coverage_detail::ordered(minimum) &&
               coverage_detail::ordered(value) <=
                   coverage_detail::ordered(maximum);
    }

    Coverpoint& add_wildcard(CoverageBinKind kind, std::string name,
                             std::string_view pattern) {
        uint64_t care = 0;
        uint64_t want = 0;
        std::size_t bits = 0;
        for (const char character : pattern) {
            if (character == '_') continue;  // 6'b1_0000 reads as written
            care <<= 1;
            want <<= 1;
            switch (character) {
                case '0':
                    care |= 1;
                    break;
                case '1':
                    care |= 1;
                    want |= 1;
                    break;
                case '?':
                case 'x':
                case 'X':
                    break;
                default:
                    throw std::invalid_argument(
                        "cpptb wildcard bin '" + name +
                        "' has a character other than 0, 1, ? or x in its "
                        "pattern");
            }
            ++bits;
        }
        if (bits == 0 || bits > 64) {
            throw std::invalid_argument("cpptb wildcard bin '" + name +
                                        "' needs between 1 and 64 pattern "
                                        "bits");
        }
        return add_bin(kind, std::move(name), {}, {}, /*wildcard=*/true,
                       /*default_sequence=*/false, care, want);
    }

    Coverpoint& add_bin(CoverageBinKind kind, std::string name,
                        std::vector<Range> from, std::vector<Range> to,
                        bool wildcard = false, bool default_sequence = false,
                        uint64_t care = 0, uint64_t want = 0) {
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
        for (const auto& ranges : {std::cref(from), std::cref(to)}) {
            for (const Range& range : ranges.get()) {
                if (coverage_detail::ordered(range.maximum) <
                    coverage_detail::ordered(range.minimum)) {
                    throw std::invalid_argument(
                        "cpptb coverage bin '" + name +
                        "' has a reversed range");
                }
            }
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
        bins_.push_back(Bin{std::move(name), kind, std::move(from),
                            std::move(to), wildcard, care, want,
                            default_sequence, 0});
        return *this;
    }

    std::vector<Bin> bins_;
    std::vector<std::string> ordinary_bin_names_;
    std::function<bool()> guard_;
    uint64_t samples_ = 0;
    uint64_t illegal_hits_ = 0;
    std::optional<Value> previous_;
    bool sampled_ = false;
    bool sealed_ = false;
};

// ---------------------------------------------------------------------------
// Cross bin selection: `binsof(point) intersect {values}`, and the `&&`, `||`
// and `!` that combine them.
//
// A cross bin is one combination of its coverpoints' bins. A select expression
// picks out a subset of those combinations, and `illegal_bins` or
// `ignore_bins` in a cross body then removes them or makes them errors. Without
// this a cross keeps every combination, which is a different -- and much
// larger -- bin set than the source describes.
// ---------------------------------------------------------------------------

namespace coverage_detail {

// The bins each coverpoint of a cross matched, by index into that coverpoint's
// ordinary bins.
struct CrossBinRef {
    const std::vector<CoverpointBase*>* points = nullptr;
    const std::vector<std::size_t>* bins = nullptr;
};

class SelectNode {
   public:
    virtual ~SelectNode() = default;
    virtual bool matches(const CrossBinRef& bin) const = 0;
    virtual std::string describe() const = 0;
};

using SelectPtr = std::shared_ptr<const SelectNode>;

class BinsOfNode final : public SelectNode {
   public:
    BinsOfNode(const CoverpointBase* point, std::vector<int64_t> values)
        : point_(point), values_(std::move(values)) {}

    bool matches(const CrossBinRef& bin) const override {
        for (std::size_t index = 0; index < bin.points->size(); ++index) {
            if ((*bin.points)[index] != point_) continue;
            const std::size_t which = (*bin.bins)[index];
            if (values_.empty()) return true;  // bare binsof(point)
            return std::ranges::any_of(values_, [&](int64_t value) {
                return (*bin.points)[index]->ordinary_bin_accepts(which,
                                                                  value);
            });
        }
        return false;
    }

    std::string describe() const override {
        std::string text = "binsof(" + point_->name() + ")";
        if (values_.empty()) return text;
        text += " intersect {";
        for (std::size_t index = 0; index < values_.size(); ++index) {
            if (index) text += ", ";
            text += std::to_string(values_[index]);
        }
        return text + "}";
    }

   private:
    const CoverpointBase* point_ = nullptr;
    std::vector<int64_t> values_;
};

// The bins one cross combination is made of, for a `where` predicate.
class CrossBinView {
   public:
    CrossBinView(const std::vector<CoverpointBase*>* points,
                 const std::vector<std::size_t>* bins)
        : points_(points), bins_(bins) {}

    std::size_t size() const noexcept { return points_->size(); }

    std::string_view bin_name(std::size_t position) const {
        return (*points_)[position]->ordinary_bin_name((*bins_)[position]);
    }

    std::string_view point_name(std::size_t position) const {
        return (*points_)[position]->name();
    }

    // The bin this combination uses for a particular coverpoint.
    template <typename Point>
    std::string_view bin_name_of(const Point& point) const {
        for (std::size_t index = 0; index < points_->size(); ++index) {
            if ((*points_)[index] == static_cast<const CoverpointBase*>(&point)) {
                return bin_name(index);
            }
        }
        throw std::invalid_argument(
            "cpptb where(): that coverpoint is not part of this cross");
    }

   private:
    const std::vector<CoverpointBase*>* points_ = nullptr;
    const std::vector<std::size_t>* bins_ = nullptr;
};

class WhereNode final : public SelectNode {
   public:
    WhereNode(std::string text, std::function<bool(const CrossBinView&)> test)
        : text_(std::move(text)), test_(std::move(test)) {}

    bool matches(const CrossBinRef& bin) const override {
        return test_(CrossBinView{bin.points, bin.bins});
    }
    std::string describe() const override { return "where(" + text_ + ")"; }

   private:
    std::string text_;
    std::function<bool(const CrossBinView&)> test_;
};

class BinaryNode final : public SelectNode {
   public:
    BinaryNode(bool conjunction, SelectPtr left, SelectPtr right)
        : conjunction_(conjunction),
          left_(std::move(left)),
          right_(std::move(right)) {}

    bool matches(const CrossBinRef& bin) const override {
        return conjunction_ ? (left_->matches(bin) && right_->matches(bin))
                            : (left_->matches(bin) || right_->matches(bin));
    }

    std::string describe() const override {
        return "(" + left_->describe() + (conjunction_ ? " && " : " || ") +
               right_->describe() + ")";
    }

   private:
    bool conjunction_ = true;
    SelectPtr left_;
    SelectPtr right_;
};

class NotNode final : public SelectNode {
   public:
    explicit NotNode(SelectPtr inner) : inner_(std::move(inner)) {}
    bool matches(const CrossBinRef& bin) const override {
        return !inner_->matches(bin);
    }
    std::string describe() const override {
        return "!" + inner_->describe();
    }

   private:
    SelectPtr inner_;
};

}  // namespace coverage_detail

// The expression a cross body is written with. `Select` is copyable and cheap;
// the nodes behind it are shared.
class Select {
   public:
    explicit Select(coverage_detail::SelectPtr node) : node_(std::move(node)) {}

    bool matches(const coverage_detail::CrossBinRef& bin) const {
        return node_->matches(bin);
    }
    std::string describe() const { return node_->describe(); }
    const coverage_detail::SelectPtr& node() const noexcept { return node_; }

   private:
    coverage_detail::SelectPtr node_;
};

inline Select operator&&(const Select& left, const Select& right) {
    return Select{std::make_shared<coverage_detail::BinaryNode>(
        true, left.node(), right.node())};
}

inline Select operator||(const Select& left, const Select& right) {
    return Select{std::make_shared<coverage_detail::BinaryNode>(
        false, left.node(), right.node())};
}

inline Select operator!(const Select& inner) {
    return Select{std::make_shared<coverage_detail::NotNode>(inner.node())};
}

// The escape hatch for a select expression that binsof/intersect cannot say.
//
// SystemVerilog's `with (expr)` filters a cross select by an expression over
// the crossed coverpoints' *values*, where cpptb selects whole bins. The two
// agree whenever the expression is constant across each bin, which is the case
// for every `with` in Ibex's functional coverage -- there the expression tests
// bits that its bins already partition on, so it can be rewritten as a choice
// between named bins. `text` is the SystemVerilog it stands for, and it is
// reported, so a translation stays checkable against the source.
//
// Where an expression does vary within a bin, no bin-level predicate can match
// it, and this cannot pretend otherwise.
inline Select where(std::string text,
                    std::function<bool(const coverage_detail::CrossBinView&)>
                        test) {
    return Select{std::make_shared<coverage_detail::WhereNode>(
        std::move(text), std::move(test))};
}

// `binsof(cp)` and `binsof(cp) intersect {a, b}`.
template <CoverageScalar Value>
Select binsof(Coverpoint<Value>& point) {
    return Select{std::make_shared<coverage_detail::BinsOfNode>(
        &point, std::vector<int64_t>{})};
}

template <CoverageScalar Value>
Select binsof(Coverpoint<Value>& point,
              std::initializer_list<Value> intersect) {
    std::vector<int64_t> values;
    values.reserve(intersect.size());
    for (const Value value : intersect) {
        values.push_back(
            static_cast<int64_t>(coverage_detail::ordered(value)));
    }
    return Select{
        std::make_shared<coverage_detail::BinsOfNode>(&point,
                                                      std::move(values))};
}

template <typename Sample>
class Covergroup {
   private:
    // Defined before the public API because CrossRef and cross()
    // both name it.
    struct Cross {
        struct Filter {
            std::string name;
            Select select;
            bool illegal = false;
            uint64_t hits = 0;
        };

        explicit Cross(std::string cross_name,
                       std::vector<coverage_detail::CoverpointBase*> crossed)
            : name(std::move(cross_name)), points(std::move(crossed)) {
            std::size_t total = 1;
            for (const auto* point : points) {
                total *= point->ordinary_bin_count();
            }
            hits.assign(total, 0);
        }

        std::string name;
        std::vector<coverage_detail::CoverpointBase*> points;
        std::vector<uint64_t> hits;
        std::vector<Filter> filters;
        std::function<bool()> guard;
        uint64_t illegal_hits = 0;

        // Mixed radix over the coverpoints' ordinary bin counts, most
        // significant first, so the bin order matches the nesting a
        // SystemVerilog cross reports.
        std::size_t index_of(const std::vector<std::size_t>& bins) const {
            std::size_t index = 0;
            for (std::size_t position = 0; position < points.size();
                 ++position) {
                index = index * points[position]->ordinary_bin_count() +
                        bins[position];
            }
            return index;
        }

        // Which filter, if any, removes this combination. Ignore wins over
        // illegal: a bin excluded from the cross cannot also be an error.
        const Filter* excluded_by(const std::vector<std::size_t>& bins) const {
            const coverage_detail::CrossBinRef reference{&points, &bins};
            const Filter* illegal_match = nullptr;
            for (const auto& filter : filters) {
                if (!filter.select.matches(reference)) continue;
                if (!filter.illegal) return &filter;
                if (!illegal_match) illegal_match = &filter;
            }
            return illegal_match;
        }

        uint32_t sample() {
            if (guard && !guard()) return 0;
            std::vector<std::size_t> bins(points.size(), 0);
            uint32_t illegal = 0;
            walk(0, bins, illegal);
            return illegal;
        }

        void walk(std::size_t position, std::vector<std::size_t>& bins,
                  uint32_t& illegal) {
            if (position == points.size()) {
                const auto* excluded = excluded_by(bins);
                if (!excluded) {
                    ++hits[index_of(bins)];
                    return;
                }
                // The filter counts either way, so that a run can show an
                // illegal cross bin was reached rather than merely defined.
                const_cast<Filter*>(excluded)->hits++;
                if (excluded->illegal) {
                    ++illegal;
                    ++illegal_hits;
                }
                return;
            }
            for (const std::size_t bin :
                 points[position]->last_ordinary_hits()) {
                bins[position] = bin;
                walk(position + 1, bins, illegal);
            }
        }

        CoverageCrossSnapshot snapshot() const {
            CoverageCrossSnapshot result{.name = name};
            result.points.reserve(points.size());
            for (const auto* point : points) {
                result.points.push_back(point->name());
            }
            for (const auto& filter : filters) {
                (filter.illegal ? result.illegal : result.ignored)
                    .push_back(filter.name + " = " + filter.select.describe());
            }
            result.illegal_hits = illegal_hits;
            std::vector<std::size_t> bins(points.size(), 0);
            collect(0, bins, result);
            return result;
        }

        void collect(std::size_t position, std::vector<std::size_t>& bins,
                     CoverageCrossSnapshot& result) const {
            if (position == points.size()) {
                // A filtered-out combination is not part of the bin set, so it
                // is not reported and does not count towards coverage.
                if (excluded_by(bins)) return;
                CoverageCrossBinSnapshot entry{.hits = hits[index_of(bins)]};
                entry.bins.reserve(points.size());
                for (std::size_t which = 0; which < points.size(); ++which) {
                    entry.bins.emplace_back(
                        points[which]->ordinary_bin_name(bins[which]));
                }
                result.bins.push_back(std::move(entry));
                return;
            }
            const std::size_t count =
                points[position]->ordinary_bin_count();
            for (std::size_t bin = 0; bin < count; ++bin) {
                bins[position] = bin;
                collect(position + 1, bins, result);
            }
        }
    };

   public:
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

    // A handle on a cross, so that a cross body's `illegal_bins`,
    // `ignore_bins` and `iff` read in the order the source writes them.
    class CrossRef {
       public:
        explicit CrossRef(Cross* cross) : cross_(cross) {}

        CrossRef& illegal(std::string name, Select select) {
            return add(std::move(name), std::move(select), true);
        }
        CrossRef& ignore(std::string name, Select select) {
            return add(std::move(name), std::move(select), false);
        }
        CrossRef& iff(std::function<bool()> guard) {
            cross_->guard = std::move(guard);
            return *this;
        }

       private:
        CrossRef& add(std::string name, Select select, bool illegal) {
            if (name.empty()) {
                throw std::invalid_argument(
                    "cpptb cross bin filter name is empty");
            }
            cross_->filters.push_back(typename Cross::Filter{
                std::move(name), std::move(select), illegal, 0});
            return *this;
        }

        Cross* cross_ = nullptr;
    };

    // `cross a, b;` and `cross a, b, c;`. SystemVerilog allows any number of
    // coverpoints; Ibex's fcov uses two and three.
    template <CoverageScalar... Values>
    CrossRef cross(std::string name, Coverpoint<Values>&... points) {
        static_assert(sizeof...(points) >= 2,
                      "a cross needs at least two coverpoints");
        if (sampled_) {
            throw std::logic_error(
                "cpptb crosses cannot be added after sampling");
        }
        if (name.empty()) {
            throw std::invalid_argument("cpptb coverage cross name is empty");
        }
        std::vector<coverage_detail::CoverpointBase*> crossed;
        crossed.reserve(sizeof...(points));
        (
            [&] {
                auto* found = find_point(points);
                if (!found) {
                    throw std::invalid_argument(
                        "cpptb cross coverpoints must belong to the "
                        "covergroup");
                }
                found->seal();
                crossed.push_back(found);
            }(),
            ...);
        if (std::ranges::any_of(crosses_, [&](const Cross& cross) {
                return cross.name == name;
            })) {
            throw std::invalid_argument("duplicate cpptb coverage cross '" +
                                        name + "'");
        }
        crosses_.push_back(Cross{std::move(name), std::move(crossed)});
        return CrossRef{&crosses_.back()};
    }

    CoverageSampleResult sample(const Sample& sample_value) {
        sampled_ = true;
        ++samples_;
        uint32_t illegal = 0;
        for (auto& point : points_) {
            illegal += point->sample(sample_value).illegal_hits;
        }
        // After every coverpoint, so that each cross reads the bins its
        // coverpoints matched on this sample.
        for (auto& cross : crosses_) illegal += cross.sample();
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
        const auto string_list = [&](std::string_view field,
                                     const std::vector<std::string>& values) {
            coverage_detail::write_json_string(stream, field);
            std::fputs(":[", stream);
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (index) std::fputc(',', stream);
                coverage_detail::write_json_string(stream, values[index]);
            }
            std::fputc(']', stream);
        };
        string_field("name", cross.name);
        std::fputs(",", stream);
        string_list("points", cross.points);
        std::fputs(",", stream);
        string_list("ignored", cross.ignored);
        std::fputs(",", stream);
        string_list("illegal", cross.illegal);
        std::fprintf(stream, ",\"illegal_hits\":%llu",
                     static_cast<unsigned long long>(cross.illegal_hits));
        std::fputs(",\"bins\":[", stream);
        for (std::size_t bin_index = 0; bin_index < cross.bins.size();
             ++bin_index) {
            const auto& bin = cross.bins[bin_index];
            std::fputs(bin_index == 0 ? "{" : ",{", stream);
            string_list("bins", bin.bins);
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
