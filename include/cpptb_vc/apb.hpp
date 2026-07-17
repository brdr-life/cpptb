#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <functional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "cpptb/coro_runtime.hpp"
#include "cpptb/test_api.hpp"
#include "cpptb_vc/memory_mapped.hpp"
#include "cpptb_vc/ports.hpp"

namespace cpptb::vc {

struct NoApbStrobe {};

template <typename ClockSignal, typename SelectSignal, typename EnableSignal,
          typename WriteSignal, typename AddressSignal,
          typename WriteDataSignal, typename ReadDataSignal,
          typename ReadySignal, typename ErrorSignal,
          typename StrobeSignal = NoApbStrobe>
struct ApbBus {
    ClockSignal clock;
    SelectSignal select;
    EnableSignal enable;
    WriteSignal write;
    AddressSignal address;
    WriteDataSignal write_data;
    ReadDataSignal read_data;
    ReadySignal ready;
    ErrorSignal error;
    StrobeSignal strobe{};
};

template <typename ClockSignal, typename SelectSignal, typename EnableSignal,
          typename WriteSignal, typename AddressSignal,
          typename WriteDataSignal, typename ReadDataSignal,
          typename ReadySignal, typename ErrorSignal>
ApbBus(ClockSignal, SelectSignal, EnableSignal, WriteSignal, AddressSignal,
       WriteDataSignal, ReadDataSignal, ReadySignal, ErrorSignal)
    -> ApbBus<ClockSignal, SelectSignal, EnableSignal, WriteSignal,
              AddressSignal, WriteDataSignal, ReadDataSignal, ReadySignal,
              ErrorSignal>;

template <typename ClockSignal, typename SelectSignal, typename EnableSignal,
          typename WriteSignal, typename AddressSignal,
          typename WriteDataSignal, typename ReadDataSignal,
          typename ReadySignal, typename ErrorSignal, typename StrobeSignal>
ApbBus(ClockSignal, SelectSignal, EnableSignal, WriteSignal, AddressSignal,
       WriteDataSignal, ReadDataSignal, ReadySignal, ErrorSignal,
       StrobeSignal)
    -> ApbBus<ClockSignal, SelectSignal, EnableSignal, WriteSignal,
              AddressSignal, WriteDataSignal, ReadDataSignal, ReadySignal,
              ErrorSignal, StrobeSignal>;

struct ApbConfig {
    coro::SimTime sample_delay{};
    uint32_t max_wait_cycles = 0;
};

namespace apb_detail {

template <typename Strobe>
struct ByteEnableType {
    using type = typename Strobe::value_type;
};

template <>
struct ByteEnableType<NoApbStrobe> {
    using type = uint64_t;
};

template <typename Bus>
using ByteEnable = typename ByteEnableType<
    std::remove_cvref_t<decltype(std::declval<Bus>().strobe)>>::type;

template <typename Strobe>
constexpr auto all_bytes() {
    using value_type = typename ByteEnableType<Strobe>::type;
    if constexpr (std::same_as<Strobe, NoApbStrobe> ||
                  !requires { Strobe::width; }) {
        return std::numeric_limits<value_type>::max();
    } else if constexpr (std::unsigned_integral<value_type>) {
        constexpr auto digits = std::numeric_limits<value_type>::digits;
        if constexpr (Strobe::width >= digits) {
            return std::numeric_limits<value_type>::max();
        } else {
            return static_cast<value_type>(
                (value_type{1} << Strobe::width) - value_type{1});
        }
    } else {
        typename value_type::word_array words{};
        words.fill(std::numeric_limits<uint32_t>::max());
        return value_type::from_words(words);
    }
}

class LockGuard {
   public:
    explicit LockGuard(coro::Lock& lock) noexcept : lock_(&lock) {}
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
    ~LockGuard() {
        if (lock_) lock_->release();
    }

   private:
    coro::Lock* lock_;
};

}  // namespace apb_detail

template <typename ClockSignal, typename SelectSignal, typename EnableSignal,
          typename WriteSignal, typename AddressSignal,
          typename WriteDataSignal, typename ReadDataSignal,
          typename ReadySignal, typename ErrorSignal, typename StrobeSignal>
class ApbMaster {
   public:
    using bus_type =
        ApbBus<ClockSignal, SelectSignal, EnableSignal, WriteSignal,
               AddressSignal, WriteDataSignal, ReadDataSignal, ReadySignal,
               ErrorSignal, StrobeSignal>;
    using address_type = typename AddressSignal::value_type;
    using data_type = typename WriteDataSignal::value_type;
    using byte_enable_type =
        typename apb_detail::ByteEnableType<StrobeSignal>::type;
    using write_request_type =
        MemoryWriteRequest<address_type, data_type, byte_enable_type>;
    using read_request_type = MemoryReadRequest<address_type>;
    using write_response_type = MemoryWriteResponse;
    using read_response_type = MemoryReadResponse<data_type>;
    using transaction_type =
        MemoryTransaction<address_type, data_type, byte_enable_type>;

    explicit ApbMaster(bus_type bus, ApbConfig config = {})
        : bus_(std::move(bus)), config_(config) {}

    ApbMaster(const ApbMaster&) = delete;
    ApbMaster& operator=(const ApbMaster&) = delete;

    static constexpr bool supports_byte_enable =
        !std::same_as<StrobeSignal, NoApbStrobe>;
    static constexpr byte_enable_type all_bytes() {
        return apb_detail::all_bytes<StrobeSignal>();
    }

    coro::Task<write_response_type> write(write_request_type request) {
        if constexpr (std::same_as<StrobeSignal, NoApbStrobe>) {
            if (request.byte_enable != all_bytes()) {
                throw std::invalid_argument(
                    "cpptb-vc: APB partial write requires a PSTRB signal");
            }
        }
        co_await lock_.acquire();
        apb_detail::LockGuard guard{lock_};

        co_await coro::FallingEdge{static_cast<coro::Signal>(bus_.clock)};
        bus_.address.set(request.address);
        bus_.write_data.set(request.data);
        set_strobe(request.byte_enable);
        bus_.write.set(1);
        bus_.select.set(1);
        bus_.enable.set(0);

        co_await coro::RisingEdge{static_cast<coro::Signal>(bus_.clock)};
        co_await coro::FallingEdge{static_cast<coro::Signal>(bus_.clock)};
        bus_.enable.set(1);

        const auto completion = co_await wait_for_completion();
        co_await coro::FallingEdge{static_cast<coro::Signal>(bus_.clock)};
        idle();
        co_return write_response_type{completion.status,
                                      completion.wait_cycles};
    }

    coro::Task<write_response_type> write(
        address_type address, data_type data,
        byte_enable_type byte_enable = all_bytes()) {
        return write(write_request_type{address, data, byte_enable});
    }

    coro::Task<read_response_type> read(read_request_type request) {
        co_await lock_.acquire();
        apb_detail::LockGuard guard{lock_};

        co_await coro::FallingEdge{static_cast<coro::Signal>(bus_.clock)};
        bus_.address.set(request.address);
        set_strobe(byte_enable_type{});
        bus_.write.set(0);
        bus_.select.set(1);
        bus_.enable.set(0);

        co_await coro::RisingEdge{static_cast<coro::Signal>(bus_.clock)};
        co_await coro::FallingEdge{static_cast<coro::Signal>(bus_.clock)};
        bus_.enable.set(1);

        const auto completion = co_await wait_for_completion();
        const data_type data = completion.status == MemoryStatus::Timeout
                                   ? data_type{}
                                   : bus_.read_data.get();
        co_await coro::FallingEdge{static_cast<coro::Signal>(bus_.clock)};
        idle();
        co_return read_response_type{data, completion.status,
                                     completion.wait_cycles};
    }

    coro::Task<read_response_type> read(address_type address) {
        return read(read_request_type{address});
    }

    void idle() {
        bus_.select.set(0);
        bus_.enable.set(0);
        bus_.write.set(0);
    }

   private:
    struct Completion {
        MemoryStatus status;
        uint32_t wait_cycles;
    };

    coro::Task<Completion> wait_for_completion() {
        uint32_t wait_cycles = 0;
        while (true) {
            co_await coro::RisingEdge{static_cast<coro::Signal>(bus_.clock)};
            if (config_.sample_delay.in_femtoseconds() != 0) {
                co_await coro::Delay{config_.sample_delay};
            }
            if (bus_.ready.get() != 0) {
                co_return Completion{
                    bus_.error.get() == 0 ? MemoryStatus::Okay
                                          : MemoryStatus::SlaveError,
                    wait_cycles};
            }
            ++wait_cycles;
            if (config_.max_wait_cycles != 0 &&
                wait_cycles >= config_.max_wait_cycles) {
                co_return Completion{MemoryStatus::Timeout, wait_cycles};
            }
        }
    }

    void set_strobe(byte_enable_type byte_enable) {
        if constexpr (!std::same_as<StrobeSignal, NoApbStrobe>) {
            bus_.strobe.set(byte_enable);
        }
    }

    bus_type bus_;
    ApbConfig config_;
    coro::Lock lock_;
};

template <typename... Signals>
ApbMaster(ApbBus<Signals...>, ApbConfig = {}) -> ApbMaster<Signals...>;

template <typename Bus>
class ApbMonitor;

template <typename ClockSignal, typename SelectSignal, typename EnableSignal,
          typename WriteSignal, typename AddressSignal,
          typename WriteDataSignal, typename ReadDataSignal,
          typename ReadySignal, typename ErrorSignal, typename StrobeSignal>
class ApbMonitor<ApbBus<ClockSignal, SelectSignal, EnableSignal, WriteSignal,
                        AddressSignal, WriteDataSignal, ReadDataSignal,
                        ReadySignal, ErrorSignal, StrobeSignal>> {
   public:
    using bus_type =
        ApbBus<ClockSignal, SelectSignal, EnableSignal, WriteSignal,
               AddressSignal, WriteDataSignal, ReadDataSignal, ReadySignal,
               ErrorSignal, StrobeSignal>;
    using address_type = typename AddressSignal::value_type;
    using data_type = typename WriteDataSignal::value_type;
    using byte_enable_type =
        typename apb_detail::ByteEnableType<StrobeSignal>::type;
    using transaction_type =
        MemoryTransaction<address_type, data_type, byte_enable_type>;

    explicit ApbMonitor(bus_type bus, coro::SimTime sample_delay = {})
        : bus_(std::move(bus)), sample_delay_(sample_delay) {}

    coro::Task<void> run(AnalysisPort<transaction_type>& observed,
                         std::size_t transactions) {
        std::size_t completed = 0;
        while (completed < transactions) {
            if (co_await sample(observed)) ++completed;
        }
    }

    coro::Task<void> run_forever(AnalysisPort<transaction_type>& observed) {
        while (true) static_cast<void>(co_await sample(observed));
    }

   private:
    coro::Task<bool> sample(AnalysisPort<transaction_type>& observed) {
        co_await coro::RisingEdge{static_cast<coro::Signal>(bus_.clock)};
        if (sample_delay_.in_femtoseconds() != 0) {
            co_await coro::Delay{sample_delay_};
        }

        const bool selected = bus_.select.get() != 0;
        const bool enabled = bus_.enable.get() != 0;
        if (!selected) {
            active_ = false;
            wait_cycles_ = 0;
            co_return false;
        }
        if (!enabled) {
            active_ = true;
            wait_cycles_ = 0;
            co_return false;
        }
        if (bus_.ready.get() == 0) {
            if (active_) ++wait_cycles_;
            co_return false;
        }

        const bool write = bus_.write.get() != 0;
        observed.write(transaction_type{
            .operation = write ? MemoryOperation::Write
                               : MemoryOperation::Read,
            .address = bus_.address.get(),
            .data = write ? bus_.write_data.get() : bus_.read_data.get(),
            .byte_enable =
                write ? strobe()
                      : apb_detail::all_bytes<StrobeSignal>(),
            .status = bus_.error.get() == 0 ? MemoryStatus::Okay
                                            : MemoryStatus::SlaveError,
            .wait_cycles = wait_cycles_,
        });
        active_ = false;
        wait_cycles_ = 0;
        co_return true;
    }

    byte_enable_type strobe() const {
        if constexpr (std::same_as<StrobeSignal, NoApbStrobe>) {
            return apb_detail::all_bytes<StrobeSignal>();
        } else {
            return bus_.strobe.get();
        }
    }

    bus_type bus_;
    coro::SimTime sample_delay_;
    uint32_t wait_cycles_ = 0;
    bool active_ = false;
};

template <typename... Signals>
ApbMonitor(ApbBus<Signals...>, coro::SimTime = {})
    -> ApbMonitor<ApbBus<Signals...>>;

template <typename Bus>
class ApbProtocolChecker;

template <typename ClockSignal, typename SelectSignal, typename EnableSignal,
          typename WriteSignal, typename AddressSignal,
          typename WriteDataSignal, typename ReadDataSignal,
          typename ReadySignal, typename ErrorSignal, typename StrobeSignal>
class ApbProtocolChecker<
    ApbBus<ClockSignal, SelectSignal, EnableSignal, WriteSignal, AddressSignal,
           WriteDataSignal, ReadDataSignal, ReadySignal, ErrorSignal,
           StrobeSignal>> {
   public:
    using bus_type =
        ApbBus<ClockSignal, SelectSignal, EnableSignal, WriteSignal,
               AddressSignal, WriteDataSignal, ReadDataSignal, ReadySignal,
               ErrorSignal, StrobeSignal>;
    using address_type = typename AddressSignal::value_type;
    using data_type = typename WriteDataSignal::value_type;
    using byte_enable_type =
        typename apb_detail::ByteEnableType<StrobeSignal>::type;

    ApbProtocolChecker(TestContext test, bus_type bus,
                       coro::SimTime sample_delay = {})
        : test_(std::move(test)),
          bus_(std::move(bus)),
          sample_delay_(sample_delay) {}

    coro::Task<void> run_cycles(std::size_t cycles) {
        for (std::size_t cycle = 0; cycle < cycles; ++cycle) {
            co_await sample();
        }
    }

    coro::Task<void> run_forever() {
        while (true) co_await sample();
    }

    template <typename Stop>
        requires std::invocable<Stop&> &&
                 std::convertible_to<std::invoke_result_t<Stop&>, bool>
    coro::Task<void> run_until(Stop stop) {
        while (!std::invoke(stop)) co_await sample();
    }

    uint64_t violations() const noexcept { return violations_; }

   private:
    coro::Task<void> sample() {
        co_await coro::RisingEdge{static_cast<coro::Signal>(bus_.clock)};
        if (sample_delay_.in_femtoseconds() != 0) {
            co_await coro::Delay{sample_delay_};
        }

        const bool selected = bus_.select.get() != 0;
        const bool enabled = bus_.enable.get() != 0;
        const bool ready = bus_.ready.get() != 0;
        const bool access = selected && enabled;

        report(!enabled || selected, "APB PENABLE requires PSEL");
        report(!access || setup_seen_ || waiting_,
               "APB access phase requires a setup phase");

        if (waiting_) {
            report(selected, "APB PSEL dropped before PREADY");
            report(enabled, "APB PENABLE dropped before PREADY");
            report(bus_.address.get() == address_,
                   "APB PADDR changed while waiting for PREADY");
            report(bus_.write.get() == write_,
                   "APB PWRITE changed while waiting for PREADY");
            report(!static_cast<bool>(write_) ||
                       bus_.write_data.get() == write_data_,
                   "APB PWDATA changed while waiting for PREADY");
            report(strobe() == strobe_,
                   "APB PSTRB changed while waiting for PREADY");
        }

        if (selected && !enabled) {
            if constexpr (!std::same_as<StrobeSignal, NoApbStrobe>) {
                report(bus_.write.get() != 0 ||
                           strobe() == byte_enable_type{},
                       "APB PSTRB must be zero during reads");
            }
            capture_control();
            setup_seen_ = true;
            waiting_ = false;
        } else if (access) {
            if (setup_seen_) {
                report(bus_.address.get() == address_,
                       "APB PADDR changed between setup and access");
                report(bus_.write.get() == write_,
                       "APB PWRITE changed between setup and access");
                report(!static_cast<bool>(write_) ||
                           bus_.write_data.get() == write_data_,
                       "APB PWDATA changed between setup and access");
                report(strobe() == strobe_,
                       "APB PSTRB changed between setup and access");
            }
            waiting_ = !ready;
            setup_seen_ = false;
        } else {
            waiting_ = false;
            setup_seen_ = false;
        }
    }

    void capture_control() {
        address_ = bus_.address.get();
        write_ = bus_.write.get();
        write_data_ = bus_.write_data.get();
        strobe_ = strobe();
    }

    byte_enable_type strobe() const {
        if constexpr (std::same_as<StrobeSignal, NoApbStrobe>) {
            return apb_detail::all_bytes<StrobeSignal>();
        } else {
            return bus_.strobe.get();
        }
    }

    void report(bool condition, const char* message) {
        if (condition) return;
        ++violations_;
        test_.expect(message, false);
    }

    TestContext test_;
    bus_type bus_;
    coro::SimTime sample_delay_;
    address_type address_{};
    data_type write_data_{};
    byte_enable_type strobe_{};
    typename WriteSignal::value_type write_{};
    uint64_t violations_ = 0;
    bool setup_seen_ = false;
    bool waiting_ = false;
};

template <typename... Signals>
ApbProtocolChecker(TestContext, ApbBus<Signals...>, coro::SimTime = {})
    -> ApbProtocolChecker<ApbBus<Signals...>>;

}  // namespace cpptb::vc
