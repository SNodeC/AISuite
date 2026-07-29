#include <core/State.h>
#include <cstdlib>

int main() {
    return core::eventLoopState() == core::State::LOADED ? EXIT_SUCCESS : EXIT_FAILURE;
}
