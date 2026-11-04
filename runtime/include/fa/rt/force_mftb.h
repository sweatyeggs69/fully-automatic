// Force-included into every recompiled ppc_recomp.*.cpp (see runtime/CMakeLists.txt).
//
// XenonRecomp emits mftb as `rN.u64 = __rdtsc();`. Reading the raw host TSC is wrong (the game
// expects 50 MHz), and patching the inlined rdtsc afterwards only works for the shapes the
// compiler happens to produce (it drops the `shl rdx,32` when only the low 32 bits are used).
// So every mftb becomes an out of line call that returns the rescaled timebase and no rdtsc
// is left in the recompiled code.
#pragma once
#include <x86intrin.h>
#include <cstdint>

extern "C" uint64_t fa_guest_mftb();

#undef __rdtsc
#define __rdtsc() fa_guest_mftb()
