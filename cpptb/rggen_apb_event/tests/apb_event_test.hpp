#pragma once

#include <cstdint>

#include "cpptb/coro_runtime.hpp"
#include "cpptb/rggen_apb_event/apb_event_dut.hpp"

namespace cpptb::rggen_apb_event::tests {

void register_tests(coro::Testbench& tb, ApbEventDut dut);
int32_t done(coro::Testbench& tb, ApbEventDut dut);

}  // namespace cpptb::rggen_apb_event::tests
