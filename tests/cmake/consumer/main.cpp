#include <cpptb/cpptb.hpp>
#include <cpptb_vc/cpptb_vc.hpp>

#ifdef CPPTB_CONSUMER_WITH_Z3
#include <cpptb/z3_random_backend.hpp>
#endif

int main() {
    cpptb::Bits<65> value;
    value.set_word(0, 0x1234u);
    cpptb::vc::AnalysisPort<uint32_t> observed;

    bool ok = value.word(0) == 0x1234u && observed.subscriber_count() == 0;

#ifdef CPPTB_CONSUMER_WITH_Z3
    cpptb::Z3RandomBackend backend;
    ok = ok && backend.name() == "z3";
#endif

    return ok ? 0 : 1;
}
