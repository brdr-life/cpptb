#include "examples/counter/framework.hpp"

#include <cstdio>

namespace cpptb::examples::counter {

void CounterTb::expect_eq(const char* label, uint32_t actual,
                          uint32_t expected) const {
    ++result_->checks;
    if (actual == expected) return;

    ++result_->failures;
    std::printf("CPPTB_COUNTER_MISMATCH %s actual=%u expected=%u\n", label,
                actual, expected);
}

}  // namespace cpptb::examples::counter
