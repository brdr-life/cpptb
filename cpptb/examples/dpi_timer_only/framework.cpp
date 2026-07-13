#include "cpptb/examples/dpi_timer_only/framework.hpp"

#include <cstdio>

namespace cpptb::examples::dpi_timer_only {

void TimerOnlyTb::expect_eq(const char* label, uint64_t actual,
                            uint64_t expected) const {
    ++result_->checks;
    if (actual == expected) return;

    ++result_->failures;
    std::printf(
        "CPP_DPI_TIMER_ONLY_MISMATCH %s actual=0x%016llx expected=0x%016llx\n",
        label, static_cast<unsigned long long>(actual),
        static_cast<unsigned long long>(expected));
}

}  // namespace cpptb::examples::dpi_timer_only
