#pragma once

#include <cstdint>

namespace cpptb {

struct TestResult {
    uint64_t checks = 0;
    uint32_t failures = 0;
};

}  // namespace cpptb
