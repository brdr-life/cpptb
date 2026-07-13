#include "examples/fifo_scoreboard/framework.hpp"

#include <cstdio>

namespace cpptb::examples::fifo_scoreboard {

void FifoScoreboardTb::expect_eq(const char* label, uint32_t actual,
                                 uint32_t expected) const {
    ++result_->checks;
    if (actual == expected) return;

    ++result_->failures;
    std::printf(
        "CPPTB_FIFO_SCOREBOARD_MISMATCH %s actual=0x%08x expected=0x%08x\n",
        label, actual, expected);
}

}  // namespace cpptb::examples::fifo_scoreboard
