#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#ifdef CPPTB_VERILATOR_TIMING_PROFILE
#include <cstdio>
#endif

#include "verilated.h"
#include "verilated_vpi.h"

#ifndef CPPTB_VERILATED_TOP
#error "CPPTB_VERILATED_TOP must name the generated Verilator model class"
#endif

#if defined(CPPTB_VERILATOR_DIRECT_TIMING) && \
    defined(CPPTB_VERILATOR_FULL_VPI_LOOP)
#error "direct timing dispatch cannot be combined with the full external VPI loop"
#endif

#define CPPTB_STRINGIFY_IMPL(Value) #Value
#define CPPTB_STRINGIFY(Value) CPPTB_STRINGIFY_IMPL(Value)
#include CPPTB_STRINGIFY(CPPTB_VERILATED_TOP.h)

extern "C" void (*vlog_startup_routines[])() VL_ATTR_WEAK;

#ifdef CPPTB_VERILATOR_DIRECT_TIMING
extern "C" unsigned int cpptb_verilator_pending_phases();
extern "C" void cpptb_verilator_dispatch_phase(unsigned int phase);

namespace {
constexpr uint32_t kReadWritePhase = 5;
constexpr uint32_t kReadOnlyPhase = 6;
constexpr uint32_t kNextTimeStepPhase = 7;

bool dispatch_if_pending(uint32_t phase) {
    if ((cpptb_verilator_pending_phases() & (1u << phase)) == 0) {
        return false;
    }
    cpptb_verilator_dispatch_phase(phase);
    return true;
}
}  // namespace
#endif

int main(int argc, char** argv, char**) {
    Verilated::debug(0);
    const auto context = std::make_unique<VerilatedContext>();
    context->threads(1);
    context->commandArgs(argc, argv);

    const auto top =
        std::make_unique<CPPTB_VERILATED_TOP>(context.get(), "");

    if (vlog_startup_routines) {
        for (auto routine = &vlog_startup_routines[0]; *routine; ++routine) {
            (*routine)();
        }
    }
    VerilatedVpi::callCbs(cbStartOfSimulation);

#ifdef CPPTB_VERILATOR_TIMING_PROFILE
    uint64_t time_steps = 0;
    uint64_t eval_calls = 0;
    uint64_t value_callback_hits = 0;
    uint64_t next_time_step_callback_hits = 0;
    uint64_t read_write_callback_hits = 0;
    uint64_t read_only_callback_hits = 0;
#endif

    while (VL_LIKELY(!context->gotFinish())) {
#ifdef CPPTB_VERILATOR_FULL_VPI_LOOP
        VerilatedVpi::callTimedCbs();
#endif
#ifdef CPPTB_VERILATOR_TIMING_PROFILE
        ++time_steps;
#ifdef CPPTB_VERILATOR_DIRECT_TIMING
        next_time_step_callback_hits +=
            dispatch_if_pending(kNextTimeStepPhase);
#else
        next_time_step_callback_hits += VerilatedVpi::callCbs(cbNextSimTime);
#endif
#else
#ifdef CPPTB_VERILATOR_DIRECT_TIMING
        dispatch_if_pending(kNextTimeStepPhase);
#else
        VerilatedVpi::callCbs(cbNextSimTime);
#endif
#endif
#ifdef CPPTB_VERILATOR_FULL_VPI_LOOP
        VerilatedVpi::callCbs(cbAtStartOfSimTime);
#endif

        bool read_write_called = false;
        do {
            VerilatedVpi::clearEvalNeeded();
            top->eval();
#ifdef CPPTB_VERILATOR_FULL_VPI_LOOP
#ifdef CPPTB_VERILATOR_TIMING_PROFILE
            ++eval_calls;
            value_callback_hits += VerilatedVpi::callValueCbs();
#else
            VerilatedVpi::callValueCbs();
#endif
            VerilatedVpi::callCbs(cbAtEndOfSimTime);
#else
#ifdef CPPTB_VERILATOR_TIMING_PROFILE
            ++eval_calls;
#endif
#endif
#ifdef CPPTB_VERILATOR_DIRECT_TIMING
            read_write_called = dispatch_if_pending(kReadWritePhase);
#else
            read_write_called = VerilatedVpi::callCbs(cbReadWriteSynch);
#endif
#ifdef CPPTB_VERILATOR_TIMING_PROFILE
            read_write_callback_hits += read_write_called;
#endif
        } while (read_write_called || VerilatedVpi::evalNeeded());

#ifdef CPPTB_VERILATOR_TIMING_PROFILE
#ifdef CPPTB_VERILATOR_DIRECT_TIMING
        read_only_callback_hits += dispatch_if_pending(kReadOnlyPhase);
#else
        read_only_callback_hits += VerilatedVpi::callCbs(cbReadOnlySynch);
#endif
#else
#ifdef CPPTB_VERILATOR_DIRECT_TIMING
        dispatch_if_pending(kReadOnlyPhase);
#else
        VerilatedVpi::callCbs(cbReadOnlySynch);
#endif
#endif

        uint64_t next_time = std::numeric_limits<uint64_t>::max();
        if (top->eventsPending()) next_time = top->nextTimeSlot();
#ifdef CPPTB_VERILATOR_FULL_VPI_LOOP
        next_time = std::min(next_time, VerilatedVpi::cbNextDeadline());
#endif
        if (next_time == std::numeric_limits<uint64_t>::max()) break;
        context->time(next_time);
    }

    top->final();
    VerilatedVpi::callCbs(cbEndOfSimulation);
#ifdef CPPTB_VERILATOR_TIMING_PROFILE
    std::printf(
        "CPPTB_VERILATOR_TIMING_PROFILE time_steps=%llu eval_calls=%llu "
        "value_cb_hits=%llu next_time_step_cb_hits=%llu "
        "read_write_cb_hits=%llu read_only_cb_hits=%llu\n",
        static_cast<unsigned long long>(time_steps),
        static_cast<unsigned long long>(eval_calls),
        static_cast<unsigned long long>(value_callback_hits),
        static_cast<unsigned long long>(next_time_step_callback_hits),
        static_cast<unsigned long long>(read_write_callback_hits),
        static_cast<unsigned long long>(read_only_callback_hits));
#endif
    context->statsPrintSummary();
    return 0;
}
