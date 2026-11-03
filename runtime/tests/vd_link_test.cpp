// Pulls imports_vd.cpp (and through it the GPU command processor) into an executable.
#include "fa/rt/import.hpp"

PPC_FUNC(__imp__VdInitializeRingBuffer);

int main() {
    volatile auto f = &__imp__VdInitializeRingBuffer;
    return f == nullptr;
}
