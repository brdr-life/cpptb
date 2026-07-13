#include "examples/apb_regfile/framework.hpp"

#include <cstdio>

namespace cpptb::examples::apb_regfile {

void ApbRegfileTb::expect_eq(const char* label, uint32_t actual,
                             uint32_t expected) const {
    ++result_->checks;
    if (actual == expected) return;

    ++result_->failures;
    std::printf(
        "CPPTB_APB_REGFILE_MISMATCH %s actual=0x%08x expected=0x%08x\n",
        label, actual, expected);
}

}  // namespace cpptb::examples::apb_regfile
