#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "cpptb/coro_runtime.hpp"
#include "cpptb_vc/ports.hpp"

namespace cpptb::vc {

template <typename Source>
concept StreamSource = requires(Source& source,
                                typename Source::value_type value) {
    typename Source::send_result_type;
    { source.send(std::move(value)) } ->
        std::same_as<coro::Task<typename Source::send_result_type>>;
};

template <typename Sink>
concept StreamSink = requires(Sink& sink) {
    typename Sink::value_type;
    { sink.receive() } -> std::same_as<coro::Task<typename Sink::value_type>>;
};

template <typename ClockSignal, typename ValidSignal, typename ReadySignal,
          typename DataSignal>
class ReadyValidDriver {
   public:
    using value_type = typename DataSignal::value_type;
    using send_result_type = uint32_t;

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

template <typename ClockSignal, typename ValidSignal, typename ReadySignal,
          typename DataSignal>
class ReadyValidSink {
   public:
    using value_type = typename DataSignal::value_type;

    ReadyValidSink(ClockSignal clock, ValidSignal valid, ReadySignal ready,
                   DataSignal data, coro::SimTime sample_delay)
        : clock_(clock),
          valid_(valid),
          ready_(ready),
          data_(data),
          sample_delay_(sample_delay) {}

    coro::Task<value_type> receive() {
        while (true) {
            co_await coro::FallingEdge{static_cast<coro::Signal>(clock_)};
            ready_.set(1);
            co_await coro::RisingEdge{static_cast<coro::Signal>(clock_)};
            if (sample_delay_.in_femtoseconds() != 0) {
                co_await coro::Delay{sample_delay_};
            }
            if (valid_.get() == 0) continue;
            const value_type value = data_.get();
            co_await coro::FallingEdge{static_cast<coro::Signal>(clock_)};
            ready_.set(0);
            co_return value;
        }
    }

   private:
    ClockSignal clock_;
    ValidSignal valid_;
    ReadySignal ready_;
    DataSignal data_;
    coro::SimTime sample_delay_;
};

}  // namespace cpptb::vc
