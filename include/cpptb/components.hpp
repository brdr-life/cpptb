#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "cpptb/coro_runtime.hpp"
#include "cpptb/test_api.hpp"

namespace cpptb {

template <typename T>
class PutPort {
    static_assert(std::move_constructible<T>,
                  "PutPort values must be move constructible");

   public:
    template <typename Backend>
        requires requires(Backend& backend, T value) {
            static_cast<coro::Task<void> (Backend::*)(T)>(&Backend::put);
            { backend.put(std::move(value)) } ->
                std::same_as<coro::Task<void>>;
            { backend.put_nowait(std::move(value)) } ->
                std::convertible_to<bool>;
        }
    explicit PutPort(Backend& backend) noexcept
        : object_(std::addressof(backend)),
          put_(&put_backend<Backend>),
          put_nowait_(&put_nowait_backend<Backend>) {}

    coro::Task<void> put(T value) const {
        return put_(object_, std::move(value));
    }

    bool put_nowait(T value) const {
        return put_nowait_(object_, std::move(value));
    }

   private:
    template <typename Backend>
    static coro::Task<void> put_backend(void* object, T value) {
        return static_cast<Backend*>(object)->put(std::move(value));
    }

    template <typename Backend>
    static bool put_nowait_backend(void* object, T value) {
        return static_cast<bool>(
            static_cast<Backend*>(object)->put_nowait(std::move(value)));
    }

    void* object_;
    coro::Task<void> (*put_)(void*, T);
    bool (*put_nowait_)(void*, T);
};

template <typename T>
PutPort(coro::Queue<T>&) -> PutPort<T>;

template <typename T>
class GetPort {
    static_assert(std::move_constructible<T>,
                  "GetPort values must be move constructible");

   public:
    template <typename Backend>
        requires requires(Backend& backend) {
            { backend.get() } -> std::same_as<coro::Task<T>>;
            { backend.get_nowait() } -> std::same_as<std::optional<T>>;
        }
    explicit GetPort(Backend& backend) noexcept
        : object_(std::addressof(backend)),
          get_(&get_backend<Backend>),
          get_nowait_(&get_nowait_backend<Backend>) {}

    coro::Task<T> get() const { return get_(object_); }

    std::optional<T> get_nowait() const { return get_nowait_(object_); }

   private:
    template <typename Backend>
    static coro::Task<T> get_backend(void* object) {
        return static_cast<Backend*>(object)->get();
    }

    template <typename Backend>
    static std::optional<T> get_nowait_backend(void* object) {
        return static_cast<Backend*>(object)->get_nowait();
    }

    void* object_;
    coro::Task<T> (*get_)(void*);
    std::optional<T> (*get_nowait_)(void*);
};

template <typename T>
GetPort(coro::Queue<T>&) -> GetPort<T>;

template <typename Subscriber, typename T>
concept AnalysisSubscriber = requires(Subscriber& subscriber, const T& value) {
    { subscriber.write(value) } -> std::same_as<void>;
};

template <typename T>
class AnalysisPort {
   private:
    struct Slot {
        uint64_t id;
        void* subscriber;
        void (*write)(void*, const T&);
        bool connected = true;
    };

    struct State {
        void disconnect(uint64_t id) {
            for (auto& slot : slots) {
                if (slot.id != id) continue;
                slot.connected = false;
                compact_pending = true;
                break;
            }
            compact();
        }

        void compact() {
            if (dispatch_depth != 0 || !compact_pending) return;
            std::erase_if(slots,
                          [](const Slot& slot) { return !slot.connected; });
            compact_pending = false;
        }

        std::vector<Slot> slots;
        uint64_t next_id = 1;
        std::size_t dispatch_depth = 0;
        bool compact_pending = false;
    };

   public:
    class Connection {
       public:
        Connection() = default;
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;

        Connection(Connection&& other) noexcept
            : state_(std::move(other.state_)),
              id_(std::exchange(other.id_, 0)) {}

        Connection& operator=(Connection&& other) noexcept {
            if (this == &other) return *this;
            disconnect();
            state_ = std::move(other.state_);
            id_ = std::exchange(other.id_, 0);
            return *this;
        }

        ~Connection() { disconnect(); }

        void disconnect() noexcept {
            if (id_ == 0) return;
            if (auto state = state_.lock()) state->disconnect(id_);
            id_ = 0;
            state_.reset();
        }

        bool connected() const noexcept {
            if (id_ == 0) return false;
            const auto state = state_.lock();
            if (!state) return false;
            return std::ranges::any_of(
                state->slots, [id = id_](const Slot& slot) {
                    return slot.id == id && slot.connected;
                });
        }

       private:
        friend class AnalysisPort;

        Connection(const std::shared_ptr<State>& state, uint64_t id)
            : state_(state), id_(id) {}

        std::weak_ptr<State> state_;
        uint64_t id_ = 0;
    };

    AnalysisPort() : state_(std::make_shared<State>()) {}
    AnalysisPort(const AnalysisPort&) = delete;
    AnalysisPort& operator=(const AnalysisPort&) = delete;
    AnalysisPort(AnalysisPort&&) noexcept = default;
    AnalysisPort& operator=(AnalysisPort&&) noexcept = default;

    template <typename Subscriber>
        requires AnalysisSubscriber<Subscriber, T>
    [[nodiscard]] Connection connect(Subscriber& subscriber) {
        const uint64_t id = state_->next_id++;
        state_->slots.push_back(Slot{
            .id = id,
            .subscriber = std::addressof(subscriber),
            .write = &write_subscriber<Subscriber>,
        });
        return Connection{state_, id};
    }

    void write(const T& value) const {
        const std::size_t published_subscribers = state_->slots.size();
        ++state_->dispatch_depth;
        try {
            for (std::size_t index = 0; index < published_subscribers;
                 ++index) {
                auto& slot = state_->slots[index];
                if (slot.connected) slot.write(slot.subscriber, value);
            }
        } catch (...) {
            --state_->dispatch_depth;
            state_->compact();
            throw;
        }
        --state_->dispatch_depth;
        state_->compact();
    }

    std::size_t subscriber_count() const {
        return static_cast<std::size_t>(std::ranges::count_if(
            state_->slots,
            [](const Slot& slot) { return slot.connected; }));
    }

   private:
    template <typename Subscriber>
    static void write_subscriber(void* subscriber, const T& value) {
        static_cast<Subscriber*>(subscriber)->write(value);
    }

    std::shared_ptr<State> state_;
};

enum class AnalysisOverflowPolicy {
    DropNewest,
    DropOldest,
    Error,
};

template <std::copy_constructible T>
class AnalysisBuffer {
   public:
    AnalysisBuffer(std::size_t capacity, AnalysisOverflowPolicy overflow)
        : queue_(capacity), overflow_(overflow) {}

    void write(const T& value) {
        if (queue_.put_nowait(value)) return;

        if (overflow_ == AnalysisOverflowPolicy::DropOldest) {
            auto discarded = queue_.get_nowait();
            if (discarded && queue_.put_nowait(value)) {
                ++dropped_;
                return;
            }
        }

        if (overflow_ == AnalysisOverflowPolicy::Error) {
            throw std::overflow_error(
                "cpptb: AnalysisBuffer is full under the Error overflow "
                "policy");
        }
        ++dropped_;
    }

    coro::Task<T> get() { return queue_.get(); }
    std::optional<T> get_nowait() { return queue_.get_nowait(); }
    GetPort<T> output() { return GetPort<T>{queue_}; }

    std::size_t size() { return queue_.size(); }
    bool empty() { return queue_.empty(); }
    std::size_t dropped() const { return dropped_; }

   private:
    coro::Queue<T> queue_;
    AnalysisOverflowPolicy overflow_;
    std::size_t dropped_ = 0;
};

template <typename T>
    requires std::copy_constructible<T> &&
             requires(const T& left, const T& right) {
                 { left == right } -> std::convertible_to<bool>;
             }
class InOrderScoreboard {
   private:
    enum class Side { Expected, Actual };

    class Input {
       public:
        void write(const T& value) {
            if (side_ == Side::Expected) {
                owner_->write_expected(value);
            } else {
                owner_->write_actual(value);
            }
        }

       private:
        friend class InOrderScoreboard;

        Input(InOrderScoreboard& owner, Side side)
            : owner_(std::addressof(owner)), side_(side) {}

        InOrderScoreboard* owner_;
        Side side_;
    };

   public:
    InOrderScoreboard(
        TestContext test, std::string label,
        std::source_location location = std::source_location::current())
        : test_(std::move(test)),
          label_(std::move(label)),
          expected_pending_label_(label_ + " expected pending"),
          actual_pending_label_(label_ + " actual pending"),
          expected_input_(*this, Side::Expected),
          actual_input_(*this, Side::Actual),
          location_(location) {}

    InOrderScoreboard(const InOrderScoreboard&) = delete;
    InOrderScoreboard& operator=(const InOrderScoreboard&) = delete;
    InOrderScoreboard(InOrderScoreboard&&) = delete;
    InOrderScoreboard& operator=(InOrderScoreboard&&) = delete;

    Input& expected() { return expected_input_; }
    Input& actual() { return actual_input_; }

    void write_expected(const T& value) {
        expected_.push_back(value);
        compare_available();
    }

    void write_actual(const T& value) {
        actual_.push_back(value);
        compare_available();
    }

    void finalize() {
        test_.expect_eq(expected_pending_label_, expected_.size(),
                        std::size_t{0}, location_);
        test_.expect_eq(actual_pending_label_, actual_.size(), std::size_t{0},
                        location_);
    }

    std::size_t compared() const { return compared_; }
    std::size_t expected_pending() const { return expected_.size(); }
    std::size_t actual_pending() const { return actual_.size(); }

   private:
    void compare_available() {
        while (!expected_.empty() && !actual_.empty()) {
            test_.expect_eq(label_, actual_.front(), expected_.front(),
                            location_);
            expected_.pop_front();
            actual_.pop_front();
            ++compared_;
        }
    }

    TestContext test_;
    std::string label_;
    std::string expected_pending_label_;
    std::string actual_pending_label_;
    std::deque<T> expected_;
    std::deque<T> actual_;
    std::size_t compared_ = 0;
    Input expected_input_;
    Input actual_input_;
    std::source_location location_;
};

template <typename ClockSignal, typename ValidSignal, typename ReadySignal,
          typename DataSignal>
class ReadyValidDriver {
   public:
    using value_type = typename DataSignal::value_type;

    ReadyValidDriver(ClockSignal clock, ValidSignal valid, ReadySignal ready,
                     DataSignal data, coro::SimTime sample_delay)
        : clock_(clock),
          valid_(valid),
          ready_(ready),
          data_(data),
          sample_delay_(sample_delay) {}

    coro::Task<uint32_t> send(value_type value) {
        uint32_t stalls = 0;
        while (true) {
            co_await coro::FallingEdge{static_cast<coro::Signal>(clock_)};
            data_.set(value);
            valid_.set(1);
            co_await coro::RisingEdge{static_cast<coro::Signal>(clock_)};
            if (sample_delay_.in_femtoseconds() != 0) {
                co_await coro::Delay{sample_delay_};
            }
            if (ready_.get() == 0) {
                ++stalls;
                continue;
            }
            co_await coro::FallingEdge{static_cast<coro::Signal>(clock_)};
            valid_.set(0);
            co_return stalls;
        }
    }

   private:
    ClockSignal clock_;
    ValidSignal valid_;
    ReadySignal ready_;
    DataSignal data_;
    coro::SimTime sample_delay_;
};

enum class ReadyValidSampleEdge {
    Rising,
    Falling,
};

template <typename ClockSignal, typename ValidSignal, typename ReadySignal,
          typename DataSignal>
class ReadyValidMonitor {
   public:
    using value_type = typename DataSignal::value_type;

    ReadyValidMonitor(ClockSignal clock, ValidSignal valid, ReadySignal ready,
                      DataSignal data, ReadyValidSampleEdge sample_edge,
                      coro::SimTime sample_delay)
        : clock_(clock),
          valid_(valid),
          ready_(ready),
          data_(data),
          sample_edge_(sample_edge),
          sample_delay_(sample_delay) {}

    coro::Task<void> run(AnalysisPort<value_type>& observed,
                         std::size_t transactions) {
        std::size_t received = 0;
        while (received < transactions) {
            if (sample_edge_ == ReadyValidSampleEdge::Rising) {
                co_await coro::RisingEdge{static_cast<coro::Signal>(clock_)};
            } else {
                co_await coro::FallingEdge{static_cast<coro::Signal>(clock_)};
            }
            if (sample_delay_.in_femtoseconds() != 0) {
                co_await coro::Delay{sample_delay_};
            }
            if (valid_.get() == 0 || ready_.get() == 0) continue;
            observed.write(data_.get());
            ++received;
        }
    }

   private:
    ClockSignal clock_;
    ValidSignal valid_;
    ReadySignal ready_;
    DataSignal data_;
    ReadyValidSampleEdge sample_edge_;
    coro::SimTime sample_delay_;
};

}  // namespace cpptb
