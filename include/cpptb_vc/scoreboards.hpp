// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <concepts>
#include <cstddef>
#include <deque>
#include <functional>
#include <source_location>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "cpptb/test_api.hpp"
#include "cpptb_vc/ports.hpp"
#include "cpptb_vc/transaction_recording.hpp"

namespace cpptb::vc {

template <typename T>
concept ComparableTransaction =
    std::copy_constructible<T> && requires(const T& left, const T& right) {
        { left == right } -> std::convertible_to<bool>;
    };

template <ComparableTransaction T>
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

        template <typename Observation>
            requires TransactionObservationFor<Observation, T>
        void write(const Observation& observation) {
            if (observation.disposition ==
                TransactionDisposition::Completed) {
                write(observation.value);
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

template <ComparableTransaction T, typename KeyOf,
          typename Key = std::remove_cvref_t<
              std::invoke_result_t<KeyOf, const T&>>,
          typename Hash = std::hash<Key>>
    requires std::copy_constructible<Key> &&
             std::invocable<Hash, const Key&> &&
             std::equality_comparable<Key>
class KeyedScoreboard {
   private:
    enum class Side { Expected, Actual };
    using Pending = std::unordered_map<Key, std::deque<T>, Hash>;

    class Input {
       public:
        void write(const T& value) {
            if (side_ == Side::Expected) {
                owner_->write_expected(value);
            } else {
                owner_->write_actual(value);
            }
        }

        template <typename Observation>
            requires TransactionObservationFor<Observation, T>
        void write(const Observation& observation) {
            if (observation.disposition ==
                TransactionDisposition::Completed) {
                write(observation.value);
            }
        }

       private:
        friend class KeyedScoreboard;

        Input(KeyedScoreboard& owner, Side side)
            : owner_(std::addressof(owner)), side_(side) {}

        KeyedScoreboard* owner_;
        Side side_;
    };

   public:
    KeyedScoreboard(
        TestContext test, std::string label, KeyOf key_of, Hash hash = {},
        std::source_location location = std::source_location::current())
        : test_(std::move(test)),
          label_(std::move(label)),
          expected_pending_label_(label_ + " expected pending"),
          actual_pending_label_(label_ + " actual pending"),
          key_of_(std::move(key_of)),
          expected_(0, hash),
          actual_(0, std::move(hash)),
          expected_input_(*this, Side::Expected),
          actual_input_(*this, Side::Actual),
          location_(location) {}

    KeyedScoreboard(const KeyedScoreboard&) = delete;
    KeyedScoreboard& operator=(const KeyedScoreboard&) = delete;
    KeyedScoreboard(KeyedScoreboard&&) = delete;
    KeyedScoreboard& operator=(KeyedScoreboard&&) = delete;

    Input& expected() { return expected_input_; }
    Input& actual() { return actual_input_; }

    void write_expected(const T& value) {
        write(Side::Expected, value);
    }

    void write_actual(const T& value) { write(Side::Actual, value); }

    void finalize() {
        test_.expect_eq(expected_pending_label_, expected_pending_,
                        std::size_t{0}, location_);
        test_.expect_eq(actual_pending_label_, actual_pending_, std::size_t{0},
                        location_);
    }

    std::size_t compared() const { return compared_; }
    std::size_t expected_pending() const { return expected_pending_; }
    std::size_t actual_pending() const { return actual_pending_; }

   private:
    void write(Side side, const T& value) {
        const Key key = std::invoke(key_of_, value);
        Pending& own = side == Side::Expected ? expected_ : actual_;
        Pending& peer = side == Side::Expected ? actual_ : expected_;
        std::size_t& own_count = side == Side::Expected ? expected_pending_
                                                       : actual_pending_;
        std::size_t& peer_count = side == Side::Expected ? actual_pending_
                                                        : expected_pending_;

        const auto found = peer.find(key);
        if (found == peer.end() || found->second.empty()) {
            own[key].push_back(value);
            ++own_count;
            return;
        }

        const T peer_value = std::move(found->second.front());
        found->second.pop_front();
        --peer_count;
        if (found->second.empty()) peer.erase(found);

        if (side == Side::Expected) {
            test_.expect_eq(label_, peer_value, value, location_);
        } else {
            test_.expect_eq(label_, value, peer_value, location_);
        }
        ++compared_;
    }

    TestContext test_;
    std::string label_;
    std::string expected_pending_label_;
    std::string actual_pending_label_;
    KeyOf key_of_;
    Pending expected_;
    Pending actual_;
    std::size_t expected_pending_ = 0;
    std::size_t actual_pending_ = 0;
    std::size_t compared_ = 0;
    Input expected_input_;
    Input actual_input_;
    std::source_location location_;
};

template <typename Input, typename Model,
          typename Output =
              std::remove_cvref_t<std::invoke_result_t<Model&, const Input&>>>
    requires std::copy_constructible<Output> &&
             std::invocable<Model&, const Input&>
class ReferenceModelAdapter {
   public:
    explicit ReferenceModelAdapter(Model model) : model_(std::move(model)) {}

    void write(const Input& value) {
        predicted.write(std::invoke(model_, value));
    }

    template <typename Observation>
        requires TransactionObservationFor<Observation, Input>
    void write(const Observation& observation) {
        if (observation.disposition == TransactionDisposition::Completed) {
            write(observation.value);
        }
    }

    AnalysisPort<Output> predicted;

   private:
    Model model_;
};

template <typename Input, typename Model>
auto make_reference_model(Model model) {
    return ReferenceModelAdapter<Input, Model>{std::move(model)};
}

}  // namespace cpptb::vc
