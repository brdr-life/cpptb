#include <cpptb/cpptb.hpp>

int main() {
    cpptb::Bits<65> value;
    value.set_word(0, 0x1234u);
    return value.word(0) == 0x1234u ? 0 : 1;
}
