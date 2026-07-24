#pragma once

#include <concepts>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <ostream>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cpptb/diagnostic.hpp"
#include "cpptb/test_api.hpp"
#include "cpptb_vc/memory_mapped.hpp"
#include "cpptb_vc/ports.hpp"

namespace cpptb::vc {

enum class TransactionDisposition : uint8_t {
    Completed,
    Aborted,
    Incomplete,
};

inline constexpr std::string_view transaction_disposition_name(
    TransactionDisposition disposition) noexcept {
    switch (disposition) {
        case TransactionDisposition::Completed:
            return "completed";
        case TransactionDisposition::Aborted:
            return "aborted";
        case TransactionDisposition::Incomplete:
            return "incomplete";
    }
    return "unknown";
}

template <typename T>
struct TransactionObservation {
    coro::SimTime begin_time{};
    coro::SimTime end_time{};
    TransactionDisposition disposition = TransactionDisposition::Completed;
    T value{};
};

template <typename Observation, typename T>
concept TransactionObservationFor =
    std::same_as<std::remove_cvref_t<Observation>,
                 TransactionObservation<T>>;

template <auto Member>
struct TransactionField {
    static constexpr auto pointer = Member;
    std::string_view name;
};

template <auto Member>
constexpr TransactionField<Member> transaction_field(std::string_view name) {
    return {name};
}

template <typename T, typename... Fields>
struct StaticTransactionDescriptor {
    using value_type = T;

    std::string_view name;
    std::tuple<Fields...> fields;
};

template <typename T, typename... Fields>
constexpr auto describe_transaction(std::string_view name, Fields... fields) {
    return StaticTransactionDescriptor<T, Fields...>{
        name, std::tuple<Fields...>{fields...}};
}

namespace transaction_detail {

void cpptb_transaction_descriptor() = delete;

template <typename T>
constexpr auto descriptor_for()
    -> decltype(cpptb_transaction_descriptor(std::type_identity<T>{})) {
    return cpptb_transaction_descriptor(std::type_identity<T>{});
}

template <typename Member>
struct MemberPointer;

template <typename Owner, typename Value>
struct MemberPointer<Value Owner::*> {
    using owner_type = Owner;
    using value_type = Value;
};

inline void write_json_string(std::ostream& output, std::string_view value) {
    output.put('"');
    static constexpr char kHex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20) {
                    output << "\\u00" << kHex[character >> 4u]
                           << kHex[character & 0x0fu];
                } else {
                    output.put(static_cast<char>(character));
                }
                break;
        }
    }
    output.put('"');
}

template <typename T>
concept DescribedTransaction = requires { descriptor_for<T>(); };

template <typename T>
void write_json_value(std::ostream& output, const T& value);

template <typename T, auto Member>
void write_json_field(std::ostream& output, const void* object,
                      std::string_view name, bool& first) {
    using Traits = MemberPointer<decltype(Member)>;
    using Owner = typename Traits::owner_type;
    static_assert(std::same_as<T, Owner>,
                  "transaction descriptors must name direct data members");
    if (!first) output.put(',');
    first = false;
    write_json_string(output, name);
    output.put(':');
    write_json_value(output, static_cast<const Owner*>(object)->*Member);
}

template <typename T, typename... Fields>
void write_json_object_fields(
    std::ostream& output, const T& value,
    const StaticTransactionDescriptor<T, Fields...>& descriptor) {
    output.put('{');
    bool first = true;
    std::apply(
        [&](const auto&... field) {
            (write_json_field<
                 T, std::remove_cvref_t<decltype(field)>::pointer>(
                 output, std::addressof(value), field.name, first),
             ...);
        },
        descriptor.fields);
    output.put('}');
}

template <typename T>
concept StringLike = std::convertible_to<const T&, std::string_view>;

template <typename T>
void write_json_value(std::ostream& output, const T& value) {
    using Type = std::remove_cvref_t<T>;
    if constexpr (DescribedTransaction<Type>) {
        write_json_object_fields(output, value, descriptor_for<Type>());
    } else if constexpr (std::same_as<Type, bool>) {
        output << (value ? "true" : "false");
    } else if constexpr (std::is_enum_v<Type>) {
        if (const auto formatted = cpptb::format_diagnostic(value)) {
            write_json_string(output, *formatted);
        } else {
            using Underlying = std::underlying_type_t<Type>;
            if constexpr (std::is_signed_v<Underlying>) {
                output << static_cast<long long>(value);
            } else {
                output << static_cast<unsigned long long>(value);
            }
        }
    } else if constexpr (std::integral<Type>) {
        if constexpr (std::signed_integral<Type>) {
            output << static_cast<long long>(value);
        } else {
            output << static_cast<unsigned long long>(value);
        }
    } else if constexpr (std::floating_point<Type>) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument(
                "cpptb-vc: transaction floating-point field must be finite");
        }
        const auto previous_precision = output.precision();
        output << std::setprecision(std::numeric_limits<Type>::max_digits10)
               << value;
        output.precision(previous_precision);
    } else if constexpr (StringLike<Type>) {
        write_json_string(output, std::string_view{value});
    } else if constexpr (std::ranges::input_range<Type>) {
        output.put('[');
        bool first = true;
        for (const auto& element : value) {
            if (!first) output.put(',');
            first = false;
            write_json_value(output, element);
        }
        output.put(']');
    } else {
        const auto formatted = cpptb::format_diagnostic(value);
        if (!formatted) {
            throw std::invalid_argument(
                "cpptb-vc: transaction field has no JSON encoder");
        }
        write_json_string(output, *formatted);
    }
}

template <typename T>
void write_transaction_json(std::ostream& output, const void* value) {
    write_json_object_fields(output, *static_cast<const T*>(value),
                             descriptor_for<T>());
}

}  // namespace transaction_detail

template <typename Address, typename Data, typename ByteEnable>
constexpr auto cpptb_transaction_descriptor(
    std::type_identity<MemoryTransaction<Address, Data, ByteEnable>>) {
    using Transaction = MemoryTransaction<Address, Data, ByteEnable>;
    return describe_transaction<Transaction>(
        "memory_transaction",
        transaction_field<&Transaction::operation>("operation"),
        transaction_field<&Transaction::address>("address"),
        transaction_field<&Transaction::data>("data"),
        transaction_field<&Transaction::byte_enable>("byte_enable"),
        transaction_field<&Transaction::status>("status"),
        transaction_field<&Transaction::wait_cycles>("wait_cycles"));
}

template <typename T>
concept DescribedTransaction =
    transaction_detail::DescribedTransaction<std::remove_cvref_t<T>>;

class TransactionRecordView {
   public:
    std::string_view stream() const noexcept { return stream_; }
    std::string_view type() const noexcept { return type_; }
    uint64_t sequence() const noexcept { return sequence_; }
    coro::SimTime begin_time() const noexcept { return begin_time_; }
    coro::SimTime end_time() const noexcept { return end_time_; }
    TransactionDisposition disposition() const noexcept {
        return disposition_;
    }

    void write_value_json(std::ostream& output) const {
        write_value_json_(output, value_);
    }

   private:
    friend class TransactionRecorder;

    using JsonWriter = void (*)(std::ostream&, const void*);

    TransactionRecordView(std::string_view stream, std::string_view type,
                          uint64_t sequence, coro::SimTime begin_time,
                          coro::SimTime end_time,
                          TransactionDisposition disposition,
                          const void* value, JsonWriter write_value_json)
        : stream_(stream),
          type_(type),
          sequence_(sequence),
          begin_time_(begin_time),
          end_time_(end_time),
          disposition_(disposition),
          value_(value),
          write_value_json_(write_value_json) {}

    std::string_view stream_;
    std::string_view type_;
    uint64_t sequence_;
    coro::SimTime begin_time_;
    coro::SimTime end_time_;
    TransactionDisposition disposition_;
    const void* value_;
    JsonWriter write_value_json_;
};

struct RecordedTransaction {
    std::string stream;
    std::string type;
    uint64_t sequence = 0;
    coro::SimTime begin_time{};
    coro::SimTime end_time{};
    TransactionDisposition disposition = TransactionDisposition::Completed;
    std::string value_json;
};

class InMemoryTransactionSink {
   public:
    void write(const TransactionRecordView& record) {
        std::ostringstream value;
        record.write_value_json(value);
        records_.push_back(RecordedTransaction{
            .stream = std::string{record.stream()},
            .type = std::string{record.type()},
            .sequence = record.sequence(),
            .begin_time = record.begin_time(),
            .end_time = record.end_time(),
            .disposition = record.disposition(),
            .value_json = value.str(),
        });
    }

    const std::vector<RecordedTransaction>& records() const noexcept {
        return records_;
    }

    void clear() noexcept { records_.clear(); }

   private:
    std::vector<RecordedTransaction> records_;
};

class JsonLinesTransactionSink {
   public:
    explicit JsonLinesTransactionSink(std::string path)
        : path_(std::move(path)), output_(path_, std::ios::trunc) {
        if (!output_) {
            throw std::runtime_error(
                "cpptb-vc: could not open transaction trace '" + path_ +
                "'");
        }
    }

    JsonLinesTransactionSink(const JsonLinesTransactionSink&) = delete;
    JsonLinesTransactionSink& operator=(const JsonLinesTransactionSink&) =
        delete;

    ~JsonLinesTransactionSink() {
        if (output_.is_open()) output_.close();
    }

    void write(const TransactionRecordView& record) {
        if (finalized_) {
            throw std::logic_error(
                "cpptb-vc: transaction trace is already finalized");
        }
        std::ostringstream line;
        line.put('{');
        transaction_detail::write_json_string(line, "stream");
        line.put(':');
        transaction_detail::write_json_string(line, record.stream());
        line << ",\"sequence\":" << record.sequence();
        line << ",\"type\":";
        transaction_detail::write_json_string(line, record.type());
        line << ",\"begin_time_fs\":"
             << record.begin_time().in_femtoseconds();
        line << ",\"end_time_fs\":"
             << record.end_time().in_femtoseconds();
        line << ",\"disposition\":";
        transaction_detail::write_json_string(
            line, transaction_disposition_name(record.disposition()));
        line << ",\"value\":";
        record.write_value_json(line);
        line << "}\n";
        output_ << line.str();
        if (!output_) fail("write");
    }

    void finalize() {
        if (finalized_) return;
        output_.flush();
        if (!output_) fail("flush");
        output_.close();
        if (!output_) fail("close");
        finalized_ = true;
    }

    bool finalized() const noexcept { return finalized_; }
    const std::string& path() const noexcept { return path_; }

   private:
    [[noreturn]] void fail(std::string_view operation) const {
        throw std::runtime_error("cpptb-vc: transaction trace " +
                                 std::string{operation} + " failed for '" +
                                 path_ + "'");
    }

    std::string path_;
    std::ofstream output_;
    bool finalized_ = false;
};

class TransactionStreamBase {
   public:
    virtual ~TransactionStreamBase() = default;
};

class TransactionRecorder;

template <DescribedTransaction T>
class TransactionStream final : public TransactionStreamBase {
   public:
    void write(const TransactionObservation<T>& observation);

    std::string_view name() const noexcept { return name_; }
    uint64_t next_sequence() const noexcept { return next_sequence_; }

   private:
    friend class TransactionRecorder;

    TransactionStream(TransactionRecorder& recorder, std::string name)
        : recorder_(std::addressof(recorder)), name_(std::move(name)) {}

    TransactionRecorder* recorder_;
    std::string name_;
    uint64_t next_sequence_ = 0;
};

class TransactionRecorder {
   public:
    using Connection = AnalysisPort<TransactionRecordView>::Connection;

    TransactionRecorder() = default;
    TransactionRecorder(const TransactionRecorder&) = delete;
    TransactionRecorder& operator=(const TransactionRecorder&) = delete;
    TransactionRecorder(TransactionRecorder&&) = delete;
    TransactionRecorder& operator=(TransactionRecorder&&) = delete;

    template <typename Sink>
        requires AnalysisSubscriber<Sink, TransactionRecordView>
    [[nodiscard]] Connection connect(Sink& sink) {
        return output_.connect(sink);
    }

    template <DescribedTransaction T>
    TransactionStream<T>& stream(std::string name) {
        if (name.empty()) {
            throw std::invalid_argument(
                "cpptb-vc: transaction stream name must not be empty");
        }
        if (!stream_names_.insert(name).second) {
            throw std::invalid_argument(
                "cpptb-vc: duplicate transaction stream name '" + name +
                "'");
        }
        auto stream = std::unique_ptr<TransactionStream<T>>{
            new TransactionStream<T>{*this, std::move(name)}};
        auto& result = *stream;
        streams_.push_back(std::move(stream));
        return result;
    }

    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    bool enabled() const noexcept { return enabled_; }
    std::size_t stream_count() const noexcept { return streams_.size(); }
    std::size_t sink_count() const { return output_.subscriber_count(); }

   private:
    template <DescribedTransaction T>
    friend class TransactionStream;

    template <DescribedTransaction T>
    void publish(TransactionStream<T>& stream,
                 const TransactionObservation<T>& observation) {
        if (!enabled_ || output_.subscriber_count() == 0) return;
        static constexpr auto descriptor =
            transaction_detail::descriptor_for<T>();
        const TransactionRecordView view{
            stream.name_,
            descriptor.name,
            stream.next_sequence_++,
            observation.begin_time,
            observation.end_time,
            observation.disposition,
            std::addressof(observation.value),
            &transaction_detail::write_transaction_json<T>,
        };
        output_.write(view);
    }

    AnalysisPort<TransactionRecordView> output_;
    std::vector<std::unique_ptr<TransactionStreamBase>> streams_;
    std::unordered_set<std::string> stream_names_;
    bool enabled_ = true;
};

template <DescribedTransaction T>
void TransactionStream<T>::write(
    const TransactionObservation<T>& observation) {
    recorder_->publish(*this, observation);
}

template <typename T>
class TransactionMonitor {
   public:
    using transaction_type = T;
    using observation_type = TransactionObservation<T>;

    explicit TransactionMonitor(TestContext test,
                                coro::SimTime sample_delay = {})
        : test_(std::move(test)), sample_delay_(sample_delay) {}

    AnalysisPort<observation_type>& observed() noexcept { return observed_; }
    const AnalysisPort<observation_type>& observed() const noexcept {
        return observed_;
    }

   protected:
    template <typename ClockSignal>
    coro::Task<void> sample(ClockSignal clock) {
        co_await coro::RisingEdge{static_cast<coro::Signal>(clock)};
        if (sample_delay_.in_femtoseconds() != 0) {
            co_await coro::Delay{sample_delay_};
        }
    }

    template <typename ClockSignal, typename Predicate>
        requires std::predicate<Predicate&>
    coro::Task<void> sample_until(ClockSignal clock, Predicate predicate) {
        do {
            co_await coro::RisingEdge{static_cast<coro::Signal>(clock)};
            if (sample_delay_.in_femtoseconds() != 0) {
                co_await coro::Delay{sample_delay_};
            }
        } while (!predicate());
    }

    coro::SimTime now() const { return test_.now(); }

    void publish(coro::SimTime begin_time, T value,
                 TransactionDisposition disposition =
                     TransactionDisposition::Completed) {
        observed_.write(observation_type{
            .begin_time = begin_time,
            .end_time = test_.now(),
            .disposition = disposition,
            .value = std::move(value),
        });
    }

   private:
    TestContext test_;
    coro::SimTime sample_delay_{};
    AnalysisPort<observation_type> observed_;
};

}  // namespace cpptb::vc

#define CPPTB_VC_DESCRIBE_TRANSACTION(Type, Name, ...)                     \
    constexpr auto cpptb_transaction_descriptor(std::type_identity<Type>) { \
        return ::cpptb::vc::describe_transaction<Type>(Name, __VA_ARGS__);  \
    }
