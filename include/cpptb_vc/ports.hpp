#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "cpptb/coro_runtime.hpp"

namespace cpptb::vc {

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
                "cpptb-vc: AnalysisBuffer is full under the Error overflow "
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

}  // namespace cpptb::vc
