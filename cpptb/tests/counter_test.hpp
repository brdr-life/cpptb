#pragma once

#include <cstdint>

#include "cpptb/runtime.hpp"

namespace cpptb::counter_test {

uint32_t process_count();
void setup(CounterDut dut);
WaitRequest process_start(CounterDut dut, uint32_t process_id);
WaitRequest process_resume(CounterDut dut, uint32_t process_id, uint64_t time,
                           uint32_t phase);
int32_t done(CounterDut dut);

}  // namespace cpptb::counter_test
