#include <cpptb/cpptb.hpp>
#include <cpptb/z3_random_backend.hpp>
#include <cpptb_vc/cpptb_vc.hpp>

int main() {
    cpptb::Bits<65> value;
    value.set_word(0, 0x1234u);
    cpptb::vc::AnalysisPort<uint32_t> observed;
    cpptb::Z3RandomBackend backend;
    return value.word(0) == 0x1234u && observed.subscriber_count() == 0 &&
                   backend.name() == "z3"
               ? 0
               : 1;
}
