#include "experiments/cpp_vpi/counter/counter_test.hpp"

#include <cstdint>
#include <cstdio>

namespace cpptb::counter_test {

constexpr uint32_t kProcessResetDriver = 0;
constexpr uint32_t kProcessEnableDriver = 1;
constexpr uint32_t kProcessCountMonitor = 2;
constexpr uint32_t kProcessCount = 3;

constexpr uint32_t kPhaseRun = 0;

struct ResetDriver {
    CounterDut dut;
    Scheduler scheduler;

    WaitRequest start() {
        dut.rst.set(1);
        std::printf("cpp reset: rst=1, wait one clock\n");
        return scheduler.wait(RisingEdge{dut.clk}, kPhaseRun);
    }

    WaitRequest resume(uint64_t time) {
        dut.rst.set(0);
        std::printf("cpp reset: time=%llu rst=0, done\n",
                    static_cast<unsigned long long>(time));
        return scheduler.finish();
    }
};

struct EnableDriver {
    CounterDut dut;
    Scheduler scheduler;

    WaitRequest start() {
        dut.en.set(0);
        std::printf("cpp enable: en=0, waiting for reset release\n");
        return scheduler.wait(RisingEdge{dut.clk}, kPhaseRun);
    }

    WaitRequest resume(uint64_t time) {
        const auto rst_value = dut.rst.get();
        const auto count_value = dut.count.get();

        if (rst_value != 0) {
            dut.en.set(0);
            std::printf("cpp enable: time=%llu rst=1, keep en=0\n",
                        static_cast<unsigned long long>(time));
            return scheduler.wait(RisingEdge{dut.clk}, kPhaseRun);
        }

        if (count_value < 5) {
            dut.en.set(1);
            std::printf("cpp enable: time=%llu count=%u drive en=1\n",
                        static_cast<unsigned long long>(time), count_value);
            return scheduler.wait(RisingEdge{dut.clk}, kPhaseRun);
        }

        dut.en.set(0);
        std::printf("cpp enable: time=%llu count=%u drive en=0, done\n",
                    static_cast<unsigned long long>(time), count_value);
        return scheduler.finish();
    }
};

struct CountMonitor {
    CounterDut dut;
    Scheduler scheduler;

    WaitRequest start() {
        std::printf("cpp monitor: waiting for count activity\n");
        return scheduler.wait(RisingEdge{dut.clk}, kPhaseRun);
    }

    WaitRequest resume(uint64_t time) {
        const auto count_value = dut.count.get();
        std::printf("cpp monitor: time=%llu count=%u\n",
                    static_cast<unsigned long long>(time), count_value);

        if (count_value >= 5) {
            std::printf("cpp monitor: observed target count, done\n");
            return scheduler.finish();
        }

        return scheduler.wait(RisingEdge{dut.clk}, kPhaseRun);
    }
};

uint32_t process_count() { return kProcessCount; }

void setup(CounterDut dut) {
    dut.clk.set(0);
    dut.rst.set(1);
    dut.en.set(0);
    std::printf("cpp tb: setup clk=0 rst=1 en=0\n");
}

WaitRequest process_start(CounterDut dut, uint32_t process_id) {
    const Scheduler scheduler{kPhaseRun};

    if (process_id == kProcessResetDriver) {
        return ResetDriver{dut, scheduler}.start();
    }

    if (process_id == kProcessEnableDriver) {
        return EnableDriver{dut, scheduler}.start();
    }

    if (process_id == kProcessCountMonitor) {
        return CountMonitor{dut, scheduler}.start();
    }

    std::printf("cpp tb: unknown process id=%u\n", process_id);
    return scheduler.finish();
}

WaitRequest process_resume(CounterDut dut, uint32_t process_id, uint64_t time,
                           uint32_t phase) {
    const Scheduler scheduler{phase};

    if (process_id == kProcessResetDriver) {
        return ResetDriver{dut, scheduler}.resume(time);
    }

    if (process_id == kProcessEnableDriver) {
        return EnableDriver{dut, scheduler}.resume(time);
    }

    if (process_id == kProcessCountMonitor) {
        return CountMonitor{dut, scheduler}.resume(time);
    }

    std::printf("cpp tb: unknown process id=%u\n", process_id);
    return scheduler.finish();
}

int32_t done(CounterDut dut) {
    const auto final_count = dut.count.get();
    if (final_count == 5) {
        std::printf("cpp tb: PASS final_count=%u\n", final_count);
        return 0;
    }

    std::printf("cpp tb: FAIL final_count=%u\n", final_count);
    return 1;
}

}  // namespace cpptb::counter_test
