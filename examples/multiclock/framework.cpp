#include "examples/multiclock/framework.hpp"

#include <cstdio>

namespace cpptb::examples::dpi_multiclock {

void MulticlockTb::expect_eq(const char* label, uint32_t actual,
                             uint32_t expected) const {
    ++result_->checks;
    if (actual == expected) return;

    ++result_->failures;
    std::printf("CPPTB_MULTICLOCK_MISMATCH %s actual=0x%08x expected=0x%08x\n",
                label, actual, expected);
}

}  // namespace cpptb::examples::dpi_multiclock
